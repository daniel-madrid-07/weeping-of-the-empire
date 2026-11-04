"""The hash-pinned Python wheels (toolchain.WHEEL_LOCKS, lock_wheels.py): the lock files match the package lists and
pin every line; which Pythons they cover; what pip_install / kit_wheels hand pip; lock_wheels' wheel-tag test.
Offline, seconds. --online: also a real pip install of each lock into a temporary venv of this Python (when the lock
covers it), and the refusal of a wheel whose pinned hash is changed.
    py tools/builder/test_wheel_lock.py [--online]"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import toolchain  # noqa: E402
import lock_wheels  # noqa: E402

fails = 0


def check(what, ok, got=''):
    global fails
    print(('PASS ' if ok else 'FAIL ') + what + ('' if ok else f': {got}'))
    fails += not ok


def entries(path):
    """{name: (version, marker, [hashes])} of a lock file"""
    text = open(path, encoding='utf-8').read().replace('\\\n', ' ')
    out = {}
    for line in text.splitlines():
        if line.startswith('#') or not line.strip():
            continue
        m = re.match(r'([a-z0-9-]+)==([^ ;]+)( ; [^-]*)?', line)
        out[m[1]] = (m[2], (m[3] or '').strip(), re.findall(r'--hash=sha256:([0-9a-f]{64})', line))
    return out


def main():
    lists = {'setup': toolchain.PACKAGES, 'venv': lock_wheels.venv_packages()}
    for which, specs in lists.items():
        p = os.path.join(HERE, toolchain.WHEEL_LOCKS[which])
        e = entries(p)
        check(f'{which}: every listed package pinned at its version',
              all(e.get(lock_wheels.norm(s.split('==')[0]), ('',))[0] == s.split('==')[1] for s in specs),
              [s for s in specs if e.get(lock_wheels.norm(s.split('==')[0]), ('',))[0] != s.split('==')[1]])
        check(f'{which}: every line has hashes', all(h for _, _, h in e.values()), [n for n, x in e.items() if not x[2]])
        check(f'{which}: covers Setup\'s Python on Windows and Linux',
              '# targets: cp313-win_amd64 cp313-linux_x86_64' in open(p, encoding='utf-8').read())
    check('cryptography brings its dependencies (cffi, pycparser)', {'cffi', 'pycparser'} <= set(entries(
        os.path.join(HERE, toolchain.WHEEL_LOCKS['setup']))))

    # which wheels lock_wheels counts for a target
    F = lock_wheels.fits
    check('tags: cp313 win_amd64', F('numpy-2.4.4-cp313-cp313-win_amd64.whl', 'win_amd64'))
    check('tags: not cp312, not cp313t, not win32', not F('numpy-2.4.4-cp312-cp312-win_amd64.whl', 'win_amd64') and
          not F('numpy-2.4.4-cp313-cp313t-win_amd64.whl', 'win_amd64') and not F('x-1-cp313-cp313-win32.whl', 'win_amd64'))
    check('tags: abi3 from an older cp3x, py3-none-any', F('cryptography-50.0.1-cp311-abi3-win_amd64.whl', 'win_amd64')
          and F('fonttools-4.62.1-py3-none-any.whl', 'linux_x86_64'))
    check('tags: manylinux x86_64 on linux only', F('lz4-4.4.5-cp313-cp313-manylinux_2_17_x86_64.manylinux2014_x86_64.whl',
                                                    'linux_x86_64') and
          not F('lz4-4.4.5-cp313-cp313-manylinux_2_17_aarch64.whl', 'linux_x86_64') and
          not F('lz4-4.4.5-cp313-cp313-manylinux_2_17_x86_64.whl', 'win_amd64') and
          not F('lz4-4.4.5-cp313-cp313-musllinux_1_2_x86_64.whl', 'linux_x86_64'))

    # coverage: this Python (the test's) and a made-up target
    tgt = toolchain.python_target()
    check('python_target names this Python', tgt and tgt.startswith(f'cp{sys.version_info[0]}{sys.version_info[1]}-'), tgt)
    saved = toolchain.python_target
    try:
        toolchain.python_target = lambda py=None: 'cp312-win_amd64'
        check('a Python the lock does not cover: no lock (unpinned specs)', toolchain.wheel_lock('setup') is None)
        toolchain.python_target = lambda py=None: 'cp313-win_amd64'
        check('Setup\'s Python: the lock file', toolchain.wheel_lock('setup', 'x') == os.path.join(HERE, 'wheels-setup.txt'))

        # what pip_install / kit_wheels hand pip (subprocess.run recorded; nothing installed)
        calls, real_run, real_wheels = [], subprocess.run, toolchain.kit.wheels

        class R:
            returncode, stdout, stderr = 0, '', ''
        toolchain.subprocess.run = lambda cmd, **kw: calls.append(cmd) or R()
        toolchain.kit.wheels = lambda say: None
        try:
            toolchain.pip_install(['py'], ['zstandard==0.25.0'], lock='setup')
            check('pip_install: the whole lock, --require-hashes', calls and calls[-1][-3:] ==
                  ['--require-hashes', '-r', os.path.join(HERE, 'wheels-setup.txt')], calls[-1:])
            toolchain.kit.wheels = lambda say: r'C:\kit\wheels'
            calls.clear()
            toolchain.pip_install(['py'], ['zstandard==0.25.0'], lock='venv')
            check('pip_install from a kit: --no-index --find-links + the lock', calls and
                  calls[0][-6:] == ['--no-index', '--find-links', r'C:\kit\wheels', '--require-hashes', '-r',
                                    os.path.join(HERE, 'wheels-venv.txt')], calls[:1])
            calls.clear()
            said, real_say = [], toolchain.say
            toolchain.say = said.append
            toolchain.kit.wheels = lambda say: None
            toolchain.pip_install(['py'], ['zstandard==0.26.0'], lock='setup')
            toolchain.say = real_say
            check('a list ahead of its lock: the specs unpinned, said loudly (lock_wheels.py)', calls and
                  calls[-1][-1] == 'zstandard==0.26.0' and '--require-hashes' not in calls[-1] and
                  any('WITHOUT their hash check' in s and 'lock_wheels.py' in s for s in said), (calls[-1:], said))
            # an older kit (other wheels than the lock): the kit try and the index both fail -> the error says so
            R.returncode, R.stderr = 1, 'ERROR: Could not find a version that satisfies the requirement'
            toolchain.kit.wheels = lambda say: r'C:\kit\wheels'
            toolchain.say = lambda s: None
            try:
                toolchain.pip_install(['py'], ['zstandard==0.25.0'], lock='setup')
                check('older kit, offline: fails', False)
            except toolchain.ToolchainError as e:
                check('older kit, offline: the error says the kit is older than this Setup',
                      'older than this Setup' in str(e), str(e)[-300:])
            toolchain.say = real_say
            R.returncode, R.stderr = 0, ''
            toolchain.python_target = lambda py=None: None
            calls.clear()
            toolchain.kit.wheels = lambda say: None
            toolchain.pip_install(['py'], ['zstandard==0.25.0'], lock='setup')
            check('pip_install, uncovered Python: the specs as before', calls and calls[-1][-1] == 'zstandard==0.25.0'
                  and '--require-hashes' not in calls[-1], calls[-1:])
            R.returncode, R.stderr = 1, 'ERROR: THESE PACKAGES DO NOT MATCH THE HASHES FROM THE REQUIREMENTS FILE.'
            toolchain.python_target = lambda py=None: 'cp313-win_amd64'
            try:
                toolchain.pip_install(['py'], [], lock='setup')
                check('a hash mismatch fails', False)
            except toolchain.ToolchainError as e:
                check('a hash mismatch fails, saying the package is not the pinned one', 'not the pinned one' in str(e), str(e)[:200])
        finally:
            toolchain.subprocess.run, toolchain.kit.wheels = real_run, real_wheels
    finally:
        toolchain.python_target = saved

    # export_public refuses a tree whose package lists are ahead of their locks (offline, from the file texts)
    sys.path.insert(0, os.path.join(lock_wheels.REPO, 'tools', 'release'))
    import export_public
    files = {p: open(os.path.join(lock_wheels.REPO, p), encoding='utf-8').read() for p in (
        'tools/builder/toolchain.py', 'tools/engine/build.py', 'tools/builder/wheels-setup.txt', 'tools/builder/wheels-venv.txt')}
    text_of = lambda p: files.get(p, '')
    check('export check: this tree\'s lists and locks agree', export_public.wheel_pins_current(text_of) == [],
          export_public.wheel_pins_current(text_of))
    files['tools/engine/build.py'] = files['tools/engine/build.py'].replace("'ninja==1.13.2'", "'ninja==1.13.3'")
    bad = export_public.wheel_pins_current(text_of)
    check('export check: a bumped VENV_PACKAGES pin without lock_wheels.py is refused', len(bad) == 1 and
          'ninja==1.13.3' in bad[0] and 'lock_wheels.py' in bad[0], bad)
    files['tools/builder/toolchain.py'] = files['tools/builder/toolchain.py'].replace('\nPACKAGES = [', '\nPACKAGES: list = [')
    bad = export_public.wheel_pins_current(text_of)
    check('export check: a list it cannot read is refused, not skipped', any("no 'PACKAGES = [...]'" in p for p in bad), bad)

    if '--online' in sys.argv:
        online()
    print('wheel lock: all passed' if not fails else f'wheel lock: {fails} FAIL')
    return 1 if fails else 0


def online():
    """a real install of each lock into a temporary venv of this Python, and a changed hash refused"""
    if not toolchain.wheel_lock('venv'):
        check(f'online: this Python ({toolchain.python_target()}) is not a lock target', False)
        return
    tmp = tempfile.mkdtemp(prefix='wheel_lock_')
    try:
        subprocess.run([sys.executable, '-m', 'venv', os.path.join(tmp, 'v')], check=True)
        py = os.path.join(tmp, 'v', 'Scripts' if os.name == 'nt' else 'bin', 'python' + ('.exe' if os.name == 'nt' else ''))
        for which in ('venv', 'setup'):
            try:
                toolchain.pip_install([py], [], quiet=True, lock=which)
                check(f'online: {which} lock installs with --require-hashes', True)
            except toolchain.ToolchainError as e:
                check(f'online: {which} lock installs with --require-hashes', False, str(e)[-400:])
        bad = os.path.join(tmp, 'bad.txt')
        text = open(os.path.join(HERE, 'wheels-venv.txt'), encoding='utf-8').read()
        open(bad, 'w', encoding='utf-8').write(re.sub(r'--hash=sha256:[0-9a-f]{64}', '--hash=sha256:' + '0' * 64, text))
        subprocess.run([py, '-m', 'pip', 'uninstall', '-y', '-q', 'capstone'], capture_output=True)
        r = subprocess.run([py, '-m', 'pip', 'install', '--disable-pip-version-check', '--only-binary', ':all:',
                            '--require-hashes', '-r', bad], capture_output=True, text=True)
        check('online: a wheel whose pinned hash is not its own is refused', r.returncode != 0 and
              'DO NOT MATCH THE HASHES' in r.stdout + r.stderr, (r.stdout + r.stderr)[-300:])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
