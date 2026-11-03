"""builder.py's questions about this PC, answered on Linux (linux-port, 2026-11-02).

builder.py asks Windows through kernel32 / the registry; each of those functions starts with `if LINUX: return
host_linux.<same name>(...)`, so the Windows paths stay as they were and the Linux answers live here, one per question,
with the same meaning and return shape.
"""
import locale
import os
import posixpath
import re
import sys


def read(path, default=''):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return default


def settings_ini():
    """the game's settings.ini on Linux: the launcher's data folder (engine/launcher/linux/launcher_linux.py user_dirs:
    $XDG_DATA_HOME/nxrecomp/TotK-engine, next to userdata, where pc_settings.cpp reads it)"""
    base = os.environ.get('XDG_DATA_HOME') or os.path.join(os.path.expanduser('~'), '.local', 'share')
    return os.path.join(base, 'nxrecomp', 'TotK-engine', 'settings.ini')


def cjk():
    """the game's language is Korean or Chinese: settings.ini's game.language, else the desktop's (LANG / LC_ALL)"""
    for ln in read(settings_ini()).splitlines():
        k, _, v = ln.split('#', 1)[0].partition('=')
        if k.strip() == 'game.language' and v.strip():
            return v.strip().lower().startswith(('ko', 'zh'))
    lang = os.environ.get('LC_ALL') or os.environ.get('LC_MESSAGES') or os.environ.get('LANG') or (locale.getlocale()[0] or '')
    return lang.lower().startswith(('ko', 'zh'))


def cpu_flags():
    m = re.search(r'^flags\s*:\s*(.*)$', read('/proc/cpuinfo'), re.M)
    return set(m.group(1).split()) if m else set()


def vulkan_loader():
    """the system's Vulkan loader (libvulkan.so.1, from the distribution or the GPU driver) as the game will find it:
    loaded the way the dynamic linker loads it (any layout: NixOS, Flatpak runtimes), else None"""
    try:
        import ctypes
        ctypes.CDLL('libvulkan.so.1')
        return 'libvulkan.so.1'
    except OSError:
        return None


def check_pc():
    problems = []
    if not {'ssse3', 'sse4_1', 'sse4_2'} <= cpu_flags():
        problems.append('this processor lacks SSE4.2 (or SSSE3 / SSE4.1), which the game needs at the least '
                        '(x86-64-v2: Intel 2008+ Nehalem, AMD 2011+ Bulldozer)')
    if not vulkan_loader():
        problems.append('no Vulkan loader (libvulkan.so.1 missing): install your distribution\'s Vulkan packages and the '
                        'current driver for your graphics card (Mesa: mesa-vulkan-drivers; NVIDIA: its driver)')
    return problems


def cgroup_file(name):
    """this process's cgroup v2 file `name` ('' without one): /proc/self/cgroup's '0::<path>' under /sys/fs/cgroup"""
    for ln in read('/proc/self/cgroup').splitlines():
        if ln.startswith('0::'):
            return read('/sys/fs/cgroup' + ln[3:].rstrip('/') + '/' + name).strip()
    return ''


def memory_gb():
    """(total, available) physical memory in GB, within the cgroup's memory.max when there is one (a container or a
    systemd slice: what the build may really use before the OOM killer acts)"""
    m = {}
    for ln in read('/proc/meminfo').splitlines():
        k, _, v = ln.partition(':')
        if v.split() and v.split()[0].isdigit():
            m[k] = int(v.split()[0]) / 2**20
    total, avail = m.get('MemTotal', 16.0), m.get('MemAvailable', 8.0)
    lim, cur = cgroup_file('memory.max'), cgroup_file('memory.current')
    if lim.isdigit():
        total = min(total, int(lim) / 2**30)
        if cur.isdigit():
            avail = min(avail, max(0.0, (int(lim) - int(cur)) / 2**30))
    return total, avail


def cpu_count():
    """the CPUs this process may use: its affinity, within the cgroup's cpu.max quota"""
    try:
        n = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        n = os.cpu_count() or 1
    quota = cgroup_file('cpu.max').split()
    if len(quota) == 2 and quota[0].isdigit() and quota[1].isdigit() and int(quota[1]):
        n = min(n, max(1, int(quota[0]) // int(quota[1])))
    return n


def mount_of(path):
    """(mount point, fs type, device) of the file system path is on (/proc/self/mounts' longest matching mount point)"""
    # (realpath needs no existing file: a work folder not made yet resolves through its existing parents; off Linux,
    # the tests, the path is taken as it is)
    real = os.path.realpath(path) if sys.platform.startswith('linux') else posixpath.normpath(path)
    best = ('/', '', '')
    for ln in read('/proc/self/mounts').splitlines():
        parts = ln.split()
        if len(parts) < 3:
            continue
        mp = parts[1].replace('\\040', ' ')
        if (real == mp or real.startswith(mp.rstrip('/') + '/')) and len(mp) >= len(best[0]):
            best = (mp, parts[2], parts[0])
    return best


def filesystem(path):
    """the file system's name in Windows' words where builder.py tests them ('FAT32' cannot hold the 17 GB romfs.bin);
    else Linux's own (ext4, btrfs, exfat...)"""
    fs = mount_of(path)[1]
    return {'vfat': 'FAT32', 'msdos': 'FAT', 'exfat': 'exFAT', 'ntfs': 'NTFS', 'ntfs3': 'NTFS', 'fuseblk': 'NTFS?'}.get(fs, fs)


def dump_drive(path):
    """'fixed', or 'removable' / 'usb' / 'network' (a drive that can go away: the game's data is copied out then)"""
    mp, fs, dev = mount_of(path)
    if fs in ('nfs', 'nfs4', 'cifs', 'smb3', 'sshfs', 'fuse.sshfs', '9p'):
        return 'network'
    name = posixpath.basename(os.path.realpath(dev) if sys.platform.startswith('linux') else dev) if dev.startswith('/dev/') else ''
    block = re.sub(r'p?\d+$', '', name) if not name.startswith('dm-') else name
    if block and read(f'/sys/block/{block}/removable').strip() == '1':
        return 'removable'
    if block and sys.platform.startswith('linux') and '/usb' in os.path.realpath(f'/sys/block/{block}'):
        return 'usb'
    return 'fixed'


def host_cpu():
    """'v3' with AVX2 (and FMA, BMI2: x86-64-v3), else 'v2'"""
    return 'v3' if {'avx2', 'fma', 'bmi2'} <= cpu_flags() else 'v2'


def gpus():
    """[[name, driver]] of the display adapters (the shaders step's stamp: a new GPU or driver redoes it)"""
    out = []
    for card in sorted(os.listdir('/sys/class/drm')) if os.path.isdir('/sys/class/drm') else []:
        if not re.fullmatch(r'card\d+', card):
            continue
        dev = f'/sys/class/drm/{card}/device'
        ue = dict(ln.split('=', 1) for ln in read(f'{dev}/uevent').splitlines() if '=' in ln)
        drv = ue.get('DRIVER', '')
        ver = read(f'/sys/module/{drv}/version').strip() if drv else ''
        out.append([ue.get('PCI_ID', card), f'{drv} {ver}'.strip()])
    nv = read('/proc/driver/nvidia/version').splitlines()   # (NVIDIA's driver version, where the module shows none)
    if nv:
        out.append(['nvidia', nv[0].strip()])
    return out
