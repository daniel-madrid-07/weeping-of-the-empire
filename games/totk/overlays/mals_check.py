"""The mals set's patchers on every language, nothing written: their own checks (options_menu.check_captions: a caption
line wider than the game's in that language,...) in seconds. One line per failing language, exit 1 if any.

    python -m games.totk.overlays.mals_check --romfs <work>/extracted/v100"""
import argparse
import sys
from pathlib import Path

from . import RomFS, Zstd, sarc_members
from .mals import LANGS, archive, patches_of


def problems(romfs, zstd, langs=LANGS):
    out = []
    for lang in langs:
        try:
            patches = patches_of(lang)
            if not patches:
                continue
            sarc = bytes(zstd.decompress(romfs.read(archive(lang)))[0])
            where = sarc_members(sarc)
            for msbt, fns in patches.items():
                lo, hi = where[msbt]
                data = sarc[lo:hi]
                for _, fn in fns:
                    data = fn(data)
        except Exception as e:   # (every language's first failure, not only the first language's)
            out.append(f'{lang}: {type(e).__name__}: {e}')
    return out


def main(argv=None):
    p = argparse.ArgumentParser(prog='python -m games.totk.overlays.mals_check', description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', required=True, type=Path)
    a = p.parse_args(argv)
    romfs = RomFS(a.romfs)
    bad = problems(romfs, Zstd(romfs))
    for b in bad:
        print(b)
    print(f'mals check: {len(LANGS) - len(bad)}/{len(LANGS)} languages pass')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
