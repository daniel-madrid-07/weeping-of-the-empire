#!/usr/bin/env python3
"""Run one heavy job of the build (a compile step, the shader pack) once the PC has room for it.

    py tools/heavy.py [--prio low|normal|high] [--min-free-gb N] [--pool P] -- <command> [args...]

Waits until free RAM is at least --min-free-gb and free commit (the commit limit minus the commit charge: what new
allocations get before Windows grows the pagefile) is at least the pool's floor, then runs the command at below-normal
priority (--prio high: normal) and exits with its exit code. --slots, --exclusive and --max-min are accepted and
ignored (one build on one PC). tools/engine/build.py also reads free_commit_gb() and COMMIT_FLOOR_GB to size how many
compiles run at once.
"""
import argparse
import ctypes
import subprocess
import sys
import time

LIGHT_POOLS = ('difftest', 'gpulite', 'drive')
COMMIT_FLOOR_GB = {'light': 3.0, 'heavy': 4.0}   # a compile's commit is ~0.3 GB, the largest ~2.2 GB
POLL_S = 5
BELOW_NORMAL_PRIORITY_CLASS = 0x4000


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


def free_gb():
    """free physical memory"""
    return mem_status().ullAvailPhys / 2**30


def free_commit_gb():
    """commit limit minus commit charge"""
    return mem_status().ullAvailPageFile / 2**30


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
    flags = 0 if a.prio == 'high' else BELOW_NORMAL_PRIORITY_CLASS
    try:
        return subprocess.call(cmd, creationflags=flags)
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    sys.exit(main())
