"""Tests of the Linux port's pure parts (linux-port, 2026-11-02); they run on Windows and Linux, no game, no compiler.

    python tools/linux/test_linux.py
  pgo_remap: MSVC -> Itanium names, records dropped / kept whole
  launcher_linux: the game's environment from player.txt and the XDG folders, the version's own saves, the cleared shell,
    the crash report out of the log
  host_linux: the file system and drive kind from a mounts table, the CPU level and memory from /proc text
  builder: the hot list reads the Linux profile's names
"""
import os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [HERE, os.path.join(REPO, 'engine', 'launcher', 'linux'), os.path.join(REPO, 'tools', 'builder')]
import pgo_remap, launcher_linux, host_linux  # noqa: E402

fails = 0


def check(what, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(('PASS ' if ok else 'FAIL ') + what + ('' if ok else f': got {got!r}, want {want!r}'))


# ---- pgo_remap
check('itanium f_', pgo_remap.itanium('f_1b4d760'), '_Z9f_1b4d760P3Ctx')
check('itanium chunk', pgo_remap.itanium('f_5346a0_c3'), '_Z11f_5346a0_c3P3Ctx')
text = [':ir\n',
        '?f_1b4d760@@YAXPEIAUCtx@@@Z\n', '# Func Hash:\n', '123\n', '# Num Counters:\n', '1\n', '# Counter Values:\n', '7\n', '\n',
        'exec.c;draw_\n', '# Func Hash:\n', '9\n', '# Num Counters:\n', '1\n', '# Counter Values:\n', '5\n', '\n',
        '?s_imp_12@@YAXPEAUCtx@@@Z\n', '# Func Hash:\n', '44\n', '# Num Counters:\n', '1\n', '# Counter Values:\n', '0\n', '\n']
out, renamed, dropped = pgo_remap.remap_text(text)
check('remap counts', (renamed, dropped), (2, 1))
check('remap names', [l for l in out if l.startswith(('_Z', '?', 'exec'))], ['_Z9f_1b4d760P3Ctx\n', '_Z8s_imp_12P3Ctx\n'])
check('remap keeps the header and the kept records whole', out[:9], [':ir\n', '_Z9f_1b4d760P3Ctx\n'] + text[2:9])
check('remap drops the runtime record whole', any('9\n' == l for l in out[9:17]), False)
# a profile with no generated-function record: exit 1 and no file (builder.py would keep an empty one for good)
with tempfile.TemporaryDirectory() as t:
    def fake_profdata(cmd, check=True):
        if '--text' in cmd:   # (merge --text <src> -o <txt>: only a runtime record)
            open(cmd[cmd.index('-o') + 1], 'w').writelines(text[:1] + text[9:17])
        else:
            open(cmd[cmd.index('-o') + 1], 'w').close()
    run0, argv0 = pgo_remap.subprocess.run, sys.argv
    pgo_remap.subprocess.run, sys.argv = fake_profdata, ['pgo_remap', os.path.join(t, 'in.profdata'), os.path.join(t, 'out.profdata')]
    try:
        check('remap with nothing renamed: exit 1', pgo_remap.main(), 1)
    finally:
        pgo_remap.subprocess.run, sys.argv = run0, argv0
    check('remap with nothing renamed: no file', os.listdir(t), [])

# ---- launcher_linux
with tempfile.TemporaryDirectory() as t:
    pkg, home = os.path.join(t, 'game', 'TotK-PC-x'), os.path.join(t, 'home')
    os.makedirs(os.path.join(pkg, 'cache-seed')); os.makedirs(home)
    open(os.path.join(pkg, 'cache-seed', 'precompile.bin'), 'w').close()
    open(os.path.join(pkg, 'settings.ini'), 'w').write('display.fps_cap = 120\n')
    with open(os.path.join(pkg, 'player.txt'), 'w', newline='\n') as f:
        f.write('img=/w/engine/img/abc\nromfs=/w/extracted/v100/romfs\noverlay=\nsettings=settings.ini\n'
                'env.TOTK_MOUSE_CAMERA=1\nenv.TOTK_PRECOMPILE_SEED=\nimg=/second/ignored\n')
    env0 = {'HOME': home, 'PATH': '/usr/bin', 'TOTK_FPS': '999', 'NXR_X': '1', 'LARECOMP_Y': '1'}
    run, logs, env = launcher_linux.game_env(pkg, env0)
    data = os.path.join(home, '.local', 'share', 'nxrecomp', 'TotK-engine')
    check('XDG data folder', env['TOTK_USERDATA'], os.path.join(data, 'userdata'))
    check('run folder in the cache', run, os.path.join(home, '.cache', 'nxrecomp', 'TotK-engine', 'run'))
    check('player.txt first value wins', env['TOTK_IMG'], '/w/engine/img/abc')
    check('empty overlay not set', 'TOTK_OVERLAY' in env, False)
    check('env lines', env['TOTK_MOUSE_CAMERA'], '1')
    check('player.txt seed opt-out kept (empty)', env['TOTK_PRECOMPILE_SEED'], '')
    check('shell knobs cleared', [k for k in ('TOTK_FPS', 'NXR_X', 'LARECOMP_Y') if k in env], [])
    # (':' between them: the runtime's Linux list reader, mods.cpp env_list; compared whole, a Windows test path has a
    #  drive letter's ':' too)
    check('mods: game folder then player, joined by ":"', env['TOTK_MODS_DIR'],
          ':'.join([os.path.join(pkg, 'mods'), os.path.join(data, 'mods')]))
    check('settings copied on first start', open(os.path.join(data, 'settings.ini')).read(), 'display.fps_cap = 120\n')
    # another version: its own saves, a copy of 1.0.0's once
    os.makedirs(os.path.join(data, 'userdata', 'save', '0'))
    open(os.path.join(data, 'userdata', 'save', '0', 'progress.sav'), 'w').write('x')
    with open(os.path.join(pkg, 'player.txt'), 'a') as f:
        f.write('version=1.4.3\n')
    _, _, env = launcher_linux.game_env(pkg, env0)
    check('version saves folder', env['TOTK_USERDATA'], os.path.join(data, 'userdata-v143'))
    check('1.0.0 saves copied', os.path.isfile(os.path.join(data, 'userdata-v143', 'save', '0', 'progress.sav')), True)
check('exit codes keep their low byte', (launcher_linux.RESTART_CODE, launcher_linux.GPU_UNSUPPORTED_CODE), (0x52, 0x30))
# the crash report out of the game's log into crashes/crash-<time>-<pid>.txt (PARITY_037 #30)
with tempfile.TemporaryDirectory() as t:
    logp, crashes = os.path.join(t, 'totk-1.log'), os.path.join(t, 'crashes')
    report = ['[crash] signal 11 (Segmentation fault) code 1 at host 4a1b2c (guest fn f_710012) addr 0  stage frame #9\n',
              '[crash] exe+4a1b2c (load 400000)  in HLE: (none: generated code)\n', 'r8 =0000000000000000\n']
    with open(logp, 'w', encoding='utf-8') as f:
        f.writelines([f'[vk] line {i}\n' for i in range(100)] + report)
    rep = launcher_linux.crash_report(logp, crashes, 3, 4242, {}, 'abc123')
    text = open(rep, encoding='utf-8').read() if rep else ''
    check('crash report: named as on Windows', bool(rep) and os.path.basename(rep).startswith('crash-') and
          rep.endswith('-4242.txt'), True)
    check('crash report: the report whole, the 40 lines before it, the commit', (text.endswith(''.join(report)),
          '[vk] line 60\n' in text, '[vk] line 59\n' in text, 'commit: abc123' in text), (True, True, False, True))
    check('crash report: TOTK_PC_CRASH=0 none', launcher_linux.crash_report(logp, crashes, 3, 1, {'TOTK_PC_CRASH': '0'}), None)
    with open(logp, 'w') as f:
        f.write('[vk] closed with an error\n')
    check('crash report: no report in the log, no file', launcher_linux.crash_report(logp, crashes, 1, 2, {}), None)
    for i in range(12):
        open(os.path.join(crashes, f'crash-20260101-0000{i:02d}-1.txt'), 'w').close()
    with open(logp, 'w') as f:
        f.writelines(report)
    rep = launcher_linux.crash_report(logp, crashes, 3, 7, {})
    left = sorted(os.listdir(crashes))
    check('crash report: the newest 10 kept, this one among them', (len(left), os.path.basename(rep) in left), (10, True))
    # exit 48 (DMP0 / GPU0's low byte): the game's own [fatal] reason, for the notice (linux-test D#83's no-data fatal)
    with open(logp, 'w') as f:
        f.write('[fs] mount content: -> /x/romfs\n[fatal] the game data cannot be found: no game dump or RomFS image is set '
                '(TOTK_ROMFS_PACK), and no RomFS folder at /x/romfs; run Setup again\n')
    check('exit 48: the [fatal] reason read back', launcher_linux.last_fatal(logp).startswith('the game data cannot be found'), True)
    with open(logp, 'w') as f:
        f.write('[vk] fine\n')
    check('exit 48 with no [fatal] line: nothing to say', launcher_linux.last_fatal(logp), '')

# ---- host_linux (its reads patched with fixed /proc and /sys text)
files = {
    '/proc/self/mounts': '/dev/nvme0n1p2 / ext4 rw 0 0\n/dev/sda1 /run/media/deck/SD vfat rw 0 0\n'
                         'server:/x /mnt/nas nfs4 rw 0 0\n/dev/sdb1 /mnt/usb\\040disk exfat rw 0 0\n',
    '/sys/block/sda/removable': '1\n', '/sys/block/nvme0n1/removable': '0\n', '/sys/block/sdb/removable': '0\n',
    '/proc/cpuinfo': 'processor : 0\nflags : fpu sse ssse3 sse4_1 sse4_2 popcnt avx avx2 fma bmi2\n',
    '/proc/meminfo': 'MemTotal:       16384000 kB\nMemAvailable:    8192000 kB\n',
}
host_linux.read = lambda p, default='': files.get(p, default)
check('ext4 root', host_linux.filesystem('/home/x/work'), 'ext4')
check('FAT32 SD card', host_linux.filesystem('/run/media/deck/SD/w'), 'FAT32')
check('removable SD', host_linux.dump_drive('/run/media/deck/SD/game.nsp'), 'removable')
check('network', host_linux.dump_drive('/mnt/nas/game.nsp'), 'network')
check('fixed disk', host_linux.dump_drive('/home/x/game.nsp'), 'fixed')
check('exFAT with a space in the mount point', host_linux.filesystem('/mnt/usb disk/w'), 'exFAT')
check('AVX2 level', host_linux.host_cpu(), 'v3')
check('memory', tuple(round(x, 1) for x in host_linux.memory_gb()), (15.6, 7.8))
files.update({'/proc/self/cgroup': '0::/user.slice/app.scope\n',
              '/sys/fs/cgroup/user.slice/app.scope/memory.max': '4294967296\n',
              '/sys/fs/cgroup/user.slice/app.scope/memory.current': '1073741824\n',
              '/sys/fs/cgroup/user.slice/app.scope/cpu.max': '200000 100000\n'})
check('memory within the cgroup', tuple(round(x, 1) for x in host_linux.memory_gb()), (4.0, 3.0))
check('CPUs within the cgroup quota', host_linux.cpu_count(), 2)
files['/sys/fs/cgroup/user.slice/app.scope/memory.max'] = 'max\n'
check('no memory limit', round(host_linux.memory_gb()[0], 1), 15.6)
# setup.log's PC line (TRIAGE.md: a report names its Linux without a follow-up question)
files.update({'/etc/os-release': 'NAME="SteamOS"\nPRETTY_NAME="SteamOS 3.6.19"\nID=steamos\n',
              '/proc/sys/kernel/osrelease': '6.5.0-valve22-1-neptune-65\n',
              '/proc/driver/nvidia/version': 'NVRM version: NVIDIA UNIX x86_64 Kernel Module  560.35.03  Fri Aug 16 2024\n'})
real_gpus = host_linux.gpus   # (fixed cards: on a Linux test machine /sys would list its own)
nv_file = '/proc/driver/nvidia/version'
host_linux.gpus = lambda: [['nvidia', files[nv_file].strip()]] if nv_file in files else []
check('PC line: distro, kernel, desktop / session, GPU driver',
      host_linux.pc_line({'XDG_CURRENT_DESKTOP': 'KDE', 'XDG_SESSION_TYPE': 'wayland'}),
      'PC: SteamOS 3.6.19, kernel 6.5.0-valve22-1-neptune-65, KDE wayland, GPU NVIDIA 560.35.03')
files[nv_file] = 'NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  560.35.03  Release Build  (dvs-builder@U16)\n'
check('PC line: the NVIDIA open kernel modules too (RTX 20+ default)', host_linux.pc_line({}).split('GPU ')[-1],
      'NVIDIA 560.35.03')
files.pop('/etc/os-release'); files.pop('/proc/driver/nvidia/version')
check('PC line: nothing readable still says so', host_linux.pc_line({}),
      'PC: unknown distribution, kernel 6.5.0-valve22-1-neptune-65, no desktop, GPU none listed')
host_linux.gpus = real_gpus
files['/proc/cpuinfo'] = 'flags : fpu sse ssse3 sse4_1\n'
check('no SSE4.2 is a problem', len([p for p in host_linux.check_pc() if 'SSE4.2' in p]), 1)

# ---- builder's hot list names
import builder  # noqa: E402
check('hot list: Linux name', builder.HOT_NAME_LINUX.match('  _Z9f_1b4d760P3Ctx:').group(1), 'f_1b4d760')
check('hot list: Windows name', builder.HOT_NAME.match('  ?f_1b4d760@@YAXPEIAUCtx@@@Z:').group(1), 'f_1b4d760')
# Setup's step 8 on Linux fetches no FSR or XeSS runtime (Windows DLLs; ffx.c / xess.c have none off Windows)
import types  # noqa: E402
fetched_ffx, saved = [], (builder.LINUX, builder.FFX, builder.XESS, sys.modules.get('toolchain'))
sys.modules['toolchain'] = types.SimpleNamespace(download=lambda *a, **k: fetched_ffx.append(a))
builder.LINUX = True
builder.FFX = builder.XESS = {'tag': 'v0', 'name': 'x.zip', 'sha256': '0' * 64, 'size': 1}
builder.fsr_runtime(tempfile.gettempdir(), False)
fetched_xess = fetched_ffx[:]
builder.xess_runtime(tempfile.gettempdir(), False)
builder.LINUX, builder.FFX, builder.XESS = saved[0], saved[1], saved[2]
if saved[3] is not None:
    sys.modules['toolchain'] = saved[3]
else:
    del sys.modules['toolchain']
check('Linux Setup step 8: no FSR runtime download', fetched_xess, [])
check('Linux Setup step 8: no XeSS runtime download', fetched_ffx, [])

# ---- the public tree's tools/heavy.py on Linux (Setup's step 1 runs the RomFS extraction through it; linux-test D#82:
# AttributeError ctypes.windll): /proc/meminfo, no creationflags
import importlib.util  # noqa: E402
spec = importlib.util.spec_from_file_location('heavy_pub', os.path.join(REPO, 'tools', 'release', 'public', 'tools', 'heavy.py'))
heavy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(heavy)
with tempfile.TemporaryDirectory() as t:
    heavy.LINUX, heavy.MEMINFO = True, os.path.join(t, 'meminfo')
    open(heavy.MEMINFO, 'w').write('MemTotal:       8000000 kB\nMemFree:  300000 kB\nMemAvailable:   4194304 kB\n'
                                   'SwapFree:  2097152 kB\n')
    check('public heavy.py, Linux: free RAM = MemAvailable', round(heavy.free_gb(), 2), 4.0)
    check('public heavy.py, Linux: free commit = MemAvailable + SwapFree', round(heavy.free_commit_gb(), 2), 6.0)
    calls = []
    heavy.subprocess = types.SimpleNamespace(call=lambda cmd, **kw: calls.append((cmd, kw)) or 0)
    heavy.os = types.SimpleNamespace(name='posix', nice=os.nice if hasattr(os, 'nice') else None)
    rc = heavy.main(['--prio', 'low', '--', 'true'])
    check('public heavy.py, Linux: runs the command, niced, no creationflags', (rc, calls[0][0], sorted(calls[0][1]),
          callable(calls[0][1]['preexec_fn'])), (0, ['true'], ['preexec_fn'], True))
    heavy.MEMINFO = os.path.join(t, 'none')
    check('public heavy.py, Linux: /proc/meminfo unreadable: no gate, no Windows call', heavy.free_gb(), float('inf'))

# ---- Setup's step 1 on Linux (pipeline/build.py's romfs step: heavy(cmd) wraps the extraction), with ctypes.windll
# gone as on Linux: the command runs as it is, through the RAM gate, never through tools/heavy.py (D#82). pipeline/build.py
# needs cryptography + lz4 (it re-runs itself under the work venv otherwise): run this file with Setup's or the work
# folder's venv Python for these two checks
import ctypes, importlib.util as ilu  # noqa: E402
saved_windll, saved_platform = getattr(ctypes, 'windll', None), sys.platform
sys.path.insert(0, REPO)
try:
    if not (ilu.find_spec('cryptography') and ilu.find_spec('lz4')):
        raise ImportError('cryptography / lz4')
    if saved_windll is not None:
        del ctypes.windll
    sys.platform = 'linux'
    import pipeline.build as pbuild  # noqa: E402
    gated = []
    pbuild.wait_for_ram = lambda log=print: gated.append(1)
    cmd = [sys.executable, '-m', 'pipeline.extract', 'romfs', '--nsp', 'game.nsp', '--out', 'x']
    check('Linux Setup step 1: the extraction runs as it is, not through tools/heavy.py', (pbuild.heavy(cmd), gated),
          (cmd, [1]))
    # the RAM gate's read (pipeline/nx/common.py _memory_status): /proc/meminfo's MemAvailable, page cache included, not
    # MemFree; the imports above, pipeline.build -> extract / nx, ran without windll
    from pipeline.nx import common as pcommon  # noqa: E402
    saved_meminfo = pcommon.MEMINFO
    with tempfile.TemporaryDirectory() as t:
        pcommon.MEMINFO = os.path.join(t, 'meminfo')
        open(pcommon.MEMINFO, 'w').write('MemTotal:       8000000 kB\nMemFree:          300000 kB\n'
                                         'MemAvailable:   5000000 kB\nCached:  4500000 kB\n')
        check('Linux Setup step 1: the RAM gate counts MemAvailable (page cache), not MemFree',
              pcommon._memory_status(), (5000000 * 1024, 8000000 * 1024))
    pcommon.MEMINFO = saved_meminfo
except ImportError as e:
    print(f'SKIP Linux Setup step 1 ({e} not in this Python: run with a venv that has them)')
finally:
    sys.platform = saved_platform
    if saved_windll is not None:
        ctypes.windll = saved_windll

# ---- the AppImage's Setup on a drive without symbolic links (FAT32, exFAT, a Windows drive from WSL): a reason and exit 2
# before 'copying Python', not a traceback (the linux-test-v0.1.9.1 e2e run)
import shutil as _sh  # noqa: E402
with tempfile.TemporaryDirectory() as t:
    app = os.path.join(t, 'app')
    os.makedirs(app)
    _sh.copy(os.path.join(REPO, 'tools', 'linux', 'appimage', 'setup_gui.py'), app)
    open(os.path.join(app, 'release.json'), 'w').write('{"tag": "linux-test-x", "repo": "o/r", "commit": "' + '0' * 40 + '"}')
    spec = importlib.util.spec_from_file_location('setup_gui_t', os.path.join(app, 'setup_gui.py'))
    sg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sg)
    inst, said = os.path.join(t, 'inst'), []
    os.makedirs(inst)
    real_symlink = os.symlink

    def no_symlinks(*a, **k):
        raise PermissionError(1, 'Operation not permitted')
    sg.os.symlink = no_symlinks
    sg.tools_python = lambda *a: (_ for _ in ()).throw(AssertionError('reached tools_python'))
    try:
        rc = sg._install(inst, ['game.nsp', 'prod.keys'], None, said.append, None)
    finally:
        sg.os.symlink = real_symlink
    check('AppImage Setup, no symlinks: exit 2 with the reason, before the Python copy',
          (rc, 'without symbolic links' in ' '.join(said), os.listdir(inst)), (2, True, []))
    sg.os.symlink = lambda src, dst: open(dst, 'w').close()   # (a file system that has them; Windows may not let a test make one)
    try:
        check('AppImage Setup, symlinks: the probe passes and leaves nothing', (sg.symlinks_ok(inst), os.listdir(inst)), (True, []))
    finally:
        sg.os.symlink = real_symlink

print('all passed' if not fails else f'{fails} FAILED')
sys.exit(1 if fails else 0)
