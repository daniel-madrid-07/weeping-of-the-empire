"""python -m games.totk.patches {apply,list,generate,check,port} (see __init__.py, port.py)."""
import argparse
import json
import os
import sys
from pathlib import Path

from . import (DEFAULT_FEATURES, TABLE_V100, PatchError, apply, default_image, generate_toml, load_main_image,
               read_table, resolve, resolve_features)


def load_image_any(path):
    """a flat image, or an NSO decoded (another build's: no hash check)"""
    raw = Path(path).read_bytes()
    if raw[:4] == b'NSO0':
        from recomp.nso import load_nso
        raw = load_nso(str(path), 'main').data
    return bytearray(raw)


def _write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    if isinstance(data, str):
        with open(tmp, 'w', newline='\n', encoding='utf-8') as f:
            f.write(data)
    else:
        tmp.write_bytes(data)
    os.replace(tmp, path)


def main(argv=None):
    p = argparse.ArgumentParser(prog='python -m games.totk.patches', description=__doc__)
    sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('apply', help='patch a copy of the 1.0.0 main image')
    a.add_argument('--image', required=True, type=Path, help='<work>/images/main.img or extracted exefs/main (NSO)')
    a.add_argument('--out', required=True, type=Path, help='patched flat image; <out>.patches.json is written next to it')
    a.add_argument('--features', default='default',
                   help=f'default ({",".join(DEFAULT_FEATURES)}), all, none, or a comma list; -name drops one')
    a.add_argument('--table', type=Path, default=TABLE_V100)
    # 'sigma': the pre-rename word, kept as an alias until 2026-10-24 (tools/engine/build.py's text still names it)
    g = sub.add_parser('engine', aliases=['sigma'],
                       help="patch the engine's main.bin and extend its funcs JSON with the veneer ranges")
    g.add_argument('--image', required=True, type=Path, help="the engine's data/img/main.bin (nso.py's flat image)")
    g.add_argument('--funcs', required=True, type=Path, help="funcs.py's JSON for that image (its `fde` map)")
    g.add_argument('--out', required=True, type=Path, help="directory: main.bin, funcs_main.json, patchset.json, engine-patches.json (tools/engine/build.py --patches)")
    g.add_argument('--features', default='default',
                   help=f'default ({",".join(DEFAULT_FEATURES)}, plus the engine-only vk_guard), all, none, or a comma '
                        'list; -name drops one')
    g.add_argument('--table', type=Path, default=TABLE_V100)
    sub.add_parser('list', help='the features and how many words each patches')
    for name, what in (('generate', 'write v100.toml from sites_v100.py'),
                       ('check', 'fail when v100.toml differs from what sites_v100.py generates')):
        c = sub.add_parser(name, help=what)
        c.add_argument('--image', type=Path, help='the 1.0.0 main (flat image or NSO) whose words the table checks '
                                                  '(default: <work>/engine/img/<id>/main.bin)')
    o = sub.add_parser('port', help="move the 1.0.0 set to another build of main (port.py): writes e.g. v143.toml")
    o.add_argument('--old', required=True, type=Path, help='the 1.0.0 exefs/main (NSO)')
    o.add_argument('--new', required=True, type=Path, help="the other build's exefs/main (NSO)")
    o.add_argument('--version', required=True, help='its display version, e.g. 1.4.3')
    o.add_argument('--out', required=True, type=Path)
    o.add_argument('--report', type=Path, help="the porter's placement of every 1.0.0 site (JSON)")
    d = sub.add_parser('diff', help="where a table's features differ between two images (a refusal's detail: the "
                                    "offsets whose words differ from the reference build's)")
    d.add_argument('--image', required=True, type=Path, help='the image apply refused (flat image or NSO)')
    d.add_argument('--reference', required=True, type=Path, help='the image the table was made from (flat or NSO)')
    d.add_argument('--table', type=Path, default=TABLE_V100)
    e = sub.add_parser('engine-map', help="place the engine's 1.0.0 main addresses in another build (engine_map.py): "
                                          "engine/runtime/totk_main_v<NNN>.inc + v<NNN>.engine.json")
    e.add_argument('--old', required=True, type=Path, help='the 1.0.0 exefs/main (NSO)')
    e.add_argument('--new', required=True, type=Path, help="the other build's exefs/main (NSO)")
    e.add_argument('--version', required=True, help='its display version, e.g. 1.4.3')
    e.add_argument('--report', type=Path, help='every address and function with its placement (JSON)')
    args = p.parse_args(argv)

    try:
        if args.cmd in ('generate', 'check'):
            src = load_main_image(args.image or default_image())
        if args.cmd == 'generate':
            _write(TABLE_V100, generate_toml(src) + '\n')
            print(json.dumps(dict(status='pass', wrote=str(TABLE_V100))))
        elif args.cmd == 'check':
            want = generate_toml(src) + '\n'
            have = TABLE_V100.read_text(encoding='utf-8') if TABLE_V100.exists() else ''
            if want != have:
                print(json.dumps(dict(status='fail', problem=f'{TABLE_V100} is stale: run generate')))
                return 1
            print(json.dumps(dict(status='pass', table=str(TABLE_V100))))
        elif args.cmd == 'port':
            from .port import main_port
            return main_port(args)
        elif args.cmd == 'engine-map':
            from .engine_map import main_engine_map
            return main_engine_map(args)
        elif args.cmd == 'diff':
            table = read_table(args.table)
            img, ref = load_image_any(args.image), load_main_image(args.reference, table['game'])
            out = {}
            for f in table['feature']:
                rows = [r for r in table['patch'] if r['feature'] == f['name']]
                offs = sorted({r['offset'] for r in rows} | {r['copy_of'] for r in rows if 'copy_of' in r})
                bad = [hex(o) for o in offs if img[o:o + 4] != ref[o:o + 4]]
                if bad:
                    out[f['name']] = bad
            print(json.dumps(dict(status='pass' if not out else 'differ', features=out), indent=1))
            return 1 if out else 0
        elif args.cmd == 'list':
            t = read_table()
            for f in t['feature']:
                n = sum(1 for r in t['patch'] if r['feature'] == f['name'])
                print(f"{f['name']:<11} {'on ' if f['default'] else 'off'} {n:3} words"
                      + (f"  needs {', '.join(f['needs'])}" if f['needs'] else ''))
        elif args.cmd == 'apply':
            table = read_table(args.table)
            features = resolve_features(args.features, table)
            image = load_main_image(args.image, table['game'])
            report = apply(image, table, features)
            report.update(input=str(args.image), output=str(args.out))
            _write(args.out, bytes(image))
            _write(args.out.with_name(args.out.name + '.patches.json'), json.dumps(report, indent=2) + '\n')
            print(json.dumps(report))
        elif args.cmd in ('engine', 'sigma'):
            from . import engine
            table = read_table(args.table)
            features = resolve_features(engine.engine_spec(args.features), table)
            try:
                safe = engine.safe_mode(args.table, table)
            except engine.EngineError as e:
                raise PatchError(str(e))
            if 'cap30' in safe and 'fps60' in features:
                raise PatchError(f"{table['game']['version']} lost hooks that keep a frame rate above 30 right "
                                 '(its engine map: safe_mode cap30): export with --features default,-fps60')
            image = load_main_image(args.image, table['game'])
            resolve(table, image, features)   # (layout reads the copied words)
            funcs = json.loads(args.funcs.read_text(encoding='utf-8'))
            fde = {int(k, 16): v for k, v in funcs['fde'].items()}
            try:
                lay = engine.layout(table, features, fde)
            except engine.EngineError as e:
                raise PatchError(f'engine layout: {e}')
            report = apply(image, table, features)
            report.update(input=str(args.image), funcs=str(args.funcs), output=str(args.out),
                          engine=dict(fde_add={hex(k): v for k, v in lay['fde_add'].items()},
                                     ext={hex(k): [[hex(a), hex(b)] for a, b in v] for k, v in lay['ext'].items()},
                                     runtime_env=engine.runtime_env(features, safe)))
            _write(args.out / 'main.bin', bytes(image))
            _write(args.out / 'funcs_main.json', json.dumps(engine.funcs_json(funcs, lay), indent=1) + '\n')
            _write(args.out / 'patchset.json', json.dumps(report, indent=2) + '\n')
            _write(args.out / 'engine-patches.json',
                   json.dumps(engine.build_patch_set(table, features, lay, safe), indent=1) + '\n')   # build.py --patches
            print(json.dumps(dict(status='pass', features=features, image_sha256=report['image_sha256'],
                                  ext_functions=len(lay['ext']), fde_added=len(lay['fde_add']),
                                  runtime_env=engine.runtime_env(features, safe))))
    except PatchError as e:
        print(json.dumps(dict(status='refused', problem=str(e))))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
