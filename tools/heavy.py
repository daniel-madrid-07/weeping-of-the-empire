#!/usr/bin/env python3
"""Run one heavy job of the build (a compile step, the shader pack) once the PC has room for it.

    py tools/heavy.py [--prio low|normal|high] [--min-free-gb N] [--pool P] -- <command> [args...]

Waits until free RAM is at least --min-free-gb and free commit (the commit limit minus the commit charge: what new
allocations get before Windows grows the pagefile) is at least the pool's floor, then runs the command at below-normal
priority (--prio high: normal) and exits with its exit code. --slots, --exclusive and --max-min are accepted and
ignored (one build on one PC). tools/engine/build.py also reads free_commit_gb() and COMMIT_FLOOR_GB to size how many
compiles run at once. On Linux (the AppImage's Setup, which has no GlobalMemoryStatusEx: the first Linux test stopped
at Setup's step 1) the same reads come from /proc/meminfo, and the command runs niced instead of at BELOW_NORMAL.
"""
import argparse
import ctypes
import os
import subprocess
import sys
import time

LIGHT_POOLS = ('difftest', 'gpulite', 'drive')
COMMIT_FLOOR_GB = {'light': 3.0, 'heavy': 4.0}   # a compile's commit is ~0.3 GB, the largest ~2.2 GB
POLL_S = 5
BELOW_NORMAL_PRIORITY_CLASS = 0x4000
LINUX = sys.platform.startswith('linux')   # (native Linux; Wine / Proton is a Windows Python, on_wine)
MEMINFO = '/proc/meminfo' if LINUX else r'Z:\proc\meminfo'   # (Wine maps the Linux root to Z:)


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]


def mem_status():
    m = MEMORYSTATUSEX()
    m.dwLength = ctypes.sizeof(m)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m


_WINE = []


def on_wine():
    """this Windows build runs on Linux through Wine / Proton (ntdll exports wine_get_version)"""
    if not _WINE:
        try:
            k = ctypes.WinDLL('kernel32')   # (its own handle: the argtypes below change no other caller's)
            k.GetModuleHandleW.restype = ctypes.c_void_p
            k.GetProcAddress.restype = ctypes.c_void_p
            k.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            h = k.GetModuleHandleW('ntdll.dll')
            _WINE.append(bool(h and k.GetProcAddress(h, b'wine_get_version')))
        except (OSError, AttributeError):
            _WINE.append(False)
    return _WINE[0]


def linux_meminfo():
    """Linux, natively or under Wine / Proton: /proc/meminfo in GB ({'MemAvailable': ..., 'SwapFree': ...}), else
    None. Under Wine, Wine's
    GlobalMemoryStatusEx gives the free commit as MemFree + SwapFree, and Linux keeps most free RAM as page cache
    (reading the 16 GB dump fills it): the free commit read ~1 GB on a big PC, every compile waited under its commit
    floor and the build ran one compile at a time. MemAvailable is Linux's own count of what new allocations get at
    once, page cache included."""
    if not LINUX and not on_wine():
        return None
    try:
        out = {}
        with open(MEMINFO, encoding='ascii', errors='replace') as f:
            for ln in f:
                name, _, rest = ln.partition(':')
                parts = rest.split()
                if parts and parts[0].isdigit():
                    out[name.strip()] = int(parts[0]) / 2**20   # (kB)
        return out if 'MemAvailable' in out else None
    except OSError:
        return None


def free_gb():
    """free physical memory (Wine / Proton: Linux's MemAvailable)"""
    mi = linux_meminfo()
    if LINUX and not mi:   # (unreadable: no gate rather than a Windows call)
        return float('inf')
    return mi['MemAvailable'] if mi else mem_status().ullAvailPhys / 2**30


def free_commit_gb():
    """commit limit minus commit charge (Wine / Proton: Linux's MemAvailable + free swap, linux_meminfo)"""
    mi = linux_meminfo()
    if LINUX and not mi:
        return float('inf')
    return mi['MemAvailable'] + mi.get('SwapFree', 0.0) if mi else mem_status().ullAvailPageFile / 2**30


def commit_floor(pool):
    return COMMIT_FLOOR_GB['light' if pool in LIGHT_POOLS else 'heavy']


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if '--' not in argv and ('-h' in argv or '--help' in argv):
        print(__doc__)
        return 0
    if '--' not in argv:
        sys.exit('usage: heavy.py [options] -- <command> [args...]')
    i = argv.index('--')
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--prio', default='normal')
    ap.add_argument('--min-free-gb', type=float, default=2.0)
    ap.add_argument('--pool', default='heavy')
    ap.add_argument('--slots', type=int)
    ap.add_argument('--exclusive', action='store_true')
    ap.add_argument('--max-min', type=float)
    a = ap.parse_args(argv[:i])
    cmd = argv[i + 1:]
    if not cmd:
        sys.exit('heavy.py: no command after --')
    said = False
    while free_gb() < a.min_free_gb or free_commit_gb() < commit_floor(a.pool):
        if not said:
            print(f'[heavy] waiting for memory: free RAM {free_gb():.1f} GB (need {a.min_free_gb:g}), free commit '
                  f'{free_commit_gb():.1f} GB (need {commit_floor(a.pool):g})', flush=True)
            said = True
        time.sleep(POLL_S)
    low = a.prio != 'high'
    try:
        if os.name != 'nt':   # (creationflags is Windows' alone: Linux lowers the child with nice instead)
            return subprocess.call(cmd, preexec_fn=(lambda: os.nice(10)) if low else None)
        return subprocess.call(cmd, creationflags=BELOW_NORMAL_PRIORITY_CLASS if low else 0)
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    sys.exit(main())
