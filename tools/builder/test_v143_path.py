"""Setup's 1.4.3 path, the parts v0.1.8's real check died on, from the code (no game data, no build; a second):

- the generated runtime tables agree everywhere: what engine/runtime/totk_version.h / .cpp include for 1.4.3 is what
  engine-map writes, what tools/engine/build.py's version_tables() carries into the snapshot and what
  builder.version_table_files() checks for (v0.1.8 step 5: 'totk_main_v143.inc file not found');
- builder.version_tables() runs port + engine-map again when one of them is gone although its marker says done (a
  clean-up between two Setups), and not when all are there;
- none of them is tracked (game-derived) and the tracked 1.4.3 inputs are (pins_v143.json, fields_v143.json,
  sites_v143.py, the 1.4.3 pin).

    python -I tools/builder/test_v143_path.py
"""
import json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [HERE, REPO]
import builder  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)
    if not ok:
        fails.append(name)


def src(rel):
    return open(os.path.join(REPO, rel), encoding='utf-8').read()


V = builder.UPDATE_DIR   # 'v143'
n = V[1:]
included = set(re.findall(rf'#include "(totk_\w+_{V}\w*\.inc)"', src('engine/runtime/totk_version.h') +
                          src('engine/runtime/totk_version.cpp')))
check(f'the runtime includes the {V} tables', {f'totk_main_{V}.inc', f'totk_fields_{V}.inc',
                                               f'totk_main_{V}_stubs.inc'} <= included, sorted(included))
pattern = lambda text: {m.replace('{n}', n) for m in re.findall(r"(totk_\w+_v\{n\}\w*\.inc)", text)}   # noqa: E731
mapped = pattern(src('games/totk/patches/engine_map.py'))
check('engine-map writes every table the runtime includes', included <= mapped, f'{sorted(included - mapped)}')
carried = pattern(src('tools/engine/build.py'))
check("build.py's version_tables() carries them into the snapshot", included <= carried, f'{sorted(included - carried)}')
tmp = tempfile.mkdtemp(prefix='v143path-')
pat, rt = os.path.join(tmp, 'games', 'totk', 'patches'), os.path.join(tmp, 'engine', 'runtime')
os.makedirs(pat)
os.makedirs(rt)
real_repo, builder.REPO = builder.REPO, tmp
eng = {'status': 'beta', 'beta': True}
open(os.path.join(pat, V + '.engine.json'), 'w').write(json.dumps(eng))
want = {os.path.basename(p) for p in builder.version_table_files()}
check("builder.version_table_files() is every generated file (a beta map: its stubs too)",
      want == included | {V + '.toml', V + '.engine.json'}, sorted(want))
eng['beta'] = False
open(os.path.join(pat, V + '.engine.json'), 'w').write(json.dumps(eng))
check('... no stubs for a map that is no beta (engine-map removes them)',
      f'totk_main_{V}_stubs.inc' not in {os.path.basename(p) for p in builder.version_table_files()})
eng['beta'] = True
open(os.path.join(pat, V + '.engine.json'), 'w').write(json.dumps(eng))

# version_tables(): with its marker current, it runs again only when a file is gone
calls = []
real_run, real_done, real_git = builder.run, builder.done_with, builder.git_id
builder.run = lambda label, cmd, env, dry, **kw: calls.append(cmd[3] if len(cmd) > 3 else cmd)
builder.done_with = lambda marker, want: True
builder.git_id = lambda path: 'x'
work, v100, vdir = (os.path.join(tmp, d) for d in ('work', 'v100', V))
for d in (v100, vdir):
    os.makedirs(os.path.join(d, 'exefs'))
    open(os.path.join(d, 'exefs', 'main'), 'wb').write(b'm')
for p in builder.version_table_files():
    open(p, 'a').close()
builder.version_tables(work, v100, vdir, 'py', {}, '3/9 tables', False)
check('all there, marker current: no engine-map', calls == [], calls)
os.remove(os.path.join(rt, f'totk_main_{V}.inc'))
builder.version_tables(work, v100, vdir, 'py', {}, '3/9 tables', False)
check(f'totk_main_{V}.inc gone, marker current: port and engine-map again', calls == ['port', 'engine-map'], calls)
builder.run, builder.done_with, builder.git_id, builder.REPO = real_run, real_done, real_git, real_repo

# engine-map on the EXPORTED tree (a player's Setup) sees what it sees here: the export renames engine/ code
# (DECOMP_GUEST_FN -> GUEST_FN: export_public CODE_RENAMES / PATH_RENAMES), and the addresses named only through the
# old spelling were left out of 1.4.3's map: step 5 failed to compile totk_ability.cpp (the v143 rehearsal on an
# export of ee19b105, 11-03)
sys.path.insert(0, os.path.join(REPO, 'tools', 'release'))
import export_public as ep  # noqa: E402
from games.totk.patches import engine_map as em  # noqa: E402
pub = tempfile.mkdtemp(prefix='v143pub-')
for p in em.source_files():
    rel = os.path.relpath(p, REPO).replace(os.sep, '/')
    text = p.read_text(encoding='utf-8', errors='replace')
    if rel.startswith('engine/'):
        for rx, rep in ep.CODE_RENAMES:
            text = re.sub(rx, rep, text)
        for old, new in ep.PATH_RENAMES:
            rel = rel.replace(old, new)
    os.makedirs(os.path.dirname(os.path.join(pub, rel)), exist_ok=True)
    open(os.path.join(pub, rel), 'w', encoding='utf-8').write(text)
here_c, pub_c = em.collect(), em.collect(type(em.ROOT)(pub))
for k in ('offsets', 'functions', 'fields'):
    lost = sorted(set(map(str, here_c[k])) - set(map(str, pub_c[k])))
    check(f"engine-map's {k} in the exported tree are the repository's", not lost,
          f'{len(lost)} lost: {[hex(int(x)) if x.isdigit() else x for x in lost[:6]]}')

# tracked / untracked
tracked = set(subprocess.run(['git', '-C', REPO, 'ls-files', 'games/totk/patches', 'engine/runtime', 'pipeline/expected'],
                             capture_output=True, text=True).stdout.split())
gen = {f'games/totk/patches/{V}.toml', f'games/totk/patches/{V}.engine.json'} | {f'engine/runtime/{x}' for x in included}
check('none of the generated files is tracked (game-derived)', not (gen & tracked), sorted(gen & tracked))
inputs = {f'games/totk/patches/pins_{V}.json', f'games/totk/patches/fields_{V}.json', f'games/totk/patches/sites_{V}.py',
          'pipeline/expected/totk-1.4.3.json'}
check('the tracked 1.4.3 inputs are tracked', inputs <= tracked, sorted(inputs - tracked))
ignored = subprocess.run(['git', '-C', REPO, 'check-ignore', *sorted(gen)], capture_output=True, text=True).stdout.split()
check('... and the generated ones ignored (never committed by mistake)', len(ignored) == len(gen), ignored)

print('all passed' if not fails else f'FAILED: {fails}')
sys.exit(1 if fails else 0)
