"""One TOTK_OVERLAY directory for engine (totk-recomp) from our overlay sets.

    python -m games.totk.overlays.engine_dir --romfs <work>/extracted/v100 --sets base,grass-200,fog-200 \
        [--sets-root <work>/overlays] [--engine <work>/engine/overlay/romfs] --out <dir>

engine serves one overlay directory (TOTK_OVERLAY, files at the romfs root) and it overrides TOTK_MODS, so our sets go
into a copy of the engine's own overlay rather than into the mods list. the engine's overlay holds the game-over and quick-menu
fixes (the same two files our `base` set makes: checked, equal once decompressed) and its RSTB with slack. The result:
- the engine's files, then each set's files in order; a set may replace the engine's two fix files only (base / aspect sets are
  those fixes plus their own edits); two of our sets giving one path differently is refused;
- every file is checked against its set's manifest (sha256), and against the original it was made from;
- the RSTB (the game sizes each resource's heap from it) is raised for any file that decompresses bigger than its
  entry allows, by the growth; untouched otherwise.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
import zlib
from pathlib import Path

from . import RomFS, Zstd

RSTB = 'System/Resource/ResourceSizeTable.Product.100.rsizetable.zs'
ENGINE_FIXES = ('UI/LayoutArchive/Common.Product.100.Nin_NX_NVN.blarc.zs', 'Event/EventFlow/DmF_SY_GameOver.bfevfl.zs')


def rstb_entries(d):
    """{crc32 or name: (offset of the size field)} of a decompressed RESTBL."""
    if d[:6] != b'RESTBL':
        raise ValueError('not a RESTBL')
    _, slen, ncrc, nname = struct.unpack_from('<4I', d, 6)
    out, o = {}, 22
    for _ in range(ncrc):
        crc, = struct.unpack_from('<I', d, o)
        out[crc] = o + 4
        o += 8
    for _ in range(nname):
        out[bytes(d[o:o + slen]).split(b'\0')[0].decode()] = o + slen
        o += slen + 4
    return out


def rstb_key(path):
    for sfx in ('.zs', '.mc'):
        if path.endswith(sfx):
            path = path[:-len(sfx)]
    return path


def build(romfs_root, sets, sets_root, engine_dir, out):
    romfs = RomFS(romfs_root)
    zs = Zstd(romfs)
    out, engine_dir, sets_root = Path(out), Path(engine_dir), Path(sets_root)
    tmp = out.with_name(out.name + '.tmp')
    shutil.rmtree(tmp, ignore_errors=True)
    files = {}                                         # path -> (source file, origin)
    for p in engine_dir.rglob('*'):
        if p.is_file():
            files[p.relative_to(engine_dir).as_posix()] = (p, 'engine')
    for s in sets:
        man = sets_root / s / 'manifest.tsv'
        for line in man.read_text(encoding='utf-8').splitlines():
            if not line or line.startswith('#'):
                continue
            path, size, digest, orig = line.split('\t')[:4]
            src = sets_root / s / path
            data = src.read_bytes()
            if hashlib.sha256(data).hexdigest() != digest or len(data) != int(size):
                raise ValueError(f'{src}: does not match {man}')
            if hashlib.sha256(romfs.read(path)).hexdigest() != orig:
                raise ValueError(f'{path}: set {s} was made from another original')
            if path in files:
                prev, origin = files[path]
                if origin == 'engine' and path not in ENGINE_FIXES:
                    raise ValueError(f'{path}: engine ships its own version; set {s} may not replace it')
                if origin != 'engine' and prev.read_bytes() != data:
                    raise ValueError(f'{path}: sets {origin} and {s} both change it')
            files[path] = (src, s)
    # the RSTB: the engine's copy, raised where one of our files grew past its entry
    rpath = files[RSTB][0] if RSTB in files else None
    rstb, dict_id = zs.decompress((rpath.read_bytes() if rpath else romfs.read(RSTB)))
    entries = rstb_entries(rstb)
    raised = []
    for path, (src, origin) in sorted(files.items()):
        if origin == 'engine' or not path.endswith('.zs'):
            continue
        ours, _ = zs.decompress(src.read_bytes())
        base, _ = zs.decompress(romfs.read(path))
        grow = len(ours) - len(base)
        if grow <= 0:
            continue
        key = rstb_key(path)
        at = entries.get(zlib.crc32(key.encode()), entries.get(key))
        if at is None:
            continue                                   # not sized by the table
        cur, = struct.unpack_from('<I', rstb, at)
        struct.pack_into('<I', rstb, at, cur + grow)
        raised.append(dict(path=key, entry=cur, raised_by=grow))
    tmp.mkdir(parents=True)
    rows = []
    for path, (src, origin) in sorted(files.items()):
        dst = tmp / path
        dst.parent.mkdir(parents=True, exist_ok=True)
        if path == RSTB and raised:
            dst.write_bytes(zs.compress(rstb, dict_id))
            origin += ' + raised'
        else:
            shutil.copyfile(src, dst)
        rows.append(f'{path}\t{origin}\t{hashlib.sha256(dst.read_bytes()).hexdigest()}')
    with open(tmp / 'manifest.tsv', 'w', newline='\n', encoding='utf-8') as f:
        f.write('# path\tfrom\tsha256 (python -m games.totk.overlays.engine_dir; sets: ' + ','.join(sets) + ')\n')
        f.write('\n'.join(rows) + '\n')
    shutil.rmtree(out, ignore_errors=True)
    os.replace(tmp, out)
    return dict(status='pass', out=str(out), sets=sets, files=len(files), rstb_raised=raised)


def main(argv=None):
    p = argparse.ArgumentParser(prog='python -m games.totk.overlays.engine_dir', description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', required=True, type=Path)
    p.add_argument('--sets', required=True, help='comma list of set names under --sets-root, in order')
    p.add_argument('--sets-root', type=Path, default=Path(r'C:\nxrecomp-work\overlays'))
    p.add_argument('--engine', type=Path, default=Path(r'C:\nxrecomp-work\engine\overlay\romfs'))
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args(argv)
    try:
        r = build(a.romfs, [s for s in a.sets.split(',') if s], a.sets_root, a.engine, a.out)
    except (ValueError, OSError, KeyError) as e:
        print(json.dumps(dict(status='refused', problem=str(e))))
        return 1
    print(json.dumps(r))
    return 0


if __name__ == '__main__':
    sys.exit(main())
