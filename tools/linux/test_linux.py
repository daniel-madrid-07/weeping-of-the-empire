"""Tests of the Linux port's pure parts (linux-port, 2026-11-02); they run on Windows and Linux, no game, no compiler.

    python tools/linux/test_linux.py
  pgo_remap: MSVC -> Itanium names, records dropped / kept whole
  launcher_linux: the game's environment from player.txt and the XDG folders, the version's own saves, the cleared shell
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
    check('mods: game folder then player', env['TOTK_MODS_DIR'].split(';'), [os.path.join(pkg, 'mods'), os.path.join(data, 'mods')])
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
files['/proc/cpuinfo'] = 'flags : fpu sse ssse3 sse4_1\n'
check('no SSE4.2 is a problem', len([p for p in host_linux.check_pc() if 'SSE4.2' in p]), 1)

# ---- builder's hot list names
import builder  # noqa: E402
check('hot list: Linux name', builder.HOT_NAME_LINUX.match('  _Z9f_1b4d760P3Ctx:').group(1), 'f_1b4d760')
check('hot list: Windows name', builder.HOT_NAME.match('  ?f_1b4d760@@YAXPEIAUCtx@@@Z:').group(1), 'f_1b4d760')

print('all passed' if not fails else f'{fails} FAILED')
sys.exit(1 if fails else 0)
