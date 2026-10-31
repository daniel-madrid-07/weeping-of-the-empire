"""Run a command once the PC's free commit is at least N GB (tools/engine/build.py puts every game-object compile
behind it, the largest ones with a higher floor), so a build never takes Windows into growing its pagefile.

    commitgate.py <GB> [--excl POOL] -- <command line>

Everything after the first ' -- ' goes to CreateProcess exactly as ninja wrote it. --excl is accepted and ignored.
While it waits it says so at once and then every minute, on stdout and in `commitgate.log` in the current directory.
Env: NXR_COMMITGATE_GB overrides the floor (0 = no wait); NXR_COMMITGATE_MAX_MIN (default 90) bounds the wait: past
it the step fails with exit code 75 instead of hanging the build. Exit code: the command's, or 75.
"""
import ctypes
import os
import subprocess
import sys
import time

POLL = 10
GAVE_UP = 75


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]


def free_commit_gb():
    """commit limit minus commit charge"""
    m = MEMORYSTATUSEX()
    m.dwLength = ctypes.sizeof(m)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.ullAvailPageFile / 2**30


def say(msg):
    print(msg, flush=True)
    try:
        with open('commitgate.log', 'a', encoding='utf-8') as f:
            f.write(time.strftime('%H:%M:%S ') + msg + '\n')
    except OSError:
        pass


def main():
    floor = float(os.environ.get('NXR_COMMITGATE_GB') or sys.argv[1])
    max_min = float(os.environ.get('NXR_COMMITGATE_MAX_MIN') or 90)
    line = ctypes.windll.kernel32.GetCommandLineW
    line.restype = ctypes.c_wchar_p
    raw = line()
    cmd = raw[raw.index(' -- ') + 4:]
    what = next((w for w in cmd.split() if w.endswith('.cpp')), cmd[:60])
    t0, last = time.time(), None
    while floor > 0:
        free = free_commit_gb()
        if free >= floor:
            break
        if time.time() - t0 > max_min * 60:
            say(f'[commitgate] {what}: gave up after {max_min:g} min with free commit {free:.1f} GB < {floor:g} GB '
                '(NXR_COMMITGATE_GB / NXR_COMMITGATE_MAX_MIN to change)')
            return GAVE_UP
        if last is None or time.time() - last >= 60:
            say(f'[commitgate] {what}: waiting {(time.time() - t0) / 60:.0f} min, free commit {free:.1f} GB < '
                f'{floor:g} GB')
            last = time.time()
        time.sleep(POLL)
    if last is not None:
        say(f'[commitgate] {what}: starting after {(time.time() - t0) / 60:.1f} min')
    return subprocess.call(cmd)


if __name__ == '__main__':
    sys.exit(main())
