"""python -m games.totk.overlays --romfs <work>/extracted/v100 --out <work>/overlays [--outputs ...] [--aspects ...]

Writes base/, skip_opening/ (a new game starts at the Great Sky Island; TOTK_SKIP_DIR), one output-<w>x<h>/ per --outputs size, one output-<w>x<h>-scene-<w>x<h>/ per output (1920x1080
included) and scene size above 1600x900 (--scenes, plus the output's own size), one aspect-<w>x<h>/ per --aspects
size, one grass-<percent>/ per --grass, one shadow-<percent>/ per --shadow and one fog-<percent>/ per --fog multiplier,
fasttravel/ (or fasttravel-<variant>/) per --fast-travel variant, and with --texture-cache every
output set again as output-<w>x<h>[-scene-<w>x<h>]-tex<percent>/ (see __init__.py). The
runtime asks for output-<max>-scene-<render> whenever the render size is above 1600x900 and falls back to the largest
built one, so build every output / scene pair that is played.
Prints one JSON line per set; exit 1 when any edit's check fails (nothing is half-written: each set is written only
after all its edits passed)."""
import argparse
import json
import sys
from pathlib import Path

from . import (FAST_TRAVEL, SCENE_W, SCENE_H, RomFS, Zstd, build_base, build_fast_travel, build_fog, build_grass,
               build_output, build_shadow, build_skip_opening, parse_size)

# 1440p and 4K 16:9; the ultrawide (21:9, 32:9, 48:9), 16:10 and 3:2 canvases the aspect sets lay the UI out for, and
# 1920x1200 / 1920x1280, the canvases graphics.aspect_ratio = 16:10 / 3:2 draw (games/totk/engine/totk_engine.c
# wide_canvas; their layouts are 2560x1600's / 2160x1440's).
DEFAULT_OUTPUTS = '2560x1440,3840x2160,3440x1440,5120x1440,7680x1440,2560x1600,1920x1200,1920x1280'
DEFAULT_ASPECTS = '3440x1440,5120x1440,7680x1440,2560x1600,2160x1440'
# Render sizes for the output-<w>x<h>-scene-<w>x<h> sets; each output also gets a scene of its own size. 1600x1000 /
# 1620x1080: the 16:10 / 3:2 canvases' scenes.
DEFAULT_SCENES = '1920x1080,2560x1440,3840x2160,1600x1000,1620x1080'


def scene_pairs(outputs, scenes):
    """(output, scene) for every output plus 1920x1080 and every scene (plus the output's own size) above 1600x900."""
    pairs = []
    for out in dict.fromkeys(['1920x1080', *outputs]):
        for scene in dict.fromkeys([*(s for s in scenes.split(',') if s), out] if scenes else []):
            w, h = parse_size(scene)
            if w * h > SCENE_W * SCENE_H and (out, scene) not in pairs:
                pairs.append((out, scene))
    return pairs


def main(argv=None):
    p = argparse.ArgumentParser(prog='python -m games.totk.overlays', description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', required=True, type=Path,
                   help='folder with romfs-index.json + romfs.bin (pipeline/extract.py), or an unpacked RomFS tree')
    p.add_argument('--out', required=True, type=Path, help='<work>/overlays')
    p.add_argument('--outputs', default=DEFAULT_OUTPUTS, help='output sizes for HeapDef sets ("" for none)')
    p.add_argument('--aspects', default=DEFAULT_ASPECTS, help='canvas sizes for UI layout sets ("" for none)')
    p.add_argument('--scenes', default=DEFAULT_SCENES,
                   help='render sizes for output-<w>x<h>-scene-<w>x<h> sets, built for every output and 1920x1080 ("" for none)')
    p.add_argument('--hud', default='corner',
                   help='HUD placements for the wide aspect sets: corner (aspect-<w>x<h>), center (aspect-<w>x<h>-center, '
                        'the HUD in the middle 16:9; graphics.hud_layout), or corner,center for both')
    p.add_argument('--grass', default='', help='grass density multipliers for grass-<percent> sets, e.g. 2,3 ("" for none)')
    p.add_argument('--grass-noturf', default='', help='the same as grass-<percent>-noturf sets: the ground carpet (TurfSet) '
                   'left as shipped (a test candidate for public issue #16; Setup does not build them)')
    p.add_argument('--shadow', default='', help='shadow cascade distance multipliers for shadow-<percent> sets, e.g. 2 ("" for none)')
    p.add_argument('--fog', default='', help='distance fog multipliers for fog-<percent> sets, e.g. 1.5,2 ("" for none)')
    p.add_argument('--texture-cache', default='',
                   help='texture cache multipliers (1..3), e.g. 2: every HeapDef set again as ...-tex<percent>, '
                        '1920x1080 included ("" for none)')
    p.add_argument('--fast-travel', default='waits',
                   help=f'shorter fast-travel departure variants ({", ".join(FAST_TRAVEL)}; "" for none): waits -> fasttravel/')
    p.add_argument('--only-fast-travel', action='store_true', help='build the --fast-travel sets alone')
    p.add_argument('--capture-frame', type=float, default=0.0)
    p.add_argument('--only-skip-opening', action='store_true', help='build skip_opening/ alone (tools/engine/build.py overlay)')
    p.add_argument('--only-aspects', action='store_true',
                   help='build the --aspects / --hud sets alone (a shape built on demand: tools/engine/aspect_sets.py)')
    args = p.parse_args(argv)
    romfs = RomFS(args.romfs)
    zstd = Zstd(romfs)
    failures = 0
    jobs = [lambda: build_base(romfs, zstd, args.out, capture_frame=args.capture_frame),
            lambda: build_skip_opening(romfs, zstd, args.out)]
    outputs = [s for s in args.outputs.split(',') if s]
    jobs += [lambda s=s: build_output(romfs, zstd, args.out, s) for s in outputs]
    jobs += [lambda s=s, c=c: build_output(romfs, zstd, args.out, s, c) for s, c in scene_pairs(outputs, args.scenes)]
    for x in (float(x) for x in args.texture_cache.split(',') if x):
        jobs += [lambda s=s, x=x: build_output(romfs, zstd, args.out, s, tex=x) for s in dict.fromkeys(['1920x1080', *outputs])]
        jobs += [lambda s=s, c=c, x=x: build_output(romfs, zstd, args.out, s, c, x)
                 for s, c in scene_pairs(outputs, args.scenes)]
    jobs += [lambda g=g: build_grass(romfs, zstd, args.out, float(g)) for g in args.grass.split(',') if g]
    jobs += [lambda g=g: build_grass(romfs, zstd, args.out, float(g), turf=False) for g in args.grass_noturf.split(',') if g]
    jobs += [lambda d=d: build_shadow(romfs, zstd, args.out, float(d)) for d in args.shadow.split(',') if d]
    jobs += [lambda d=d: build_fog(romfs, zstd, args.out, float(d)) for d in args.fog.split(',') if d]
    huds = [h for h in args.hud.split(',') if h]
    if not huds or set(huds) - {'corner', 'center'}:
        p.error(f'--hud {args.hud!r}: corner, center or corner,center')

    def wide(s):
        w, h = parse_size(s)
        return w * 9 > h * 16
    aspects = [lambda s=s, h=h: build_base(romfs, zstd, args.out, aspect=s, hud=h, capture_frame=args.capture_frame)
               for s in args.aspects.split(',') if s for h in huds if h == 'corner' or wide(s)]
    jobs += aspects
    fast = [lambda v=v: build_fast_travel(romfs, zstd, args.out, v) for v in args.fast_travel.split(',') if v]
    jobs += fast
    if args.only_skip_opening:
        jobs = [lambda: build_skip_opening(romfs, zstd, args.out)]
    if args.only_fast_travel:
        jobs = fast
    if args.only_aspects:
        jobs = aspects
    for job in jobs:
        try:
            print(json.dumps(dict(status='pass', **job())), flush=True)
        except (ValueError, KeyError, AssertionError, SystemExit) as e:
            failures += 1
            print(json.dumps(dict(status='fail', problem=f'{type(e).__name__}: {e}')), flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
