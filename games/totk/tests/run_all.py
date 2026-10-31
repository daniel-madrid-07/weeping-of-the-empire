"""End-of-phase check of games/totk (the only test run): the table, `apply` on copies of the verified 1.0.0 main (from the
flat image and from the NSO, through recomp/nso.py), its refusals, the Unicorn verifiers on the patched image, the C
hooks' unit test, and the RomFS overlays when a RomFS is extracted. Everything is written to a fresh temp directory.

    python -m games.totk.tests.run_all [--work C:\\nxrecomp-work] [--romfs <dir>] [--c-test <exe>]

Prints one JSON report; exit 1 when anything failed."""
import argparse
import contextlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from games.totk import patches as P
from games.totk.patches import __main__ as cli

from . import verify_cloth, verify_fps60, verify_realtime, verify_sites


def _apply_cli(args):
    out = subprocess.run([sys.executable, '-m', 'games.totk.patches', *args], capture_output=True, text=True)
    return out.returncode, json.loads(out.stdout.strip().splitlines()[-1]) if out.stdout.strip() else {}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--work', type=Path, default=Path(r'C:\nxrecomp-work'))
    p.add_argument('--romfs', type=Path, help='default: <work>/extracted/v100 when it holds romfs-index.json')
    p.add_argument('--c-test', type=Path, help='a built test_totk_patches.exe to run')
    args = p.parse_args(argv)
    tmp = Path(tempfile.mkdtemp(prefix='totk-patches-'))
    report, failed = dict(tmp=str(tmp)), []

    def step(name, fn):
        t0 = time.monotonic()
        try:
            r = fn()
            status = r.get('status', 'pass') if isinstance(r, dict) else 'pass'
        except Exception as e:   # a crash in one step must not hide the others
            r, status = dict(error=f'{type(e).__name__}: {e}'), 'fail'
        if status == 'fail':
            failed.append(name)
        report[name] = {**(r if isinstance(r, dict) else dict(result=r)), 'status': status,
                        'seconds': round(time.monotonic() - t0, 1)}

    def table():
        with contextlib.redirect_stdout(io.StringIO()):   # one JSON report on stdout
            return dict(status='pass' if cli.main(['check', '--image', str(image_in)]) == 0 else 'fail')
    image_in = args.work / 'images' / 'main.img'
    step('table', table)
    nso_in = args.work / 'extracted' / 'v100' / 'exefs' / 'main'
    out_img, out_nso = tmp / 'main.patched.img', tmp / 'main.from-nso.img'

    def apply_all():
        rc1, r1 = _apply_cli(['apply', '--image', str(image_in), '--out', str(out_img), '--features', 'all'])
        rc2, r2 = _apply_cli(['apply', '--image', str(nso_in), '--out', str(out_nso), '--features', 'all'])
        same = out_img.read_bytes() == out_nso.read_bytes()
        table = P.read_table(image=P.load_main_image(image_in))   # (the copies' words come from the image)
        per_patch = []
        for row in table['patch']:
            got = out_img.read_bytes()[row['offset']:row['offset'] + 4]
            per_patch.append(dict(feature=row['feature'], offset=hex(row['offset']),
                                  status='applied' if got == row['new'] else 'refused', new_asm=row['new_asm']))
        bad = [x for x in per_patch if x['status'] != 'applied']
        return dict(status='pass' if rc1 == rc2 == 0 and same and not bad else 'fail', from_image=r1, nso_matches_image=same,
                    per_feature={f: sum(1 for x in per_patch if x['feature'] == f and x['status'] == 'applied') for f in r1.get('features', [])},
                    not_applied=bad)
    step('apply', apply_all)

    def refusals():
        cases = {}
        bad_img = tmp / 'main.bad.img'
        data = bytearray(image_in.read_bytes())
        data[0x8f6c60:0x8f6c64] = b'\0\0\0\0'
        bad_img.write_bytes(data)
        rc, r = _apply_cli(['apply', '--image', str(bad_img), '--out', str(tmp / 'x.img')])
        cases['changed image refused'] = rc == 1 and r.get('status') == 'refused' and not (tmp / 'x.img').exists()
        table = P.read_table()   # a feature's words that differ (a wrong digest), with the right image: refused, untouched
        table['feature'][0]['original_sha256'] = '0' * 64
        img = P.load_main_image(image_in)
        before = bytes(img)
        try:
            P.apply(img, table, ['fps60'])
            cases['original mismatch refused'] = False
        except P.PatchError:
            cases['original mismatch refused'] = bytes(img) == before
        rc, r = _apply_cli(['apply', '--image', str(image_in), '--out', str(tmp / 'y.img'), '--features', 'cloth'])
        cases['cloth without realtime refused'] = rc == 1 and 'needs realtime' in r.get('problem', '')
        rc, r = _apply_cli(['apply', '--image', str(image_in), '--out', str(tmp / 'z.img'), '--features', 'warp9'])
        cases['unknown feature refused'] = rc == 1
        for f in [f['name'] for f in P.read_table()['feature']]:   # every feature alone (with its needs)
            spec = 'realtime,cloth' if f == 'cloth' else f
            img = P.load_main_image(image_in)
            cases[f'{f} alone applies'] = P.apply(img, P.read_table(), P.resolve_features(spec, P.read_table()))['status'] == 'pass'
        return dict(status='pass' if all(cases.values()) else 'fail', cases=cases)
    step('refusals', refusals)

    def verifiers():
        original, patched = image_in.read_bytes(), out_img.read_bytes()
        results = [verify_fps60.verify(original, patched), verify_realtime.verify(original, patched),
                   verify_cloth.verify(original, patched), *verify_sites.verify(original, patched)]
        return dict(status='fail' if any(r['status'] != 'pass' for r in results) else 'pass', results=results)
    step('verifiers', verifiers)

    def port():
        r = subprocess.run([sys.executable, '-m', 'unittest', 'games.totk.tests.test_port'], capture_output=True, text=True)
        return dict(status='pass' if r.returncode == 0 else 'fail', output=r.stderr.strip().splitlines()[-1:])
    step('port', port)

    def engine_map():
        r = subprocess.run([sys.executable, '-m', 'unittest', 'games.totk.tests.test_engine_map'], capture_output=True,
                           text=True)
        return dict(status='pass' if r.returncode == 0 else 'fail', output=r.stderr.strip().splitlines()[-1:])
    step('engine_map', engine_map)

    def gen_ext_guard():   # gen_ext.py (every patched main) copies gen.py's Emitter methods: drift fails here, not in a key
        recomp = Path(__file__).resolve().parents[3] / 'engine' / 'tools' / 'recomp'
        r = subprocess.run([sys.executable, '-c', 'import gen_ext'], cwd=recomp, capture_output=True, text=True)
        return dict(status='pass' if r.returncode == 0 else 'fail', output=(r.stdout + r.stderr).strip()[-300:])
    step('gen_ext_guard', gen_ext_guard)

    if args.c_test:
        def c_test():
            r = subprocess.run([str(args.c_test)], capture_output=True, text=True)
            return dict(status='pass' if r.returncode == 0 else 'fail', output=r.stdout.strip().splitlines()[-1:],
                        failures=[l for l in r.stdout.splitlines() if l.startswith('FAIL')])
        step('c_hooks', c_test)

    romfs = args.romfs or args.work / 'extracted' / 'v100'
    if (romfs / 'romfs-index.json').exists() or (romfs / 'index.json').exists() or (romfs / 'romfs').is_dir():
        def overlays():
            r = subprocess.run([sys.executable, '-m', 'games.totk.overlays', '--romfs', str(romfs), '--out', str(tmp / 'overlays')],
                               capture_output=True, text=True)
            lines = [json.loads(l) for l in r.stdout.splitlines() if l.startswith('{')]
            return dict(status='pass' if r.returncode == 0 else 'fail', sets=lines, stderr=r.stderr[-2000:])
        step('overlays', overlays)
    else:
        report['overlays'] = dict(status='pending', reason=f'no extracted RomFS at {romfs}')

    report['status'] = 'fail' if failed else 'pass'
    report['failed'] = failed
    print(json.dumps(report, indent=1))
    if not failed:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
