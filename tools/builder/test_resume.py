"""Setup run again on the same folder after a failure reuses every step already done (builder-steps.json) and goes on
from the one that failed, saying 'reusing step N (done at ...)'; a changed dump or a missing output redoes the step.
A fake tree and fake steps (builder.main with run() and the PC checks replaced); no game, no build, seconds.

    python -I tools/builder/test_resume.py
"""
import json, os, re, sys, tempfile, types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import builder  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)
    if not ok:
        fails.append(name)


tmp = tempfile.mkdtemp(prefix='resume-')
work, out = os.path.join(tmp, 'work'), os.path.join(tmp, 'game')
dump, keys = os.path.join(tmp, 'game.nsp'), os.path.join(tmp, 'prod.keys')
for p in (dump, keys):
    open(p, 'w').write('x')
v100 = os.path.join(work, 'extracted', 'v100')
kdir = os.path.join(work, 'engine', 'game', 'K1')
bank = os.path.join(work, 'engine', 'glyphs', 'engine+glyphs')
head = builder.subprocess.run(['git', '-C', builder.REPO, 'rev-parse', 'HEAD'], capture_output=True,
                              text=True).stdout.strip()
state = {'fail_at': None, 'ran': [], 'lines': [], 'pkg': 0, 'prepared': 0, 'gpus': [['GPU A', '32.0.15.1']]}


def key_json():
    return json.dumps({'key': 'K1', 'dir': kdir, 'img': 'IMG', 'built': os.path.exists(os.path.join(kdir, 'manifest.json'))})


def fake_run(step, cmd, env, dry, beat=None, quiet=False, watch=None):
    num = step.split('/')[0]
    what = 'key' if 'key' in cmd else 'game' if 'game' in cmd else step.split(' ', 1)[1]
    state['ran'].append(f'{num} {what}')
    if state['fail_at'] == num:
        builder.fail(f'step {step} failed (the test)')
    if num == '1':
        os.makedirs(v100, exist_ok=True)
        open(os.path.join(v100, 'READY.json'), 'w').write('{}')
    elif num == '2':
        open(os.path.join(v100, 'romfs-source.txt.verified.json'), 'w').write('{}')
    elif num == '5' and what == 'game':
        os.makedirs(os.path.join(kdir, 'exe'), exist_ok=True)
        json.dump({'objs_done': True}, open(os.path.join(kdir, 'manifest.json'), 'w'))
        open(os.path.join(kdir, 'exe', 'totk.exe'), 'w').write('exe')
    elif num == '6':
        os.makedirs(os.path.join(work, 'overlays'), exist_ok=True)
    elif num == '7':
        os.makedirs(bank, exist_ok=True)
        os.utime(bank)
    elif num == '8':
        state['pkg'] += 1
        d = os.path.join(out, f'TotK-PC-2026110{state["pkg"]}-0000-{head[:9]}')
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, 'player.txt'), 'w').write(f'key=K1\ncommit={head}\n')
        open(os.path.join(d, 'totk.exe'), 'w').write('exe')
        os.utime(d, (1e9 + state['pkg'], 1e9 + state['pkg']))
    return key_json() if what == 'key' else ''


os.environ['LOCALAPPDATA'] = os.path.join(tmp, 'local')   # (the player's profile, where step 9's cache lives)
cold = os.path.join(tmp, 'local', 'nxrecomp', 'TotK-engine', 'run', 'cache', 'precompile_cold.key')


def fake_prepare(pkg, n, dry, cmd=None):
    state['ran'].append('9 shaders')
    state['prepared'] += 1
    os.makedirs(os.path.dirname(cold), exist_ok=True)
    open(cold, 'w').write('vendor device driver uuid exe')   # (the runtime writes it at the end of a prepare)
    return 0


for name, fn in dict(run=fake_run, say=lambda s: state['lines'].append(s), check_keys=lambda k: None,
                     check_tools=lambda managed=False: ([], [], {}), check_pc=lambda: [],
                     memory_gb=lambda: (32.0, 16.0), free_gb=lambda p: 500.0, filesystem=lambda p: 'NTFS',
                     dump_drive=lambda p: 'fixed', cjk_windows=lambda: False, check_extraction=lambda *a, **k: None,
                     engine_patches=lambda *a, **k: [], app_icon=lambda work, vdir, py, env, label, dry: env,
                     pgo_args=lambda *a, **k: [], hot_args=lambda *a, **k: [], shader_seed=lambda *a, **k: None,
                     release_of=lambda repo: None, prune=lambda *a, **k: None, prepare_shaders=fake_prepare,
                     check_dump=lambda *a, **k: '',   # (the fake dump is no NSP: test_dump_check.py covers it)
                     gpus=lambda: state['gpus']).items():
    setattr(builder, name, fn)
builder.PREPARE_SHADERS = True
sys.modules['tidy'] = types.SimpleNamespace(tidy=lambda *a, **k: None)


def go(fail_at=None):
    state.update(fail_at=fail_at, ran=[], lines=[])
    try:
        builder.main([dump, keys, '--work', work, '--out', out])
        return 0
    except SystemExit as e:
        return e.code or 0


def reused(n):
    return any(re.match(rf'step {n}/9 \w+: reusing step {n} \(done at \d{{4}}-\d\d-\d\d \d\d:\d\d\)$', ln)
               for ln in state['lines'])


# 1. the first run fails at step 7 (the overlay)
rc = go(fail_at='7')
check('1st run: fails at step 7', rc != 0 and state['ran'][-1] == '7 overlay', str(state['ran']))
# 2. again: 1-6 reused (the build is not run), 7 onward runs
go()
check('2nd run: steps 1, 2, 5, 6 reused', all(reused(n) for n in (1, 2, 5, 6)), str(state['lines'][-12:]))
check('2nd run: no build.py game', '5 game' not in state['ran'], str(state['ran']))
check('2nd run: 7, 8, 9 run', all(s in state['ran'] for s in ('7 overlay', '8 package', '9 shaders')), str(state['ran']))
check('the Setup window reads the line', all(re.search(r'step (\d+)/(\d+) (\w+)', ln) for ln in state['lines']
                                             if 'reusing step' in ln))
# 3. once more: everything reused, nothing run but the key
go()
check('3rd run: 7, 8, 9 reused', all(reused(n) for n in (7, 8, 9)), str(state['lines'][-8:]))
check('3rd run: nothing run', not any(s in state['ran'] for s in ('5 game', '7 overlay', '8 package', '9 shaders')),
      str(state['ran']))
# 4. another dump in the same path (size / time differ): the stamped steps run again
open(dump, 'w').write('another dump')
go()
check('changed dump: build, overlay, package, shaders again',
      all(s in state['ran'] for s in ('5 game', '7 overlay', '8 package', '9 shaders')), str(state['ran']))
# 5. an output gone (a clean-up): only that step and what follows it
builder.shutil.rmtree(bank)
go()
check('overlay output gone: overlay again, the build reused', '7 overlay' in state['ran'] and '5 game' not in
      state['ran'] and reused(5), str(state['ran']))
# 6. step 9's cache is in the player's profile: cleared (an uninstall, the player), it is prepared again
os.remove(cold)
go()
check('cache cleared: shaders again, the rest reused', state['ran'][-1:] == ['9 shaders'] and reused(8),
      str(state['ran']))
# 7. a new GPU driver makes the prepared cache stale: shaders again
state['gpus'] = [['GPU A', '32.0.15.2']]
go()
check('new driver: shaders again', state['ran'][-1:] == ['9 shaders'] and reused(8), str(state['ran']))
go()
check('then reused', reused(9) and '9 shaders' not in state['ran'], str(state['ran']))
# 8. the stamps file is broken: nothing is reused from it, nothing fails
open(os.path.join(work, 'builder-steps.json'), 'w').write('{not json')
rc = go()
check('broken stamps: runs, no crash', rc == 0 and '7 overlay' in state['ran'], str(state['ran']))

print('all passed' if not fails else f'FAILED: {fails}')
sys.exit(1 if fails else 0)
