"""NSO container parsing and flat module images.

An NSO holds three segments (text, ro, data), each optionally LZ4-compressed, plus a bss size. Loading lays them out
at their memory offsets in one flat little-endian image that starts at offset 0 (the module is linked at 0 and
relocated by the runtime). See https://switchbrew.org/wiki/NSO.
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field

import lz4.block

PAGE = 0x1000


@dataclass
class Segment:
    name: str
    mem_off: int
    size: int

    @property
    def end(self) -> int:
        return self.mem_off + self.size


@dataclass
class NsoImage:
    """A decompressed module image. Offsets are module-relative; `base` is the guest load address."""
    name: str
    data: bytes
    text: Segment
    ro: Segment
    rw: Segment
    bss_size: int
    build_id: bytes
    mod0_off: int
    dynamic_off: int | None
    eh_frame_hdr: tuple[int, int] | None
    bss_range: tuple[int, int] | None
    module_object_off: int | None
    base: int = 0
    segments: list[Segment] = field(default_factory=list)

    @property
    def size(self) -> int:
        """Total mapped size (data end + bss, page aligned)."""
        end = max(self.rw.end + self.bss_size, len(self.data))
        return (end + PAGE - 1) & ~(PAGE - 1)

    def u8(self, off: int) -> int:
        return self.data[off]

    def u16(self, off: int) -> int:
        return struct.unpack_from('<H', self.data, off)[0]

    def u32(self, off: int) -> int:
        return struct.unpack_from('<I', self.data, off)[0]

    def s32(self, off: int) -> int:
        return struct.unpack_from('<i', self.data, off)[0]

    def u64(self, off: int) -> int:
        return struct.unpack_from('<Q', self.data, off)[0]

    def in_text(self, off: int) -> bool:
        return self.text.mem_off <= off < self.text.end

    def in_image(self, off: int, n: int = 1) -> bool:
        return 0 <= off and off + n <= len(self.data)


def _segment_bytes(raw: bytes, file_off: int, file_size: int, mem_size: int, compressed: bool) -> bytes:
    blob = raw[file_off:file_off + file_size]
    if compressed:
        out = lz4.block.decompress(blob, uncompressed_size=mem_size)
    else:
        out = blob
    if len(out) != mem_size:
        raise ValueError(f'segment size mismatch: got {len(out):#x}, header says {mem_size:#x}')
    return out


def load_nso(path: str, name: str | None = None, verify_hashes: bool = True) -> NsoImage:
    with open(path, 'rb') as f:
        raw = f.read()
    if raw[:4] != b'NSO0':
        raise ValueError(f'{path}: not an NSO (magic {raw[:4]!r})')
    flags = struct.unpack_from('<I', raw, 0x0C)[0]
    segs = []
    for i, (hdr, fsz_off) in enumerate(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68))):
        file_off, mem_off, size = struct.unpack_from('<III', raw, hdr)
        file_size = struct.unpack_from('<I', raw, fsz_off)[0]
        segs.append((file_off, mem_off, size, file_size, bool(flags & (1 << i)), bool(flags & (8 << i))))
    bss_size = struct.unpack_from('<I', raw, 0x3C)[0]
    build_id = raw[0x40:0x60]

    end = max(m + s for _, m, s, _, _, _ in segs)
    img = bytearray(end)
    for i, (file_off, mem_off, size, file_size, comp, check) in enumerate(segs):
        blob = _segment_bytes(raw, file_off, file_size, size, comp)
        if verify_hashes and check:
            want = raw[0xA0 + 0x20 * i:0xC0 + 0x20 * i]
            if hashlib.sha256(blob).digest() != want:
                raise ValueError(f'{path}: segment {i} SHA-256 mismatch')
        img[mem_off:mem_off + size] = blob
    data = bytes(img)

    text = Segment('text', segs[0][1], segs[0][2])
    ro = Segment('ro', segs[1][1], segs[1][2])
    rw = Segment('data', segs[2][1], segs[2][2])

    mod0_off = struct.unpack_from('<I', data, text.mem_off + 4)[0]
    dynamic_off = eh = bss = modobj = None
    if 0 <= mod0_off <= len(data) - 0x1C and data[mod0_off:mod0_off + 4] == b'MOD0':
        dyn, bss_s, bss_e, eh_s, eh_e, mo = struct.unpack_from('<iiiiii', data, mod0_off + 4)
        dynamic_off = mod0_off + dyn
        bss = (mod0_off + bss_s, mod0_off + bss_e)
        if eh_e > eh_s:
            eh = (mod0_off + eh_s, mod0_off + eh_e)
        modobj = mod0_off + mo
    else:
        mod0_off = -1

    im = NsoImage(name=name or path, data=data, text=text, ro=ro, rw=rw, bss_size=bss_size, build_id=build_id,
                  mod0_off=mod0_off, dynamic_off=dynamic_off, eh_frame_hdr=eh, bss_range=bss,
                  module_object_off=modobj)
    im.segments = [text, ro, rw]
    return im
