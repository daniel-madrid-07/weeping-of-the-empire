"""An --update install brings an older 1.0.0 game folder up to date too (builder.main_100: Setup with the 1.4.3 update built
only 1.4.3, and the player kept v0.1.9's 1.0.0), from the code (no game data, no build; a second):

- no 1.0.0 folder: nothing more is built, one line says how to get 1.0.0;
- a 1.0.0 folder from this Setup's commit: nothing more;
- one from another commit: main() again with the same inputs, without --update / --gui-pid (both forms), its lines
  '1.0.0: ...' with 'step N of M' (Setup's progress reads 'step N/M'), and no second 'DONE:' line;
- that pass failing (fail()'s SystemExit or an exception): the update build stays done, one line says how to update 1.0.0;
- main() calls main_100 only on an --update install run, before prune / tidy, and the 1.0.0 pass returns before them.

    python -I tools/builder/test_update_100.py
"""
import contextlib, io, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [HERE, REPO]
import builder  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)
    if not ok:
        fails.append(name)


def run_100(old, same, inner=None):
    """main_100 with newest_package / same_100_package / main replaced; (printed lines, the argv main got)"""
    got = []

    def fake_main(argv):
        got.append(list(argv))
        builder.say('step 5/9 build: build.py game')   # (what the 1.0.0 pass prints)
        if inner:
            inner()
    saved = builder.newest_package, builder.same_100_package, builder.main
    builder.newest_package = lambda out, tag=None: old if tag == '' else None
    builder.same_100_package = lambda out, head: same
    builder.main = fake_main
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            builder.main_100(ARGV, 'OUT', 'HEAD')
    finally:
        builder.newest_package, builder.same_100_package, builder.main = saved
    return buf.getvalue().splitlines(), got


ARGV = ['dump.nsp', 'prod.keys', '--update', 'upd.nsp', '--gui-pid', '42', '--install', 'C:\\I', '--update=x.nsp',
        '--gui-pid=7', '--cpu', 'v3']
U = builder.UPDATE_VERSION

lines, got = run_100(None, False)
check('no 1.0.0 folder: no 1.0.0 build', got == [], got)
check('no 1.0.0 folder: says how to get it', any('1.0.0: not built' in ln and U in ln for ln in lines), lines)

lines, got = run_100('OUT\\TotK-PC-1', True)
check("1.0.0 from this Setup's commit: nothing more", got == [] and lines == [], (got, lines))

lines, got = run_100('OUT\\TotK-PC-20261103-1904-f2c81da2a', False)
check('older 1.0.0: main() runs once more', len(got) == 1, got)
check('older 1.0.0: same inputs, no --update / --gui-pid', got and got[0] == ['dump.nsp', 'prod.keys', '--install', 'C:\\I',
                                                                          '--cpu', 'v3'], got)
check('older 1.0.0: says so first', lines and 'TotK-PC-20261103-1904-f2c81da2a' in lines[0] and 'updating it too' in lines[0],
      lines[:1])
inner = [ln for ln in lines if 'build.py game' in ln]
check("the 1.0.0 pass's lines: '1.0.0: step N of M' (no 'step N/M' for Setup's progress)",
      inner == ['[Builder] 1.0.0: step 5 of 9 build: build.py game'], inner)
check('no DONE line from main_100', not any('DONE:' in ln for ln in lines), lines)
check('the nesting flag is cleared after', builder.NESTED == [], builder.NESTED)

for what, inner_fn in (('fail()', lambda: builder.fail('no space')), ('an exception', lambda: 1 / 0)):
    lines, got = run_100('OUT\\TotK-PC-old', False, inner_fn)
    check(f'the 1.0.0 pass ends in {what}: caught, says how to update 1.0.0',
          any('1.0.0: not updated' in ln and U in ln for ln in lines), lines[-2:])
    check(f'after {what}: the flag is cleared', builder.NESTED == [], builder.NESTED)

src = open(os.path.join(HERE, 'builder.py'), encoding='utf-8').read()
body = src[src.index('\ndef main(argv=None):'):]
i_nested, i_call, i_prune = body.find('if NESTED:'), body.find('main_100(argv, out, head)'), body.find('prune(out, newest)\n        try:')
check('main(): the 1.0.0 pass returns before the --update run goes on', 0 < i_nested < i_call, (i_nested, i_call))
check('main(): main_100 runs only on an --update install, before prune / tidy',
      'if update and install:\n        main_100(argv, out, head)' in body and i_call < i_prune, (i_call, i_prune))
check("main(): the 1.0.0 pass prints no 'DONE:' of its own", "say(f'updated: {newest}')\n        return" in body)

print('ALL PASS' if not fails else f'{len(fails)} FAIL: {fails}')
sys.exit(1 if fails else 0)
