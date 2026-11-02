"""Port addresses from one build of a module to another build of the same program (TotK 1.0.0 main -> 1.4.3 main).

    python tools/version_port.py stats --old <v100/exefs/main> --new <v143/exefs/main> [--json out.json]
    python tools/version_port.py map   --old ... --new ... --addrs 0x7b598c,0x4557b30 | --addrs-file names.json
                                       [--out map.json]

Our patches, hooks and engine reads name 1.0.0 main offsets; a later update moves almost every function. This finds
where each one went, by matching functions, never by guessing:

  1. Function list of both images from .eh_frame_hdr (every compiled function in main has an FDE).
  2. Each body is normalized: the operands that only carry an address (B/BL targets, ADRP pages, ADR, LDR-literal, the
     low 12 bits that complete an ADRP) are masked, everything else stays. Bodies whose normalized hash is unique on
     both sides match ("hash").
  3. Matched pairs vote for more: the n-th call / tail call / function-pointer load of one body against the n-th of
     the other ("call"), over the aligned parts when the bodies differ; vtables reached from matched data walk their
     relocated slots side by side ("vtable"). A vote only lands when it is unanimous and neither side is taken.
     Changed codegen (another toolchain inlines differently): a gap between two paired calls of unequal length on
     the two sides votes each old callee for the one new callee whose opcode classes clearly match best ("call").
     An unpaired function calling at least CALLEE_SET_MIN paired functions (no hubs) is voted for the one unpaired
     new function that calls all of their pairs ("callees"). Both land only through `similar`, like every vote: a
     position or a shape alone is never evidence.
  4. Runs of unmatched functions between two matched neighbours, equal in count with similar sizes on both sides,
     match in order ("order"); each one then has to agree with what its calls vote, or the whole run is dropped.
  5. Conflicts, over all pairs to a fixpoint: every aligned call / tail call / function pointer of a pair must agree
     with the pairs at its ends (normalization hides call targets, so two same-shape wrappers of different callees
     hash alike). The side with less agreement is dropped and banned, with whatever only its votes landed; a hash
     pair under SMALL_WORDS needs an agreeing edge or agreeing neighbours.
  6. Data addresses (globals, GOT slots, vtables) map by a clear majority of at least DATA_MIN_VOTES references from
     the final pairs at the same instruction ("data"), never by distance.

An address inside a function maps to the same instruction of its match when the bodies are equal after
normalization ("exact"), or when it falls in an equal stretch of the two aligned bodies ("aligned"). Anything else is
reported unmapped with the reason; `map` never invents an address. Each mapped text address also says whether the raw
word is identical (`same_word`): a patch's original word must still be checked against the new image (apply does),
and whether its pair is `confirmed` (an agreeing edge or agreeing neighbours): callers placing code use only those.
"""
from __future__ import annotations

import argparse
import bisect
import difflib
import hashlib
import json
import os
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ALIGN_MAX_WORDS = 6000      # SequenceMatcher on longer bodies is too slow to be worth it; such pairs map "exact" only
ORDER_SIZE_RATIO = 1.6      # an "order" pair's sizes may differ by at most this factor
FUZZY_RATIO = 0.7           # changed codegen: opcode-class sequences (registers ignored) that agree this much
FUZZY_MAX_WORDS = 6000
FUZZY_SHORT_WORDS = 32      # below this a fuzzy pair needs FUZZY_SHORT_AGREE agreeing paired callees
FUZZY_SHORT_AGREE = 2
GAP_MAX = 10                # an unequal call gap longer than this on either side votes nothing
GAP_MAX_WORDS = 2000        # gap candidates longer than this are not scored
GAP_MARGIN = 0.15           # the best gap candidate's ratio must beat the second's by this much
CALLEE_SET_MIN = 2          # paired callees (no hubs) a callee-set vote needs
HUB_CALLERS = 50            # a callee called from more functions than this (an allocator, a logger) proves nothing
ANCHOR_STRING_MIN = 16      # a key of a single string needs one at least this long
ANCHOR_IMPORTS_MIN = 3      # an import-only key needs this many imports, or one outside LIBC
LIBC = frozenset(('memcpy', 'memset', 'memmove', 'memcmp', 'strlen', 'strcmp', 'strncmp', 'strcpy', 'strncpy',
                  'strchr', 'strrchr', 'strstr', 'malloc', 'free', 'calloc', 'realloc', 'abort', 'vsnprintf',
                  'snprintf', 'sprintf', 'printf', '__cxa_guard_acquire', '__cxa_guard_release', '__cxa_atexit',
                  '__cxa_pure_virtual', '_Znwm', '_ZdlPv', '_Znam', '_ZdaPv', 'atexit', 'sqrtf', 'sinf', 'cosf',
                  'tanf', 'atan2f', 'powf', 'expf', 'logf', 'floorf', 'ceilf', 'fmodf'))
SIMILAR_RATIO = 0.6         # a pair without a hash match needs this much of its normalized bodies in common
SIMILAR_MIN_WORDS = 4       # below this a body says too little: only an identical one counts
SIMILAR_MAX_WORDS = 20000
SMALL_WORDS = 8             # a hash pair shorter than this needs an agreeing edge or agreeing neighbours
STRING_MIN = 4              # a string_map string has at least this many characters
DATA_MIN_VOTES = 2          # a data address maps on at least this many agreeing references


def _sext(v, bits):
    v = v.astype(np.int64)
    return np.where(v & (1 << (bits - 1)), v - (1 << bits), v)


@dataclass
class Image:
    """One module: its .text words and functions, module-relative offsets throughout."""
    name: str
    data: bytes                                   # the flat image (text, ro, data; no bss)
    text_off: int
    words: np.ndarray                             # uint32 .text words
    funcs: list[tuple[int, int]]                  # sorted (start, size), size in bytes
    relocs: dict[int, int] = field(default_factory=dict)   # R_AARCH64_RELATIVE: slot -> target
    span: int = 0                                 # loaded size, bss included
    ro: tuple = None                              # [start, end) of the read-only segment
    bounds: dict = None                           # segment boundaries by name (text_end, rw_start, bss_end, ...)
    path: str = None                              # the NSO (recomp.module: the import behind a PLT stub)

    @property
    def text_end(self):
        return self.text_off + 4 * len(self.words)


def load_image(path: str, name: str = 'main') -> Image:
    sys.path.insert(0, ROOT)
    from recomp.elf import parse_dynamic
    from recomp.nso import load_nso
    im = load_nso(path, name)
    dyn = parse_dynamic(im)
    t0, t1 = im.text.mem_off, im.text.end
    words = np.frombuffer(im.data, dtype='<u4', count=(t1 - t0) // 4, offset=t0).copy()
    funcs = {}
    for f in dyn.fdes:
        if t0 <= f.start < t1 and f.size and not f.start & 3:
            funcs[f.start] = max(funcs.get(f.start, 0), min(f.size, t1 - f.start))
    return Image(name, im.data, t0, words, sorted(funcs.items()), dict(dyn.relocated_words(im)), im.size,
                 (im.ro.mem_off, im.ro.end),
                 dict(text_end=t1, ro_start=im.ro.mem_off, ro_end=im.ro.end, rw_start=im.rw.mem_off, rw_end=im.rw.end,
                      bss_start=(im.bss_range or (None, None))[0], bss_end=(im.bss_range or (None, None))[1], span=im.size),
                 path)


# ------------------------------------------------------------------------------------------------- per-word analysis

@dataclass
class Analysis:
    norm: np.ndarray            # normalized words
    branch_tgt: np.ndarray      # per word: B/BL target (module offset) or -1
    ref_tgt: np.ndarray         # per word: address an ADRP pair completes here (ADD / LDR / STR), or -1
    adr_tgt: np.ndarray         # per word: ADR / LDR-literal target, or -1


def analyse(img: Image) -> Analysis:
    w = img.words.astype(np.uint32)
    n = len(w)
    base = img.text_off
    norm = w.copy()
    branch_tgt = np.full(n, -1, dtype=np.int32)
    ref_tgt = np.full(n, -1, dtype=np.int32)
    adr_tgt = np.full(n, -1, dtype=np.int32)

    def put(arr, idx, tgt):                   # module offsets fit in 31 bits; a word decoding outside never matters
        ok = (tgt >= 0) & (tgt < 1 << 31)
        arr[idx[ok]] = tgt[ok]

    def imm21(i):
        return _sext(((w[i] >> 5) & 0x7FFFF).astype(np.int64) << 2 | ((w[i] >> 29) & 3), 21)

    i = np.nonzero((w & 0x7C000000) == 0x14000000)[0]            # B and BL
    put(branch_tgt, i, base + 4 * i + 4 * _sext(w[i] & 0x3FFFFFF, 26))
    norm[i] &= 0xFC000000

    i = np.nonzero((w & 0x9F000000) == 0x10000000)[0]            # ADR
    put(adr_tgt, i, base + 4 * i + imm21(i))
    norm[i] &= 0x9F00001F
    i = np.nonzero((w & 0x3B000000) == 0x18000000)[0]            # LDR (literal), GPR / SIMD / LDRSW / PRFM
    put(adr_tgt, i, base + 4 * i + 4 * _sext((w[i] >> 5) & 0x7FFFF, 19))
    norm[i] &= 0xFF00001F

    adrp = np.nonzero((w & 0x9F000000) == 0x90000000)[0]
    norm[adrp] &= 0x9F00001F
    page = ((base + 4 * adrp) & ~0xFFF) + (imm21(adrp) << 12)
    rd = w[adrp] & 31
    claimed = np.zeros(n, dtype=bool)
    for k in range(1, 5):                     # the ADRP's partner, up to 4 words later (what the compilers emit)
        keep = adrp + k < n
        src, pg, r = adrp[keep], page[keep], rd[keep]
        dst = src + k
        wd = w[dst]
        addi = (wd & 0x7F800000) == 0x11000000                    # ADD (immediate), 32 / 64-bit
        ldst = (wd & 0x3B000000) == 0x39000000                    # LDR / STR (unsigned offset), GPR and SIMD
        ok = ~claimed[dst] & (((wd >> 5) & 31) == r) & (addi | ldst)
        dst, wd, pg, addi = dst[ok], wd[ok], pg[ok], addi[ok]
        imm12 = ((wd >> 10) & 0xFFF).astype(np.int64)
        size = (wd >> 30).astype(np.int64)
        simd128 = ((wd >> 26) & 1).astype(bool) & ((wd >> 23) & 1).astype(bool) & (size == 0)
        scale = np.where(simd128, 16, 1 << size)
        put(ref_tgt, dst, pg + np.where(addi, imm12, imm12 * scale))
        claimed[dst] = True
        norm[dst] &= ~np.uint32(0xFFF << 10)
    return Analysis(norm, branch_tgt, ref_tgt, adr_tgt)


# ---------------------------------------------------------------------------------------------------------- matching

class Porter:
    def __init__(self, old: Image, new: Image):
        self.old, self.new = old, new
        self.ao, self.an = analyse(old), analyse(new)
        self.starts_o = [s for s, _ in old.funcs]
        self.starts_n = [s for s, _ in new.funcs]
        self.size_o = dict(old.funcs)
        self.size_n = dict(new.funcs)
        self.hash_o = {s: self._hash(self.ao, old, s, z) for s, z in old.funcs}
        self.hash_n = {s: self._hash(self.an, new, s, z) for s, z in new.funcs}
        self.fwd: dict[int, int] = {}          # old function start -> new function start
        self.rev: dict[int, int] = {}
        self.how: dict[int, str] = {}
        self.src: dict[int, set] = {}          # a voted pair -> the pairs whose votes landed it
        self.voted: dict[int, set] = defaultdict(set)   # a pair -> the voted pairs it helped land
        self.banned: set[tuple[int, int]] = set()      # pairs a conflict dropped: never landed again
        self.conf: Counter = Counter()         # a pair -> aligned edges that agree with it (validate)
        self.data_votes: dict[int, Counter] = defaultdict(Counter)
        self.data_map: dict[int, int] = {}
        self._rn_by_target = None              # new relocation target -> its slots (vtable_map)
        self._strings = ({}, {})               # string at an address, per side (anchors)
        self._import_names = [None, None]      # PLT stub -> import name, per side (anchors)
        self._anchor_keys = None               # (strings, imports) -> functions with that key, per side, whole image
        self._callers = [None, None]           # callee -> how many functions call it, per side (hubs)
        self._done: set[int] = set()           # pairs whose votes were already cast
        self._align_cache: dict[int, list] = {}
        self.dropped_order = 0
        self.dropped_conflict = 0
        self._no_order: set[int] = set()       # old functions of an order run a call vote contradicted
        self._uniq = ({}, {})                  # unique-body partners (match_hash), per side: no vote overrules them

    # --- bodies
    @staticmethod
    def _slice(img, s, z):
        i = (s - img.text_off) // 4
        return i, i + z // 4

    def _hash(self, a, img, s, z):
        i, j = self._slice(img, s, z)
        return hashlib.blake2b(a.norm[i:j].tobytes(), digest_size=12).digest()

    def _norm(self, side, s):
        img, a, sizes = (self.old, self.ao, self.size_o) if side == 0 else (self.new, self.an, self.size_n)
        i, j = self._slice(img, s, sizes[s])
        return a.norm[i:j]

    def blocks(self, fo):
        """Equal stretches (i_old, i_new, n) of the bodies of matched pair fo -> fwd[fo], in words."""
        fn = self.fwd[fo]
        if self.hash_o[fo] == self.hash_n[fn]:
            return [(0, 0, self.size_o[fo] // 4)]
        if fo not in self._align_cache:
            a, b = self._norm(0, fo), self._norm(1, fn)
            if max(len(a), len(b)) > ALIGN_MAX_WORDS:
                self._align_cache[fo] = []
            else:
                sm = difflib.SequenceMatcher(None, a.tolist(), b.tolist(), autojunk=False)
                self._align_cache[fo] = [tuple(m) for m in sm.get_matching_blocks() if m[2]]
        return self._align_cache[fo]

    # --- lookups
    def func_of(self, side, off):
        starts, sizes = (self.starts_o, self.size_o) if side == 0 else (self.starts_n, self.size_n)
        k = bisect.bisect_right(starts, off) - 1
        if k >= 0 and off < starts[k] + sizes[starts[k]]:
            return starts[k]
        return None

    def _pair(self, fo, fn, how, src=None):
        self.fwd[fo], self.rev[fn], self.how[fo] = fn, fo, how
        if src:
            self.src[fo] = set(src)
            for v in src:
                self.voted[v].add(fo)

    def drop(self, fo):
        """Unpair fo, ban the pair, and unpair what only its votes landed (to the end of that chain)."""
        stack = [fo]
        while stack:
            x = stack.pop()
            if x not in self.fwd:
                continue
            y = self.fwd.pop(x)
            del self.rev[y]
            self.how.pop(x)
            self.banned.add((x, y))
            self._done.discard(x)
            self._align_cache.pop(x, None)
            for v in self.src.pop(x, ()):
                self.voted[v].discard(x)
            for z in self.voted.pop(x, ()):
                src = self.src.get(z)
                if src is not None:
                    src.discard(x)
                    if not src:
                        stack.append(z)

    # --- step 2: hash
    def match_hash(self):
        by_o, by_n = defaultdict(list), defaultdict(list)
        for s, h in self.hash_o.items():
            by_o[h].append(s)
        for s, h in self.hash_n.items():
            by_n[h].append(s)
        for h, so in by_o.items():
            sn = by_n.get(h)
            if len(so) == 1 and sn and len(sn) == 1:
                self._pair(so[0], sn[0], 'hash')
                self._uniq[0][so[0]], self._uniq[1][sn[0]] = sn[0], so[0]

    # --- step 3: votes
    def _events(self, fo):
        """Pairs (old target, new target) of every address-carrying word in the equal stretches of fo's pair."""
        fn = self.fwd[fo]
        io, _ = self._slice(self.old, fo, self.size_o[fo])
        jn, _ = self._slice(self.new, fn, self.size_n[fn])
        for bo, bn, n in self.blocks(fo):
            for arr_o, arr_n in ((self.ao.branch_tgt, self.an.branch_tgt), (self.ao.ref_tgt, self.an.ref_tgt),
                                 (self.ao.adr_tgt, self.an.adr_tgt)):
                xo = arr_o[io + bo:io + bo + n]
                xn = arr_n[jn + bn:jn + bn + n]
                for k in np.nonzero((xo >= 0) & (xn >= 0))[0]:
                    yield int(xo[k]), int(xn[k])

    def _in_text(self, to, tn):
        return self.old.text_off <= to < self.old.text_end, self.new.text_off <= tn < self.new.text_end

    def vote_round(self):
        fvotes: dict[int, Counter] = defaultdict(Counter)
        voters: dict[tuple, set] = defaultdict(set)
        for fo in list(self.fwd):
            if fo in self._done:
                continue
            self._done.add(fo)
            for to, tn in self._events(fo):
                to_in, tn_in = self._in_text(to, tn)
                if to_in and tn_in:
                    if to in self.size_o and tn in self.size_n:      # a call / tail call / pointer to a function
                        fvotes[to][tn] += 1
                        voters[to, tn].add(fo)
                    # branches inside the body (or into another function's middle) carry nothing new
                elif not to_in and not tn_in and (to - 0x10 in self.old.relocs or to in self.old.relocs):
                    for x, y in self._vtable_pairs(to, tn):
                        fvotes[x][y] += 1
                        voters[x, y].add(fo)
            if self.hash_o[fo] != self.hash_n[self.fwd[fo]]:
                self.callee_votes(fo, fvotes, voters)
        return self._land(fvotes, voters, 'call')

    def _vtable_pairs(self, to, tn):
        """Walk two relocated pointer tables side by side from the referenced slot while both hold code pointers."""
        ro, rn = self.old.relocs, self.new.relocs
        for k in range(0, 0x2000, 8):
            x, y = ro.get(to + k), rn.get(tn + k)
            if x is None or y is None:
                break
            if x in self.size_o and y in self.size_n:
                yield x, y
            elif (x in self.size_o) != (y in self.size_n):
                break

    def similar(self, fo, fn):
        """Positive evidence for a pair no hash proved: the normalized bodies mostly agree. A call site that now
        calls a new function, or a new function laid where a removed one was, fails this."""
        a, b = self._norm(0, fo), self._norm(1, fn)
        if max(len(a), len(b)) > SIMILAR_MAX_WORDS or min(len(a), len(b)) < SIMILAR_MIN_WORDS:
            return self.hash_o[fo] == self.hash_n[fn]
        sm = difflib.SequenceMatcher(None, a.tolist(), b.tolist(), autojunk=False)
        if sm.quick_ratio() >= SIMILAR_RATIO and sm.ratio() >= SIMILAR_RATIO:
            return True
        return self.fuzzy_similar(fo, fn)

    # --- changed codegen (1.0.0 -> 1.4.2: 42% of the bodies identical after normalization): the same function
    # compiled with other registers or another schedule. Its opcode classes (bits 21-31: the operation, not the
    # registers) still mostly agree, it references the same strings and calls the same imports.
    def _classes(self, side, s):
        return (self._norm(side, s) >> 21).tolist()

    def fuzzy_blocks(self, fo):
        fn = self.fwd[fo]
        key = ('f', fo)
        if key not in self._align_cache:
            a, b = self._classes(0, fo), self._classes(1, fn)
            if max(len(a), len(b)) > FUZZY_MAX_WORDS:
                self._align_cache[key] = []
            else:
                sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
                self._align_cache[key] = [tuple(m) for m in sm.get_matching_blocks() if m[2]]
        return self._align_cache[key]

    def fuzzy_similar(self, fo, fn):
        zo, zn = self.size_o[fo] // 4, self.size_n[fn] // 4
        if min(zo, zn) < SIMILAR_MIN_WORDS or max(zo, zn) > FUZZY_MAX_WORDS or max(zo, zn) > ORDER_SIZE_RATIO * min(zo, zn):
            return False
        # a similar shape is not enough (a new function laid where a removed one was can share it): also what the two
        # name has to agree, the same strings / imports (not none), or the callees already paired
        ao, an = self.anchors(0, fo), self.anchors(1, fn)
        if not ((ao[0] or ao[1]) and ao == an and self.anchor_key_ok(ao)):
            hubs = self._hubs(0)
            mapped = {self.fwd[t] for t in self._calls(0, fo) if t in self.fwd and t not in hubs}
            agree = len(mapped & set(self._calls(1, fn)))
            need = FUZZY_SHORT_AGREE if min(zo, zn) < FUZZY_SHORT_WORDS else 1
            if agree < need or agree < 2 * (len(mapped) - agree):
                return False
        sm = difflib.SequenceMatcher(None, self._classes(0, fo), self._classes(1, fn), autojunk=False)
        return sm.quick_ratio() >= FUZZY_RATIO and sm.ratio() >= FUZZY_RATIO

    def _string_at(self, side, t):
        img = self.old if side == 0 else self.new
        cache = self._strings[side]
        if t not in cache:
            d, ro = img.data, img.ro
            v = None
            if ro and ro[0] < t < ro[1] and d[t - 1] == 0:
                end = d.find(b'\0', t, min(ro[1], t + 512))
                if end - t >= STRING_MIN and all(32 <= c < 127 or c in (9, 10, 13) for c in d[t:end]):
                    v = d[t:end]
            cache[t] = v
        return cache[t]

    def _imports(self, side):
        """branch target -> the import behind its PLT stub (recomp.module), for the targets the image calls"""
        if self._import_names[side] is None:
            img, a = (self.old, self.ao) if side == 0 else (self.new, self.an)
            names = {}
            if img.path:
                from recomp.module import Module
                m = Module('main', img.path, 0)
                sizes = self.size_o if side == 0 else self.size_n
                for t in set(int(x) for x in a.branch_tgt[a.branch_tgt >= 0].tolist()):
                    if t not in sizes and img.text_off <= t < img.text_end:
                        n = m.import_stub(t)
                        if n:
                            names[t] = n
            self._import_names[side] = names
        return self._import_names[side]

    def anchors(self, side, s):
        """what a function names that survives a recompile: the strings it references, the imports it calls"""
        img, a, sizes = (self.old, self.ao, self.size_o) if side == 0 else (self.new, self.an, self.size_n)
        i, j = self._slice(img, s, sizes[s])
        strings = set()
        for arr in (a.ref_tgt, a.adr_tgt):
            for t in arr[i:j][arr[i:j] >= 0].tolist():
                v = self._string_at(side, t)
                if v:
                    strings.add(v)
        imp = self._imports(side)
        calls = tuple(imp[t] for t in a.branch_tgt[i:j][a.branch_tgt[i:j] >= 0].tolist() if t in imp)
        return frozenset(strings), calls

    @staticmethod
    def anchor_key_ok(k):
        """a key that names a function, not a kind of function: several strings, or one long one; imports only when
        there are ANCHOR_IMPORTS_MIN of them or one is not libc"""
        strings, calls = k
        if strings:
            return len(strings) > 1 or len(next(iter(strings))) >= ANCHOR_STRING_MIN
        return len(calls) >= ANCHOR_IMPORTS_MIN or any(c not in LIBC for c in calls)

    def match_anchors(self):
        """functions whose (strings, imports) key is unique across each WHOLE image (matched functions included) and
        the same on both sides, with sizes within 2x. An anchor pair only seeds votes: like every pair it is placement
        evidence once an agreeing edge or its neighbours confirm it (confirmed)"""
        if self._anchor_keys is None:
            self._anchor_keys = [defaultdict(list), defaultdict(list)]
            for side, starts in ((0, self.starts_o), (1, self.starts_n)):
                for st in starts:
                    k = self.anchors(side, st)
                    if (k[0] or k[1]) and self.anchor_key_ok(k):
                        self._anchor_keys[side][k].append(st)
        by = [{k: v for k, v in d.items() if not any(x in taken for x in v)}
              for d, taken in zip(self._anchor_keys, (self.fwd, self.rev))]
        added = 0
        for k, so in by[0].items():
            sn = by[1].get(k)
            if len(so) == 1 and sn and len(sn) == 1 and (so[0], sn[0]) not in self.banned and \
                    self.uniq_ok(so[0], sn[0]):
                zo, zn = self.size_o[so[0]], self.size_n[sn[0]]
                if max(zo, zn) <= 2 * min(zo, zn):
                    self._pair(so[0], sn[0], 'anchor')
                    added += 1
        return added

    def match_callee_sets(self):
        """each unpaired old function with >= CALLEE_SET_MIN paired non-hub callees: the unpaired new functions that
        call the pairs of all of them; exactly one, of a similar body, votes (and lands like any vote: unanimous,
        neither side taken)"""
        hubs_o, hubs_n = self._hubs(0), self._hubs(1)
        callers_n = defaultdict(set)
        for st in self.starts_n:
            if st not in self.rev:
                for t in self._calls(1, st):
                    callers_n[t].add(st)
        fvotes, voters = defaultdict(Counter), defaultdict(set)
        for fo in self.starts_o:
            if fo in self.fwd:
                continue
            mc = {self.fwd[t] for t in self._calls(0, fo) if t in self.fwd and t not in hubs_o}
            mc = {t for t in mc if t not in hubs_n}
            if len(mc) < CALLEE_SET_MIN:
                continue
            common = None
            for t in mc:
                common = callers_n[t] if common is None else common & callers_n[t]
                if not common:
                    break
            if common and len(common) == 1:
                fn = next(iter(common))
                if max(self.size_o[fo], self.size_n[fn]) <= ORDER_SIZE_RATIO * min(self.size_o[fo], self.size_n[fn]):
                    fvotes[fo][fn] += 1
        return self._land(fvotes, voters, 'callees')

    def _hubs(self, side):
        """callees called from more than HUB_CALLERS functions on this side"""
        if self._callers[side] is None:
            c = Counter()
            for st in (self.starts_o if side == 0 else self.starts_n):
                for t in set(self._calls(side, st)):
                    c[t] += 1
            self._callers[side] = {t for t, n in c.items() if n > HUB_CALLERS}
        return self._callers[side]

    def _calls(self, side, s):
        img, a, sizes = (self.old, self.ao, self.size_o) if side == 0 else (self.new, self.an, self.size_n)
        i, j = self._slice(img, s, sizes[s])
        return [t for t in a.branch_tgt[i:j][a.branch_tgt[i:j] >= 0].tolist() if t in sizes and t != s]

    def callee_votes(self, fo, fvotes, voters):
        """a changed pair's calls, aligned on the callees already paired: the unpaired calls between two paired ones,
        as many on each side, vote in order"""
        co, cn = self._calls(0, fo), self._calls(1, self.fwd[fo])
        if not co or not cn:
            return
        mo = [self.fwd.get(t, ('?', t)) for t in co]
        sm = difflib.SequenceMatcher(None, mo, cn, autojunk=False)
        prev = (0, 0)
        for bo, bn, n in sm.get_matching_blocks():
            go, gn = co[prev[0]:bo], cn[prev[1]:bn]
            if go and len(go) == len(gn):
                for x, y in zip(go, gn):
                    if x not in self.fwd and y not in self.rev:
                        fvotes[x][y] += 1
                        voters[x, y].add(fo)
            elif go and gn and len(go) <= GAP_MAX and len(gn) <= GAP_MAX:
                for x, y in self._gap_pairs(go, gn):
                    fvotes[x][y] += 1
                    voters[x, y].add(fo)
            prev = (bo + n, bn + n)

    def shape_ratio(self, fo, fn):
        """opcode-class agreement of two bodies (0 when the sizes are too far apart or the bodies too long / short)"""
        zo, zn = self.size_o[fo] // 4, self.size_n[fn] // 4
        if min(zo, zn) < SIMILAR_MIN_WORDS or max(zo, zn) > FUZZY_MAX_WORDS or max(zo, zn) > ORDER_SIZE_RATIO * min(zo, zn):
            return 0.0
        sm = difflib.SequenceMatcher(None, self._classes(0, fo), self._classes(1, fn), autojunk=False)
        return sm.ratio() if sm.quick_ratio() >= FUZZY_RATIO else 0.0

    def _gap_pairs(self, go, gn):
        """an unequal call gap (inlining moved calls in or out): each unpaired old callee with the one unpaired new
        callee whose shape matches it clearly best, the pairing one-to-one"""
        xs = [x for x in dict.fromkeys(go) if x not in self.fwd and self.size_o[x] // 4 <= GAP_MAX_WORDS]
        ys = [y for y in dict.fromkeys(gn) if y not in self.rev and self.size_n[y] // 4 <= GAP_MAX_WORDS]
        best = {}
        for x in xs:
            r = sorted(((self.shape_ratio(x, y), y) for y in ys), reverse=True) + [(0.0, None)]
            if r[0][0] >= FUZZY_RATIO and r[0][0] - r[1][0] >= GAP_MARGIN:
                best[x] = r[0][1]
        taken = Counter(best.values())
        return [(x, y) for x, y in best.items() if taken[y] == 1]

    def uniq_ok(self, x, y):
        """no other rule pairs x or y against its unique-body partner (match_hash), even one validate dropped: a small
        hash pair without an agreeing edge is dropped, and a vote or a run must not then pair its sides crosswise"""
        return self._uniq[0].get(x, y) == y and self._uniq[1].get(y, x) == x

    def _land(self, fvotes, voters, how):
        added = 0
        claims = defaultdict(set)
        for to, c in fvotes.items():
            if to in self.fwd:
                continue
            if len(c) == 1:
                (tn, _), = c.items()
                if tn not in self.rev and (to, tn) not in self.banned and self.uniq_ok(to, tn) and self.similar(to, tn):
                    claims[tn].add(to)
        for tn, tos in claims.items():
            if len(tos) == 1:
                to = tos.pop()
                self._pair(to, tn, how, voters[to, tn])
                added += 1
        return added

    # --- step 4: order
    def match_order(self):
        """Fill runs between monotone matched neighbours, then keep a run only if no call vote contradicts it."""
        pairs = sorted(self.fwd.items())
        mono = []
        for fo, fn in pairs:                    # longest increasing chain would be better; neighbours suffice here
            if not mono or fn > mono[-1][1]:
                mono.append((fo, fn))
        runs = []
        for (ao, an), (bo, bn) in zip(mono, mono[1:]):
            io, jo = bisect.bisect_right(self.starts_o, ao), bisect.bisect_left(self.starts_o, bo)
            i_n, j_n = bisect.bisect_right(self.starts_n, an), bisect.bisect_left(self.starts_n, bn)
            ro = [s for s in self.starts_o[io:jo] if s not in self.fwd]
            rn = [s for s in self.starts_n[i_n:j_n] if s not in self.rev]
            if not ro or len(ro) != len(rn) or len(ro) != jo - io or len(rn) != j_n - i_n:
                continue
            if any(x in self._no_order for x in ro):
                continue
            if all(max(self.size_o[x], self.size_n[y]) <= ORDER_SIZE_RATIO * min(self.size_o[x], self.size_n[y])
                   and self.uniq_ok(x, y) and self.similar(x, y) for x, y in zip(ro, rn)):
                runs.append(list(zip(ro, rn)))
        for run in runs:
            for x, y in run:
                self._pair(x, y, 'order')
        # contradiction check: a matched caller whose aligned call names a different function than an order pair
        bad = set()
        for fo in list(self.fwd):
            for to, tn in self._events(fo):
                if to in self.fwd and self.how.get(to) == 'order' and self.fwd[to] != tn and tn in self.size_n:
                    bad.add(to)
        kept = 0
        for run in runs:
            if any(x in bad for x, _ in run):
                for x, y in run:
                    del self.fwd[x], self.rev[y], self.how[x]
                    self._align_cache.pop(x, None)
                    self._no_order.add(x)
                self.dropped_order += len(run)
            else:
                kept += len(run)
        return kept

    # --- step 5: conflicts
    def _neighbours_agree(self, fo):
        """Both address-order neighbours of fo are paired with the neighbours of fo's match."""
        i = bisect.bisect_left(self.starts_o, fo)
        j = bisect.bisect_left(self.starts_n, self.fwd[fo])
        if i == 0 or j == 0 or i + 1 >= len(self.starts_o) or j + 1 >= len(self.starts_n):
            return False
        return (self.fwd.get(self.starts_o[i - 1]) == self.starts_n[j - 1]
                and self.fwd.get(self.starts_o[i + 1]) == self.starts_n[j + 1])

    def validate(self):
        """Every aligned function edge of every pair (a call, a tail call, a function pointer) must agree with the
        pairs at its two ends. A conflict drops the side with less agreement (both on a tie), and what only its votes
        landed; a small hash pair needs one agreeing edge or agreeing neighbours. Repeats to a fixpoint."""
        total = 0
        while True:
            conf, bad, edges = Counter(), Counter(), []
            for fo in list(self.fwd):
                for to, tn in self._events(fo):
                    if to == fo or to not in self.size_o or tn not in self.size_n:
                        continue
                    mo, mn = self.fwd.get(to), self.rev.get(tn)
                    if mo == tn:
                        conf[fo] += 1
                        conf[to] += 1
                    elif mo is not None or mn is not None:
                        other = to if mo is not None else mn
                        if other != fo:
                            edges.append((fo, other))
                            bad[fo] += 1
                            bad[other] += 1
            self.conf = conf
            drop = set()
            for p, q in edges:
                sp, sq = conf[p] - bad[p], conf[q] - bad[q]
                if sp <= sq:
                    drop.add(p)
                if sq <= sp:
                    drop.add(q)
            for fo, how in self.how.items():
                if (how == 'hash' and self.size_o[fo] < 4 * SMALL_WORDS and not conf[fo]
                        and not self._neighbours_agree(fo)):
                    drop.add(fo)
            n = len(self.fwd)
            for fo in drop:
                self.drop(fo)
            n -= len(self.fwd)
            total += n
            if not n:
                break
        self.dropped_conflict += total
        return total

    def confirmed(self, fo):
        """A pair with at least one agreeing edge, or whose neighbours agree: the only pairs `map` places code in."""
        return fo in self.fwd and (self.conf[fo] > 0 or self._neighbours_agree(fo))

    def _votes(self, rounds):
        for _ in range(rounds):
            if not self.vote_round():
                break

    def run(self, rounds=50):
        self.match_hash()
        self.match_anchors()
        self._votes(rounds)
        for _ in range(3):
            if not self.match_anchors():
                break
            self._votes(rounds)
        for _ in range(3):
            if not self.match_order():
                break
            self._votes(rounds)
        for _ in range(3):
            if not self.match_callee_sets():
                break
            self._votes(rounds)
        for _ in range(5):                     # conflicts out, then let the survivors vote again
            if not self.validate():
                break
            self._done.clear()
            self._votes(rounds)
        self.validate()
        # data, from the final pairs only (a dropped pair's references must not count)
        for fo in self.fwd:
            for to, tn in self._events(fo):
                to_in, tn_in = self._in_text(to, tn)
                if not to_in and not tn_in:
                    self.data_votes[to][tn] += 1
        for to, c in self.data_votes.items():
            (tn, top), *rest = c.most_common(2) + [(None, 0)]
            if top >= DATA_MIN_VOTES and top > 2 * rest[0][1]:
                self.data_map[to] = tn
        return self

    # --- queries
    def map(self, off: int) -> dict:
        r = dict(old=hex(off))
        if self.old.text_off <= off < self.old.text_end:
            fo = self.func_of(0, off)
            if fo is None:
                return dict(r, how=None, why='not inside any function (no FDE covers it)')
            r['func_old'] = hex(fo)
            if fo not in self.fwd:
                return dict(r, how=None, why='its function has no match')
            fn = self.fwd[fo]
            r['func_new'], r['func_how'] = hex(fn), self.how[fo]
            r['confirmed'], r['agreeing_edges'] = self.confirmed(fo), self.conf[fo]
            k = (off - fo) // 4
            for bo, bn, n in self.blocks(fo):
                if bo <= k < bo + n:
                    new = fn + 4 * (bn + k - bo) + (off & 3)
                    exact = self.hash_o[fo] == self.hash_n[fn]
                    wo = int(self.old.words[(off - self.old.text_off) // 4])
                    wn = int(self.new.words[(new - self.new.text_off) // 4])
                    return dict(r, new=hex(new), how='exact' if exact else 'aligned', same_word=wo == wn,
                                word_old=f'{wo:08x}', word_new=f'{wn:08x}')
            for bo, bn, n in self.fuzzy_blocks(fo):
                if bo <= k < bo + n:
                    new = fn + 4 * (bn + k - bo) + (off & 3)
                    io, jn = (off - self.old.text_off) // 4, (new - self.new.text_off) // 4
                    wo, wn = int(self.old.words[io]), int(self.new.words[jn])
                    return dict(r, new=hex(new), how='fuzzy', same_word=wo == wn,
                                same_norm=int(self.ao.norm[io]) == int(self.an.norm[jn]),
                                word_old=f'{wo:08x}', word_new=f'{wn:08x}')
            return dict(r, how=None, why='the instruction is in a changed part of its function')
        if off in self.data_map:
            return dict(r, new=hex(self.data_map[off]), how='data', votes=self.data_votes[off].most_common(3))
        votes = self.data_votes.get(off)
        if votes and len(votes) == 1:
            (tn, _), = votes.items()
            why = self.content_agrees(off, tn)
            if why:
                return dict(r, new=hex(tn), how='data1', confirmed_by=why)
        bound = next((k for k, v in (self.old.bounds or {}).items() if v == off), None)
        if bound and (self.new.bounds or {}).get(bound) is not None:
            return dict(r, new=hex(self.new.bounds[bound]), how='bound', bound=bound)
        for how, at in (('vtable', self.vtable_map(off)), ('string', self.string_map(off)), ('blob', self.blob_map(off))):
            if at is not None:
                return dict(r, new=hex(at), how=how)
        if votes:
            return dict(r, how=None, why='one reference its content does not confirm' if len(votes) == 1 and
                        sum(votes.values()) == 1 else 'data votes disagree', votes=votes.most_common(3))
        return dict(r, how=None, why='no matched code references it')

    def _table_len(self, rel, base):
        """Consecutive relocated slots from base (a vtable's body ends at the next table's offset-to-top word)."""
        n = 0
        while base + 8 * n in rel and n < 0x100:
            n += 1
        return n

    def tables_agree(self, off, tn):
        """Two pointer tables, old at off and new at tn: the same length, every paired old slot pointing at its pair,
        every unpaired old slot facing an unpaired new one. Returns the number of paired slots, or None."""
        ro, rn = self.old.relocs, self.new.relocs
        n = self._table_len(ro, off)
        if not n or self._table_len(rn, tn) != n:
            return None
        paired = 0
        for k in range(0, 8 * n, 8):
            a, b = ro[off + k], rn[tn + k]
            if a in self.fwd:
                if self.fwd[a] != b:
                    return None
                paired += 1
            elif b in self.rev:
                return None
        return paired

    def content_agrees(self, off, tn):
        """Second evidence for a data address only one reference names: a pointer table that agrees slot for slot
        (tables_agree, >= 2 paired slots), or the same 16 non-uniform bytes at both places."""
        paired = self.tables_agree(off, tn)
        if paired is not None and paired >= 2:
            return f'{paired} vtable slots'
        x, y = self.old.data[off:off + 16], self.new.data[tn:tn + 16]
        if paired is None and self._table_len(self.old.relocs, off) == 0 and len(x) == 16 and x == y and len(set(x)) > 2:
            return 'same 16 bytes'
        return None

    def vtable_map(self, off):
        """A pointer table no instruction references (an object's vtable the engine compares vptrs with): the one new
        table that agrees slot for slot (tables_agree) with at least 3 paired slots."""
        ro, rn = self.old.relocs, self.new.relocs
        n = self._table_len(ro, off)
        known = [(k, self.fwd[ro[off + k]]) for k in range(0, 8 * n, 8) if ro[off + k] in self.fwd]
        if len(known) < 3:
            return None
        if self._rn_by_target is None:
            self._rn_by_target = defaultdict(list)
            for slot, t in rn.items():
                self._rn_by_target[t].append(slot)
        k0, b0 = known[0]
        good = [c for c in (s - k0 for s in self._rn_by_target.get(b0, ())) if self.tables_agree(off, c) is not None]
        return good[0] if len(good) == 1 else None

    def blob_map(self, off):
        """Read-only data that is not a string (a float table, a constant block): the shortest of 16 / 32 / 64 bytes
        at off that occurs once in the old read-only data, found exactly once in the new. Changed content: None."""
        if not (self.old.ro and self.new.ro and self.old.ro[0] <= off < self.old.ro[1]):
            return None
        d, n = self.old.data, self.new.data
        for size in (16, 32, 64):
            blob = d[off:off + size]
            if len(blob) < size or off + size > self.old.ro[1] or len(set(blob)) <= 2:
                return None
            if d.count(blob, *self.old.ro) == 1:
                at = n.find(blob, *self.new.ro)
                return at if at >= 0 and n.find(blob, at + 1, self.new.ro[1]) < 0 else None
        return None

    def string_map(self, off):
        """A read-only C string (printable, >= STRING_MIN chars, NUL-terminated) at off: its one occurrence in the new
        read-only data, when it occurs exactly once in each build and both agree on whether a string starts there.
        For engine names of strings few instructions reference (the data vote needs DATA_MIN_VOTES)."""
        if not (self.old.ro and self.new.ro and self.old.ro[0] <= off < self.old.ro[1]):
            return None
        d, n = self.old.data, self.new.data
        end = d.find(b'\0', off, min(self.old.ro[1], off + 4096))
        if end - off < STRING_MIN or not all(32 <= b < 127 or b in (9, 10, 13) for b in d[off:end]):
            return None
        s = d[off:end + 1]
        if d.count(s, *self.old.ro) != 1:
            return None
        at = n.find(s, *self.new.ro)
        if at < 0 or n.find(s, at + 1, self.new.ro[1]) >= 0 or (d[off - 1] == 0) != (n[at - 1] == 0):
            return None
        return at

    def stats(self) -> dict:
        how = Counter(self.how.values())
        exact = sum(1 for fo, fn in self.fwd.items() if self.hash_o[fo] == self.hash_n[fn])
        return dict(funcs_old=len(self.starts_o), funcs_new=len(self.starts_n), matched=len(self.fwd),
                    matched_pct=round(100 * len(self.fwd) / max(1, len(self.starts_o)), 2), by_how=dict(how),
                    identical_bodies=exact, order_dropped=self.dropped_order, conflict_dropped=self.dropped_conflict,
                    unconfirmed=sum(1 for fo in self.fwd if not self.confirmed(fo)), data_mapped=len(self.data_map))


# ---------------------------------------------------------------------------------------------------------------- cli

def _addrs(args):
    out = {}
    if args.addrs:
        for a in args.addrs.split(','):
            out[a.strip()] = int(a, 0)
    if args.addrs_file:
        with open(args.addrs_file, encoding='utf-8') as f:
            for k, v in json.load(f).items():
                out[k] = int(v, 0) if isinstance(v, str) else v
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('cmd', choices=('stats', 'map'))
    ap.add_argument('--old', required=True, help='old main NSO (ExeFS main)')
    ap.add_argument('--new', required=True, help='new main NSO')
    ap.add_argument('--addrs', help='comma list of old main offsets')
    ap.add_argument('--addrs-file', help='JSON {name: offset}')
    ap.add_argument('--out', help='write the JSON result here')
    args = ap.parse_args(argv)
    p = Porter(load_image(args.old), load_image(args.new)).run()
    res = dict(stats=p.stats())
    if args.cmd == 'map':
        res['map'] = {k: p.map(v) for k, v in _addrs(args).items()}
        res['unmapped'] = sorted(k for k, v in res['map'].items() if v.get('how') is None)
    text = json.dumps(res, indent=1)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(text + '\n')
    print(text if args.cmd == 'stats' or not args.out else json.dumps(dict(stats=res['stats'],
                                                                          unmapped=res['unmapped'])))
    return 1 if args.cmd == 'map' and res['unmapped'] else 0


if __name__ == '__main__':
    sys.exit(main())
