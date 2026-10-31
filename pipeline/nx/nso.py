"""NSO0 modules: verified segment decompression, flat module images, dynamic-symbol inventory, analysis ELF.

Port of MacNativeGames `games/totk/scripts/prepare_modules.py` (MIT), split into `load()` (parse + verify) and
`to_elf()`. The ELF keeps module-relative addresses (ET_DYN); load it at the module base recorded in layout.json so
addresses match the recompiled `g_<addr>` names."""
import hashlib
import struct
from pathlib import Path

import lz4.block

from .common import require

SEGMENTS = ('text', 'rodata', 'data')
DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 1, 2, 4, 5, 6, 7, 8
DT_STRSZ, DT_SYMENT, DT_PLTREL, DT_JMPREL = 10, 11, 20, 23


def align(value, boundary=4096):
    return (value + boundary - 1) & ~(boundary - 1)


class Module:
    """A parsed NSO. `image` is the flat module image (segments at their offsets, zero gaps, no .bss)."""

    def __init__(self, name, raw):
        self.name = name
        self.raw = raw
        require(raw[:4] == b'NSO0' and len(raw) >= 256, f'{name}: expected NSO0')
        flags = struct.unpack_from('<I', raw, 12)[0]
        compressed = struct.unpack_from('<III', raw, 0x60)
        self.segments = []
        for i, seg in enumerate(SEGMENTS):
            fileoff, address, size, extra = struct.unpack_from('<IIII', raw, 0x10 + 16 * i)
            stored = compressed[i] if flags & (1 << i) else size
            require(fileoff + stored <= len(raw) and size <= 256 * 1024 * 1024, f'{name}: invalid NSO segment')
            payload = raw[fileoff:fileoff + stored]
            data = lz4.block.decompress(payload, uncompressed_size=size) if flags & (1 << i) else payload
            require(len(data) == size, f'{name}: NSO decompression size mismatch')
            digest = hashlib.sha256(data).digest()
            hash_checked = bool(flags & (8 << i))
            if hash_checked:
                require(digest == raw[0xA0 + 32 * i:0xC0 + 32 * i], f'{name}: {seg} hash mismatch')
            # `extra` is the module-offset alignment for text/rodata and the .bss size for data.
            self.segments.append(dict(name=seg, address=address, size=size, sha256=digest.hex(), data=data,
                                      extra=extra, hash_checked=hash_checked))
        prev_end = 0
        for s in self.segments:
            require(s['address'] % 4096 == 0 and s['address'] >= prev_end, f'{name}: segments out of order/unaligned')
            prev_end = s['address'] + s['size']
        self.build_id = raw[0x40:0x60].hex()
        data = self.segments[2]
        self.bss_offset = data['address'] + data['size']
        self.bss_size = data['extra']
        self.image = bytearray(self.bss_offset)
        for s in self.segments:
            self.image[s['address']:s['address'] + s['size']] = s['data']
        # Total address span the module needs once loaded (pages), .bss included.
        self.span = align(self.bss_offset + self.bss_size)
        self._parse_dynamic()

    def span_bytes(self, address, size):
        require(any(s['address'] <= address and address + size <= s['address'] + s['size'] for s in self.segments),
                f'{self.name}: metadata outside initialized segments')
        return bytes(self.image[address:address + size])

    def _parse_dynamic(self):
        self.mod0_offset = struct.unpack_from('<I', self.image, 4)[0]
        require(self.span_bytes(self.mod0_offset, 4) == b'MOD0', f'{self.name}: expected MOD0')
        # MOD0: magic, dynamic, bss_start, bss_end, eh_frame_hdr_start, eh_frame_hdr_end, module_object (all relative)
        fields = struct.unpack_from('<4s6i', self.image, self.mod0_offset)
        self.dynamic_offset = self.mod0_offset + fields[1]
        self.mod0 = dict(dynamic=self.mod0_offset + fields[1], bss_start=self.mod0_offset + fields[2],
                         bss_end=self.mod0_offset + fields[3], eh_frame_hdr_start=self.mod0_offset + fields[4],
                         eh_frame_hdr_end=self.mod0_offset + fields[5], module_object=self.mod0_offset + fields[6])
        dynamic = []
        for index in range(4096):
            tag, value = struct.unpack('<qQ', self.span_bytes(self.dynamic_offset + 16 * index, 16))
            dynamic.append((tag, value))
            if tag == DT_NULL:
                break
        else:
            raise ValueError(f'{self.name}: unterminated dynamic table')
        self.dynamic = dynamic
        tags = self.tags = dict(dynamic)
        self.strings = self.span_bytes(tags[DT_STRTAB], tags[DT_STRSZ])
        require(tags[DT_SYMENT] == 24, f'{self.name}: expected ELF64 symbols')
        self.symoff = tags[DT_SYMTAB]
        self.nbucket = None
        if DT_HASH in tags:
            self.nbucket, symcount = struct.unpack('<II', self.span_bytes(tags[DT_HASH], 8))
        else:
            _, _, syms_offset, syms_size = struct.unpack_from('<IIII', self.raw, 0x90)
            require(self.segments[1]['address'] + syms_offset == self.symoff and syms_size % 24 == 0,
                    f'{self.name}: cannot establish symbol count')
            symcount = syms_size // 24
        self.symcount = symcount
        self.symbols = []
        for i in range(symcount):
            no, info, other, section, value, size = struct.unpack('<IBBHQQ', self.span_bytes(self.symoff + 24 * i, 24))
            self.symbols.append(dict(index=i, name=self.string(no), value=value, size=size, binding=info >> 4,
                                     type=info & 15, section_index=section, defined=section != 0))
        self.needed = [self.string(value) for tag, value in dynamic if tag == DT_NEEDED]

    def string(self, offset):
        require(offset < len(self.strings), f'{self.name}: invalid dynstr offset')
        end = self.strings.find(b'\0', offset)
        require(end >= 0, f'{self.name}: unterminated dynstr')
        return self.strings[offset:end].decode('utf-8', errors='replace')

    @property
    def imports(self):
        return [s for s in self.symbols if s['name'] and not s['defined']]

    @property
    def exports(self):
        return [s for s in self.symbols if s['name'] and s['defined']]

    def svc_candidates(self):
        text = self.segments[0]
        words = memoryview(text['data']).cast('I')
        return [dict(offset=text['address'] + 4 * i, immediate=(w >> 5) & 0xFFFF)
                for i, w in enumerate(words) if w & 0xFFE0001F == 0xD4000001]

    def segment_table(self):
        return {s['name']: dict(offset=s['address'], size=s['size'], sha256=s['sha256'], hash_checked=s['hash_checked'])
                for s in self.segments} | {'bss': dict(offset=self.bss_offset, size=self.bss_size)}

    def to_elf(self):
        """Synthetic ELF64 (ET_DYN; payload bytes identical to the NSO except recorded
        dynsym section-index edits. Returns (elf_bytes, symbol_index_changes)."""
        segments = [dict(s) for s in self.segments]
        tags = self.tags
        dynoff = self.dynamic_offset
        symoff = self.symoff
        symcount = self.symcount
        phnum = 4
        cursor = 4096
        loads = []
        elf = bytearray(4096)
        for i, s in enumerate(segments):
            cursor = align(cursor) + s['address'] % 4096
            s['elf_file_offset'] = cursor
            end = cursor + s['size']
            elf.extend(bytes(max(0, end - len(elf))))
            elf[cursor:end] = s['data']
            loads.append((1, [5, 4, 6][i], cursor, s['address'], s['address'], s['size'],
                          s['size'] + (s['extra'] if i == 2 else 0), 4096))
            cursor = end

        def file_offset(address, size=1):
            for s in segments:
                if s['address'] <= address and address + size <= s['address'] + s['size']:
                    return s['elf_file_offset'] + address - s['address']
            raise ValueError('ELF metadata outside initialized segments')

        # name, type, flags, address, fileoff, size, link, info, alignment, entsize
        data = segments[2]
        dynsize = len(self.dynamic) * 16
        locals_count = sum(s['binding'] == 0 for s in self.symbols)
        metadata = [('.dynamic', 6, 3, dynoff, dynsize, '.dynstr', 0, 8, 16),
                    ('.dynstr', 3, 2, tags[DT_STRTAB], tags[DT_STRSZ], None, 0, 1, 0),
                    ('.dynsym', 11, 2, symoff, symcount * 24, '.dynstr', locals_count, 8, 24)]
        if self.nbucket is not None:
            metadata.append(('.hash', 5, 2, tags[DT_HASH], 8 + 4 * (self.nbucket + symcount), '.dynsym', 0, 8, 4))
        for name, address_tag, size_tag in [('.rela.dyn', DT_RELA, DT_RELASZ), ('.rela.plt', DT_JMPREL, DT_PLTRELSZ)]:
            if address_tag in tags and size_tag in tags and tags[size_tag]:
                require(name != '.rela.plt' or tags.get(DT_PLTREL) == 7, 'Unsupported PLT relocation format')
                metadata.append((name, 4, 2, tags[address_tag], tags[size_tag], '.dynsym', 0, 8, 24))
        sections = [('', 0, 0, 0, 0, 0, 0, 0, 0, 0)]
        pending_links = {}
        for segment_index, segment in enumerate(segments):
            begin, end = segment['address'], segment['address'] + segment['size']
            contained = sorted((m for m in metadata if begin <= m[3] and m[3] + m[4] <= end), key=lambda m: m[3])
            cursor_address, part = begin, 0
            for meta in contained:
                name, kind, attrs, address, size, link, info, alignment, entsize = meta
                require(address >= cursor_address, 'Overlapping dynamic metadata')
                if address > cursor_address:
                    chunkname = '.' + segment['name'] + ('' if part == 0 else f'.{part}')
                    sections.append((chunkname, 1, [6, 2, 3][segment_index], cursor_address,
                                     file_offset(cursor_address), address - cursor_address, 0, 0, 1, 0))
                    part += 1
                sections.append((name, kind, attrs, address, file_offset(address, size), size, 0, info, alignment,
                                 entsize))
                if link:
                    pending_links[len(sections) - 1] = link
                cursor_address = address + size
            if cursor_address < end:
                chunkname = '.' + segment['name'] + ('' if part == 0 else f'.{part}')
                sections.append((chunkname, 1, [6, 2, 3][segment_index], cursor_address,
                                 file_offset(cursor_address), end - cursor_address, 0, 0, 1, 0))
        sections.append(('.bss', 8, 3, data['address'] + data['size'], data['elf_file_offset'] + data['size'],
                         data['extra'], 0, 0, 1, 0))
        section_indices = {s[0]: i for i, s in enumerate(sections)}
        for i, link in pending_links.items():
            row = list(sections[i])
            row[6] = section_indices[link]
            sections[i] = tuple(row)
        changed = []
        for s in self.symbols:
            if s['section_index'] in (0, 0xFFF1, 0xFFF2):
                continue
            index = next((i for i, section in enumerate(sections)
                          if section[2] & 2 and section[3] <= s['value'] < section[3] + section[5]), None)
            if index is None:
                index = next((i for i, section in enumerate(sections)
                              if section[2] & 2 and s['size'] == 0 and s['value'] == section[3] + section[5]), None)
            require(index is not None, f'Cannot map defined symbol: {s["name"]}')
            if index != s['section_index']:
                location = file_offset(symoff + s['index'] * 24 + 6, 2)
                struct.pack_into('<H', elf, location, index)
                changed.append(dict(symbol=s['index'], file_offset=location, original=s['section_index'], elf=index))
        name_table = bytearray(b'\0')
        names = {}
        for name in [s[0] for s in sections] + ['.shstrtab']:
            if name not in names:
                names[name] = len(name_table)
                name_table.extend(name.encode() + b'\0')
        nameoff = align(len(elf), 8)
        elf.extend(bytes(nameoff - len(elf)) + name_table)
        sections.append(('.shstrtab', 3, 0, 0, nameoff, len(name_table), 0, 0, 1, 0))
        shoff = align(len(elf), 8)
        elf.extend(bytes(shoff - len(elf)))
        for section in sections:
            name, *fields = section
            elf.extend(struct.pack('<IIQQQQIIQQ', names[name] if name else 0, *fields))
        ident = b'\x7fELF\x02\x01\x01' + bytes(9)
        elf[:64] = struct.pack('<16sHHIQQQIHHHHHH', ident, 3, 183, 1, 0, 64, shoff, 0, 64, 56, phnum, 64,
                               len(sections), len(sections) - 1)
        loads.append((2, 6, file_offset(dynoff), dynoff, dynoff, dynsize, dynsize, 8))
        for i, ph in enumerate(loads):
            struct.pack_into('<IIQQQQQQ', elf, 64 + 56 * i, *ph)
        # Validate every LOAD byte against the NSO, allowing only recorded index edits.
        check_image = bytearray(elf)
        for change in changed:
            struct.pack_into('<H', check_image, change['file_offset'], change['original'])
        for s in segments:
            offset = s['elf_file_offset']
            require(bytes(check_image[offset:offset + s['size']]) == s['data'], 'ELF payload differs from NSO')
        return bytes(elf), changed


def load(path):
    path = Path(path)
    return Module(path.name, path.read_bytes())
