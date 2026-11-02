"""A module loaded at its guest base: absolute-address accessors over the NSO image and its ELF metadata.

Optionally the code comes from a patched flat image (games/totk/patches: same layout as the NSO image, code
patches applied, veneers in the padding after .text) described by `<image>.patches.json`.
"""
from __future__ import annotations

import json
import struct

import numpy as np

from .elf import DynInfo, parse_dynamic
from .nso import NsoImage, load_nso


class Module:
    def __init__(self, name: str, path: str, base: int, image: str | None = None, patches: str | None = None):
        self.name = name
        self.path = path
        self.base = base
        self.im: NsoImage = load_nso(path, name)
        self.dyn: DynInfo = parse_dynamic(self.im)
        self.image_path = image
        self.patches_path = patches
        self.veneers: tuple[tuple[int, int], ...] = ()          # absolute [start, end) of patch veneers
        text_end = self.im.text.end
        if patches:
            with open(patches, encoding='utf-8') as f:
                pj = json.load(f)
            if pj.get('status') != 'pass':
                raise ValueError(f'{patches}: status {pj.get("status")!r}')
            if int(pj['text_end'], 0) != self.im.text.end:
                raise ValueError(f'{patches}: text_end {pj["text_end"]} does not match {self.im.text.end:#x}')
            text_end = int(pj['text_end_patched'], 0)
            self.veneers = tuple(sorted((base + int(v['start'], 0), base + int(v['end'], 0)) for v in pj['veneers']))
            if image is None:
                image = patches[:-len('.patches.json')] if patches.endswith('.patches.json') else None
                self.image_path = image
        self.patched_words: frozenset[int] = frozenset()        # absolute addresses of .text words the image changed
        if image:
            with open(image, 'rb') as f:
                data = f.read()
            if len(data) < text_end or len(data) > len(self.im.data):
                raise ValueError(f'{image}: {len(data):#x} bytes does not fit the module image ({len(self.im.data):#x})')
            t0, t1 = self.im.text.mem_off, self.im.text.end
            old = np.frombuffer(self.im.data, dtype='<u4', count=(t1 - t0) // 4, offset=t0)
            new = np.frombuffer(data, dtype='<u4', count=(t1 - t0) // 4, offset=t0)
            self.patched_words = frozenset(int(base + t0 + 4 * i) for i in np.nonzero(old != new)[0])
            self.im.data = data + self.im.data[len(data):]
        if text_end > self.im.ro.mem_off:
            raise ValueError(f'patched text end {text_end:#x} overlaps .rodata')
        self.text_start = base + self.im.text.mem_off
        self.text_end = base + text_end
        nwords = (text_end - self.im.text.mem_off) // 4
        self.words = np.frombuffer(self.im.data, dtype='<u4', count=nwords, offset=self.im.text.mem_off)
        self._mv = memoryview(self.im.data)[self.im.text.mem_off:self.im.text.mem_off + nwords * 4].cast('I')
        self._reloc: dict[int, int] | None = None
        self._got_names: dict[int, str] | None = None
        self._sym_names: dict[int, str] | None = None

    def import_stub(self, a: int) -> str | None:
        """The imported symbol a PLT stub at `a` jumps to (`adrp x16; ldr x17, [x16, #o]; add x16, x16, #o;
        br x17`), '' for a stub whose GOT slot has no named import, None when `a` is not a stub."""
        if not (self.in_text(a) and self.in_text(a + 12)):
            return None
        w0, w1, w2, w3 = (self.word(a + 4 * k) for k in range(4))
        if (w0 & 0x9F00001F) != 0x90000010 or (w1 & 0xFFC003FF) != 0xF9400211 or \
                (w2 & 0xFFC003FF) != 0x91000210 or w3 != 0xD61F0220:
            return None
        imm = ((w0 >> 29) & 3) | (((w0 >> 5) & 0x7FFFF) << 2)
        page = (a & ~0xFFF) + ((imm - (1 << 21) if imm & (1 << 20) else imm) << 12)
        slot = page + ((w1 >> 10) & 0xFFF) * 8
        if self._got_names is None:
            from .elf import R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT  # noqa: PLC0415
            syms = self.dyn.symbols
            self._got_names = {self.base + r.offset: syms[r.sym].name for r in self.dyn.relocs
                               if r.type in (R_AARCH64_JUMP_SLOT, R_AARCH64_GLOB_DAT) and 0 < r.sym < len(syms)}
        return self._got_names.get(slot, '')

    def symbol_at(self, a: int) -> str | None:
        """Name of a symbol this module defines at `a` (dynsym), if any."""
        if self._sym_names is None:
            from .elf import SHN_UNDEF  # noqa: PLC0415
            self._sym_names = {}
            for s in self.dyn.symbols:
                if s.shndx != SHN_UNDEF and s.name:
                    self._sym_names.setdefault(self.base + s.value, s.name)
        return self._sym_names.get(a)

    def callee_name(self, a: int) -> str | None:
        """The symbol a call to `a` reaches: the import behind a PLT stub, else a symbol defined at `a`."""
        imp = self.import_stub(a)
        return imp if imp else self.symbol_at(a)

    def in_veneer(self, a: int) -> bool:
        for s, e in self.veneers:
            if s <= a < e:
                return True
        return False

    # code
    def in_text(self, a: int) -> bool:
        return self.text_start <= a < self.text_end

    def word(self, a: int) -> int:
        return self._mv[(a - self.text_start) >> 2]

    # data (initial image contents, rebased where the loader would rebase)
    @property
    def reloc(self) -> dict[int, int]:
        if self._reloc is None:
            self._reloc = {self.base + off: self.base + tgt for off, tgt in self.dyn.relocated_words(self.im).items()}
        return self._reloc

    def readable(self, a: int, n: int) -> bool:
        off = a - self.base
        return 0 <= off and off + n <= len(self.im.data)

    def read(self, a: int, n: int, signed: bool = False) -> int | None:
        """Initial value of n bytes at a (n = 1, 2, 4, 8), or None outside the image."""
        if not self.readable(a, n):
            return None
        if n == 8 and a in self.reloc:
            return self.reloc[a]
        fmt = {1: 'b', 2: 'h', 4: 'i', 8: 'q'}[n]
        return struct.unpack_from('<' + (fmt if signed else fmt.upper()), self.im.data, a - self.base)[0]
