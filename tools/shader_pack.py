"""Archive precompile pack: every vertex + fragment program pair of the game's shader archives, for the engine's
archive warm-up (engine/runtime/nvn/vulkan/exec.c pa_load / pa_job: cache/archive_shaders.bin, or the package's
cache-seed/archive_shaders.bin).

  python tools/shader_pack.py --romfs <romfs> --out <file> [--jobs N] [--check <cache dir> ...]

Programs come from the BNSH program variations (the vertex and fragment shader the game binds together): the material,
terrain, grass, cave, ... archives (Shader/*.bfsha) and every effect (Effect/*.esetb). Each stage is stored as the bytes
the game hands the GPU (code up to the compiler constants' offset, then the constants), so the warm-up computes the
same code hashes and content keys (pc_prog_key) as the game's own draws, and translates with the constants (BRX tables).

The shader key: the vertex attribute kinds come from the vertex shader's program header (input map). In the material
archives attributes 6 and 7, when the shader reads them, are integer (skinning indices); everything else is float, as
in every program recorded so far (2026-10-30: 3,454 recorded programs, 2,354 vertex shaders, each one with a single
attribute pattern; --check compares a pack with recorded caches). Render-target kinds are float (7 of 3,454 recorded
programs write an integer target; those come from the recordings).

File (little-endian), version 2:
  u32 'TKSP', u32 2, u32 nblobs, u32 nprogs
  nblobs x {u32 offset, u32 code size (constants offset), u32 constants size, u32 0}   (offsets from the code region)
  nprogs x {u32 vs blob, u32 fs blob, u8 attr_kind[32], u32 source}                    (44 bytes)
  u32 nsources, nsources x char[64]
  u32 ncompute, ncompute x {u32 cs blob, u32 workgroup size[3]}       (the BNSH compute shaders: 145, the game's 70
                                                                       recorded ones among them)
  code region
Everything in it is game-derived: it is written to the work folder or a package, never to the repository.
"""
import argparse
import concurrent.futures as cf
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'xlate'))
import prepare_seed as ps   # (containers: zstd with the game's dictionaries, SARC, BNSH)

M64 = (1 << 64) - 1
SCAN_DIRS = ('Shader', 'Effect')


def hash_words(h, b):
    """exec.c hash_words (program code hashes, precompile.bin records)"""
    n = len(b) // 8
    for (w,) in struct.iter_unpack('<Q', b[:n * 8]):
        h = ((h ^ w) * 0x9E3779B97F4A7C15) & M64
        h ^= h >> 29
    for x in b[n * 8:]:
        h = ((h ^ x) * 0x100000001B3) & M64
    return h ^ (h >> 32)


def stage_blob(data, offset, want):
    """(code size, constants size, bytes) of one BNSH stage as the game binds it (objects.c: const_offset = control
    0x700, const_size = control 0x6FC; exec.c caps the constants at 64 KB), or None. want: 0 vertex, 1 fragment, 5 compute
    (then (blob, workgroup size): control 0x748, as exec.c compute_program reads it)"""
    control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', ps.checked(data, offset + 8, 24))
    control = ps.checked(data, control_off, control_size)
    code = ps.checked(data, code_off, code_size)
    if len(control) < 0x7cc or struct.unpack_from('<I', control, 0x714)[0] != want:
        return None
    c1, csize = struct.unpack_from('<I', control, 0x700)[0], struct.unpack_from('<I', control, 0x6fc)[0]
    csize = min(csize, 0x10000)
    if c1 < 0x80 or c1 + csize > len(code) or not ps.plausible({0: 'vs', 1: 'ps', 5: 'cs'}[want], control, code):
        return None
    blob = c1, csize, bytes(code[:c1 + csize])
    if want == 5:
        return blob, tuple(max(1, v) for v in struct.unpack_from('<3I', control, 0x748))
    return blob


def bnsh_programs(data):
    """('g', vs blob, fs blob) for every program variation of one BNSH with both stages, ('c', cs blob, workgroup size)
    for every compute one"""
    if data[:4] != b'BNSH' or ps.checked(data, 12, 2) != b'\xff\xfe' or ps.checked(data, 96, 4) != b'grsc':
        return
    size = struct.unpack_from('<I', data, 28)[0]
    data = ps.checked(data, 0, size)
    count, variations = struct.unpack_from('<IQ', data, 124)
    ps.checked(data, variations, count * 64)
    for i in range(count):
        program = struct.unpack_from('<Q', data, variations + i * 64 + 16)[0]
        ps.checked(data, program, 56)
        vs_off, fs_off, cs_off = (struct.unpack_from('<Q', data, program + o)[0] for o in (8, 40, 48))
        if cs_off:
            cs = stage_blob(data, cs_off, 5)
            if cs:
                yield 'c', cs[0], cs[1]
        if not vs_off or not fs_off:
            continue
        vs, fs = stage_blob(data, vs_off, 0), stage_blob(data, fs_off, 1)
        if vs and fs:
            yield 'g', vs, fs


def found_blob(found, blob):   # (a worker's blobs, deduplicated: effects repeat the same stages many times)
    k = (blob[0], ps.sha(blob[2]))
    found.blobs.setdefault(k, blob)
    return k


class Found(list):
    def __init__(self):
        super().__init__()
        self.blobs = {}


def visit(name, data, found, decode, depth=0):
    if data[:4] == b'SARC':
        if depth <= 4:
            for member, raw in ps.sarc(data).items():
                visit(name + '/' + member, decode(raw), found, decode, depth + 1)
        return
    pos = 0
    while (pos := data.find(b'BNSH\0\0\0\0', pos)) >= 0:
        size = struct.unpack_from('<I', ps.checked(data, pos + 28, 4))[0]
        for kind, a, b in bnsh_programs(ps.checked(data, pos, size)):
            found.append((kind, name, found_blob(found, a), b if kind == 'c' else found_blob(found, b)))
        pos += max(size, 8)


def scan(args):
    """worker: (romfs, [relative paths]) -> ([(kind, source, blob key, blob key or workgroup size)], blobs, errors)"""
    romfs, names = args
    ps.set_priority()
    decode = ps._decoder(romfs)
    found, errors = Found(), []
    for name in names:
        try:
            raw = (Path(romfs) / name).read_bytes()
            if raw[:4] not in (ps.ZSTD, b'SARC', b'BNSH', b'FSHA') and b'BNSH\0\0\0\0' not in raw:
                continue
            visit(name, decode(raw), found, decode)
        except Exception as e:   # (one bad file must not hide the others)
            errors.append(f'{name}: {type(e).__name__}: {e}')
    return list(found), found.blobs, errors


def attr_kinds(source, vs_code):
    """the vertex attribute kinds (0 float, 2 unsigned int) the game's draws use with this vertex shader"""
    imap = vs_code[0x30 + 24:0x30 + 40]   # (shader program header after the 0x30-byte NVN header: ImapGenericVector)
    used = [(imap[i // 2] >> (4 * (i % 2))) & 15 for i in range(32)]
    kinds = bytearray(32)
    if source.split('/')[-1].startswith('material') or '/material.' in source:
        for i in (6, 7):
            if used[i]:
                kinds[i] = 2
    return bytes(kinds)


def source_rank(source):
    top = source.split('/')[0]
    return (0 if top == 'Shader' and 'material' in source else 1 if top == 'Shader' else 2, source)


def build(found, all_blobs):
    """deduplicated blobs + programs (Shader/material first) + compute programs"""
    blobs, blob_ix, progs, seen, sources, source_ix, comps = [], {}, [], set(), [], {}, []

    def blob(k):
        if k not in blob_ix:
            blob_ix[k] = len(blobs)
            blobs.append(all_blobs[k])
        return blob_ix[k]
    for kind, source, vk, fk in sorted(found, key=lambda f: source_rank(f[1])):
        if kind == 'c':
            c = (blob(vk), fk)
            if c not in seen:
                seen.add(c)
                comps.append(c)
            continue
        ix = [blob(vk), blob(fk)]
        kinds = attr_kinds(source, all_blobs[vk][2])
        if (ix[0], ix[1], kinds) in seen:
            continue
        seen.add((ix[0], ix[1], kinds))
        file = source.split('/')[0] + '/' + source.split('/')[1] if '/' in source else source
        if file not in source_ix:
            source_ix[file] = len(sources)
            sources.append(file)
        progs.append((ix[0], ix[1], kinds, source_ix[file]))
    return blobs, progs, sources, comps


def write(path, blobs, progs, sources, comps):
    tmp = str(path) + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(struct.pack('<4I', 0x50534b54, 2, len(blobs), len(progs)))
        at = 0
        for c1, csize, b in blobs:
            f.write(struct.pack('<4I', at, c1, csize, 0))
            at += len(b)
        for vs, fs, kinds, src in progs:
            f.write(struct.pack('<II', vs, fs) + kinds + struct.pack('<I', src))
        f.write(struct.pack('<I', len(sources)))
        for s in sources:
            f.write(s.encode('utf-8')[:63].ljust(64, b'\0'))
        f.write(struct.pack('<I', len(comps)))
        for b, local in comps:
            f.write(struct.pack('<4I', b, *local))
        for _, _, b in blobs:
            f.write(b)
    os.replace(tmp, path)


def check(blobs, progs, comps, caches):
    """how many recorded programs the pack holds, with the recorded attribute kinds"""
    h = [hash_words(0x6a09e667f3bcc908 ^ c1, b[:c1]) for c1, _, b in blobs]
    pairs = {}
    for vs, fs, kinds, _ in progs:
        pairs.setdefault((h[vs], h[fs]), set()).add(kinds)
    cs = {(h[b], local) for b, local in comps}
    for c in caches:
        pb = os.path.join(c, 'precompile.bin')
        if not os.path.exists(pb):
            print(f'{c}: no precompile.bin')
            continue
        d = open(pb, 'rb').read()
        o, n, have, same, nc, hc = 0, 0, 0, 0, 0, 0
        done = set()
        while o + 16 <= len(d):
            magic, kind, size, _ = struct.unpack_from('<4I', d, o)
            if magic != 0x31435054 or o + 16 + size > len(d):
                break
            if kind in (2, 4):
                ck, h0, h1 = struct.unpack_from('<3Q', d, o + 16)
                if ck not in done:
                    done.add(ck)
                    n += 1
                    if (h0, h1) in pairs:
                        have += 1
                        same += d[o + 16 + 40:o + 16 + 72] in pairs[(h0, h1)]
            elif kind == 5:
                ck, ch, _, _, lx, ly, lz = struct.unpack_from('<4Q3I', d, o + 16)
                if ck not in done:
                    done.add(ck)
                    nc += 1
                    hc += (ch, (lx, ly, lz)) in cs
            o += 16 + size
        print(f'{c}: {n} recorded programs: {have} in the pack ({100 * have / max(1, n):.1f} %), '
              f'{same} with the recorded attribute kinds; {nc} recorded compute programs: {hc} in the pack')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs', type=Path, default=ps.WORK / 'extracted' / 'v100' / 'romfs')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--check', nargs='*', default=[], metavar='CACHE', help='compare with recorded caches (precompile.bin)')
    a = ap.parse_args()
    names = []
    for d in SCAN_DIRS:
        for root, _, files in os.walk(a.romfs / d):
            names += [os.path.relpath(os.path.join(root, f), a.romfs).replace('\\', '/') for f in files]
    names.sort()
    t0 = time.monotonic()
    chunks = [names[i::a.jobs * 8] for i in range(a.jobs * 8)]
    found, all_blobs, errors = [], {}, []
    with cf.ProcessPoolExecutor(max_workers=a.jobs) as pool:
        for f, b, e in pool.map(scan, [(str(a.romfs), c) for c in chunks]):
            found += f
            for k, v in b.items():
                all_blobs.setdefault(k, v)
            errors += e
    blobs, progs, sources, comps = build(found, all_blobs)
    write(a.out, blobs, progs, sources, comps)
    print(f'{len(names)} files, {len(found)} program variations -> {len(progs)} programs, {len(comps)} compute, {len(blobs)} stage blobs '
          f'({sum(len(b) for _, _, b in blobs) / 1e6:.1f} MB), {len(sources)} sources, {len(errors)} unreadable, '
          f'{time.monotonic() - t0:.0f} s -> {a.out}')
    for e in errors[:5]:
        print('  ' + e)
    if a.check:
        check(blobs, progs, comps, a.check)


if __name__ == '__main__':
    main()
