"""ELF dynamic metadata inside a module image: .dynamic, dynsym, RELA/RELR relocations, eh_frame_hdr + FDEs.

All addresses are module-relative offsets (modules are linked at 0).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .nso import NsoImage

DT_NULL, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ, DT_RELAENT = 0, 2, 4, 5, 6, 7, 8, 9
DT_STRSZ, DT_SYMENT, DT_INIT, DT_FINI, DT_JMPREL = 10, 11, 12, 13, 23
DT_INIT_ARRAY, DT_FINI_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 25, 26, 27, 28
DT_RELRSZ, DT_RELR, DT_RELRENT = 35, 36, 37
DT_ANDROID_RELR, DT_ANDROID_RELRSZ = 0x6FFFE000, 0x6FFFE001
DT_GNU_HASH = 0x6FFFFEF5

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027

STT_FUNC = 2
SHN_UNDEF = 0


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    type: int
    bind: int
    shndx: int


@dataclass
class Reloc:
    offset: int
    type: int
    sym: int
    addend: int


@dataclass
class Fde:
    start: int
    size: int


@dataclass
class DynInfo:
    tags: dict[int, int] = field(default_factory=dict)
    symbols: list[Symbol] = field(default_factory=list)
    relocs: list[Reloc] = field(default_factory=list)      # RELA + JMPREL
    relr: list[int] = field(default_factory=list)          # offsets patched with base + *(u64 *)offset
    fdes: list[Fde] = field(default_factory=list)
    init_array: list[int] = field(default_factory=list)
    fini_array: list[int] = field(default_factory=list)

    def relocated_words(self, im: NsoImage) -> dict[int, int]:
        """Offset -> module-relative target for every word the loader rebases (RELATIVE, RELR, defined ABS64)."""
        out: dict[int, int] = {}
        for off in self.relr:
            if im.in_image(off, 8):
                out[off] = im.u64(off)
        for r in self.relocs:
            if r.type == R_AARCH64_RELATIVE:
                out[r.offset] = r.addend & 0xFFFFFFFFFFFFFFFF
            elif r.type in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT) and r.sym < len(self.symbols):
                s = self.symbols[r.sym]
                if s.shndx != SHN_UNDEF and s.value:
                    out[r.offset] = (s.value + r.addend) & 0xFFFFFFFFFFFFFFFF
        return out


def _cstr(data: bytes, off: int) -> str:
    end = data.find(b'\0', off)
    return data[off:end if end >= 0 else len(data)].decode('utf-8', 'replace')


def _gnu_hash_nsyms(im: NsoImage, off: int) -> int:
    nbuckets, symoffset, bloom_size, _ = struct.unpack_from('<IIII', im.data, off)
    buckets_off = off + 16 + bloom_size * 8
    buckets = struct.unpack_from(f'<{nbuckets}I', im.data, buckets_off)
    last = max(buckets) if buckets else 0
    if last < symoffset:
        return symoffset
    chain_off = buckets_off + nbuckets * 4
    while not (im.u32(chain_off + (last - symoffset) * 4) & 1):
        last += 1
    return last + 1


def _decode_relr(im: NsoImage, off: int, size: int) -> list[int]:
    out: list[int] = []
    where = 0
    for i in range(0, size, 8):
        entry = im.u64(off + i)
        if not entry & 1:
            out.append(entry)
            where = entry + 8
        else:
            bits = entry >> 1
            j = 0
            while bits:
                if bits & 1:
                    out.append(where + j * 8)
                bits >>= 1
                j += 1
            where += 63 * 8
    return out


def _read_encoded(data: bytes, off: int, enc: int, pc: int, datarel: int) -> tuple[int, int]:
    """DWARF EH pointer: returns (value, bytes consumed). pc = module offset of the field."""
    fmt = enc & 0x0F
    if fmt == 0x00:
        v, n = struct.unpack_from('<Q', data, off)[0], 8
    elif fmt == 0x01:      # uleb128
        v, n = _uleb(data, off)
    elif fmt == 0x02:
        v, n = struct.unpack_from('<H', data, off)[0], 2
    elif fmt == 0x03:
        v, n = struct.unpack_from('<I', data, off)[0], 4
    elif fmt == 0x04:
        v, n = struct.unpack_from('<Q', data, off)[0], 8
    elif fmt == 0x09:      # sleb128
        v, n = _sleb(data, off)
    elif fmt == 0x0A:
        v, n = struct.unpack_from('<h', data, off)[0], 2
    elif fmt == 0x0B:
        v, n = struct.unpack_from('<i', data, off)[0], 4
    elif fmt == 0x0C:
        v, n = struct.unpack_from('<q', data, off)[0], 8
    else:
        raise ValueError(f'unsupported pointer encoding {enc:#x}')
    app = enc & 0x70
    if app == 0x10:
        v += pc
    elif app == 0x30:
        v += datarel
    return v & 0xFFFFFFFFFFFFFFFF, n


def _uleb(data: bytes, off: int) -> tuple[int, int]:
    v = shift = n = 0
    while True:
        b = data[off + n]
        n += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return v, n


def _sleb(data: bytes, off: int) -> tuple[int, int]:
    v, n = _uleb(data, off)
    if data[off + n - 1] & 0x40:
        v -= 1 << (7 * n)
    return v, n


def _cie_fde_encoding(data: bytes, cie: int) -> int:
    """FDE pointer encoding from a CIE's 'z' augmentation (default absptr)."""
    length = struct.unpack_from('<I', data, cie)[0]
    p = cie + 8
    if length == 0xFFFFFFFF:
        return 0
    p += 1                                  # version
    end = data.index(b'\0', p)
    aug = data[p:end].decode('ascii', 'replace')
    p = end + 1
    if 'eh' in aug:
        p += 8
    _, n = _uleb(data, p); p += n          # code alignment
    _, n = _sleb(data, p); p += n          # data alignment
    version = data[cie + 8]
    if version == 1:
        p += 1
    else:
        _, n = _uleb(data, p); p += n      # return address register
    if not aug.startswith('z'):
        return 0
    _, n = _uleb(data, p); p += n          # augmentation length
    for ch in aug[1:]:
        if ch == 'R':
            return data[p]
        if ch == 'P':
            penc = data[p]
            p += 1
            _, n = _read_encoded(data, p, penc & 0x7F, p, 0)
            p += n
        elif ch in 'LB':
            p += 1
        elif ch == 'S':
            pass
        else:
            break
    return 0


def _parse_eh_frame_hdr(im: NsoImage, start: int, end: int) -> list[Fde]:
    d = im.data
    version, ptr_enc, cnt_enc, tbl_enc = d[start:start + 4]
    if version != 1 or tbl_enc == 0xFF:
        return []
    p = start + 4
    _, n = _read_encoded(d, p, ptr_enc, p, start); p += n
    count, n = _read_encoded(d, p, cnt_enc, p, start); p += n
    fdes: list[Fde] = []
    cie_enc: dict[int, int] = {}
    for _ in range(count):
        loc, n = _read_encoded(d, p, tbl_enc, p, start); p += n
        fde, n = _read_encoded(d, p, tbl_enc, p, start); p += n
        if not im.in_image(fde, 16):
            continue
        length = struct.unpack_from('<I', d, fde)[0]
        if length in (0, 0xFFFFFFFF):
            continue
        cie_ptr = struct.unpack_from('<I', d, fde + 4)[0]
        cie = fde + 4 - cie_ptr
        if cie not in cie_enc:
            try:
                cie_enc[cie] = _cie_fde_encoding(d, cie)
            except (ValueError, IndexError, struct.error):
                cie_enc[cie] = 0x1B
        enc = cie_enc[cie]
        q = fde + 8
        pc_begin, n = _read_encoded(d, q, enc, q, start); q += n
        pc_range, _ = _read_encoded(d, q, enc & 0x0F, q, start)
        if pc_begin != loc:
            pc_begin = loc
        fdes.append(Fde(pc_begin, pc_range))
    return fdes


def parse_dynamic(im: NsoImage) -> DynInfo:
    info = DynInfo()
    if im.dynamic_off is not None and im.in_image(im.dynamic_off, 16):
        off = im.dynamic_off
        while im.in_image(off, 16):
            tag, val = struct.unpack_from('<qQ', im.data, off)
            off += 16
            if tag == DT_NULL:
                break
            info.tags.setdefault(tag, val)
    t = info.tags

    if DT_SYMTAB in t and DT_STRTAB in t:
        syment = t.get(DT_SYMENT, 24)
        if DT_HASH in t:
            nsyms = im.u32(t[DT_HASH] + 4)
        elif DT_GNU_HASH in t:
            nsyms = _gnu_hash_nsyms(im, t[DT_GNU_HASH])
        else:
            nsyms = max(0, (t[DT_STRTAB] - t[DT_SYMTAB]) // syment)
        strtab = t[DT_STRTAB]
        for i in range(nsyms):
            so = t[DT_SYMTAB] + i * syment
            if not im.in_image(so, 24):
                break
            st_name, st_info, _, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', im.data, so)
            info.symbols.append(Symbol(_cstr(im.data, strtab + st_name), st_value, st_size, st_info & 0xF,
                                       st_info >> 4, st_shndx))

    for tag_off, tag_sz in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
        if tag_off in t and tag_sz in t:
            for i in range(0, t[tag_sz], 24):
                r_off, r_info, r_add = struct.unpack_from('<QQq', im.data, t[tag_off] + i)
                info.relocs.append(Reloc(r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add))
    for tag_off, tag_sz in ((DT_RELR, DT_RELRSZ), (DT_ANDROID_RELR, DT_ANDROID_RELRSZ)):
        if tag_off in t and tag_sz in t:
            info.relr.extend(_decode_relr(im, t[tag_off], t[tag_sz]))

    words = info.relocated_words(im)
    for tag_arr, tag_sz, dst in ((DT_INIT_ARRAY, DT_INIT_ARRAYSZ, info.init_array),
                                 (DT_FINI_ARRAY, DT_FINI_ARRAYSZ, info.fini_array)):
        if tag_arr in t and tag_sz in t:
            for i in range(0, t[tag_sz], 8):
                o = t[tag_arr] + i
                dst.append(words.get(o, im.u64(o) if im.in_image(o, 8) else 0))

    if im.eh_frame_hdr:
        try:
            info.fdes = _parse_eh_frame_hdr(im, *im.eh_frame_hdr)
        except (ValueError, IndexError, struct.error):
            info.fdes = []
    return info
