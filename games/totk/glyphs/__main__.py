"""python -m games.totk.glyphs --romfs <work>/extracted/v100 --overlays <work>/overlays --art <kenney dir>
                               [--styles xbox,ps,kbm] [--sets base,aspect-3440x1440] [--preview <dir>]
python -m games.totk.glyphs --romfs <work>/extracted/v100 --art <kenney dir> --bank <work>/engine/glyphs/engine+glyphs
                               --common <work>/engine/overlay/romfs/UI/LayoutArchive/Common.Product.100.Nin_NX_NVN.blarc.zs
                               [--preview <dir>]

--bank builds the glyph bank (bank.py: every prompt in the game, switched by the engine to the inputs bound) on the
engine overlay's Common archive (--common); the engine prefers it to the fixed sets below.

Builds <set>+glyphs-<style>/ next to every layout set in --overlays (the sets carrying the Common archive: base,
aspect-*; build those first with python -m games.totk.overlays). --art is the unzipped Kenney "Input Prompts" 1.5
(CC0: https://kenney.nl/assets/input-prompts). --preview writes original | replacement PNGs of every mask (game-derived:
keep them in <work>). Prints one JSON line per set; exit 1 when any check fails (a set is written only after all its
checks passed). Needs zstandard, numpy and Pillow."""
import argparse
import json
import sys
from pathlib import Path

from games.totk.overlays import RomFS, Zstd

from . import STYLES, build_variant, layout_sets


def main(argv=None):
    p = argparse.ArgumentParser(prog='python -m games.totk.glyphs', description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', required=True, type=Path, help='romfs-index.json + romfs.bin, or an unpacked RomFS tree')
    p.add_argument('--overlays', type=Path, help='<work>/overlays (the layout sets are read from here)')
    p.add_argument('--bank', type=Path, help='build the glyph bank into this folder (engine+glyphs) instead')
    p.add_argument('--common', type=Path, help='--bank: the Common archive (zstd) it is built on')
    p.add_argument('--rstb', type=Path, help="--bank: the resource size table it raises (default: the one of --common's "
                                             "overlay, else the game's)")
    p.add_argument('--art', required=True, type=Path, help='the unzipped Kenney Input Prompts folder')
    p.add_argument('--styles', default=','.join(STYLES))
    p.add_argument('--sets', default='', help='layout sets to build variants of (default: every one found)')
    p.add_argument('--preview', type=Path)
    a = p.parse_args(argv)
    romfs = RomFS(a.romfs)
    zstd = Zstd(romfs)
    if a.bank:
        from .bank import build_bank
        if not a.common:
            p.error('--bank needs --common')
        try:
            rstb = a.rstb or a.common.parents[2] / 'System/Resource/ResourceSizeTable.Product.100.rsizetable.zs'
            print(json.dumps(dict(status='pass', **build_bank(romfs, zstd, a.common.read_bytes(), a.art, a.bank, a.preview,
                                                              rstb.read_bytes() if rstb.exists() else None))))
            return 0
        except (ValueError, KeyError, AssertionError, OSError) as e:
            print(json.dumps(dict(status='fail', set=str(a.bank), error=str(e))))
            return 1
    if not a.overlays:
        p.error('--overlays is needed (or --bank)')
    sets = [s for s in a.sets.split(',') if s] or layout_sets(a.overlays)
    if not sets:
        print('no layout set (base, aspect-*) in', a.overlays, file=sys.stderr)
        return 1
    failures = 0
    for base in sets:
        for style in [s for s in a.styles.split(',') if s]:
            try:
                print(json.dumps(dict(status='pass', **build_variant(romfs, zstd, a.overlays, base, style, a.art, a.preview))), flush=True)
            except (ValueError, KeyError, AssertionError, OSError) as e:
                failures += 1
                print(json.dumps(dict(status='fail', set=f'{base}+glyphs-{style}', error=str(e))), flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
