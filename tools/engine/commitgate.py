"""Run a command once the PC's free commit is at least N GB (tools/engine/build.py puts every game-object compile
behind it, the largest ones with a higher floor), so a build never takes Windows into growing its pagefile.

    commitgate.py <GB> [--excl POOL] -- <command line>

Everything after the first ' -- ' goes to CreateProcess exactly as ninja wrote it. --excl is accepted and ignored.
While it waits it says so at once and then every minute, on stdout and in `commitgate.log` in the current directory.
Env: NXR_COMMITGATE_GB overrides the floor (0 = no wait); NXR_COMMITGATE_MAX_MIN (default 90) bounds the wait: past
it the step fails with exit code 75 instead of hanging the build, unless NXR_COMMITGATE_GO_ON=1, which runs the
command anyway (one person's PC: the game objects go again with fewer at once if memory was the problem). Exit
code: the command's, or 75.
"""
import ctypes
import os
import subprocess
import sys
import time

POLL = 10
GAVE_UP = 75
EXITS = 'compile_exits.log'   # (in the current directory: each failed compile's TU and exit code, build.py)


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]


_WINE = []


def wine_free_commit_gb():
    """heavy.py's count on Wine / Proton (MemAvailable + SwapFree from /proc/meminfo: Wine's GlobalMemoryStatusEx
    counts only MemFree, and Linux's page cache keeps that near zero), else None"""
    if not _WINE:   # (once per process: the wait polls every POLL seconds)
        try:
            k = ctypes.WinDLL('kernel32')
            k.GetModuleHandleW.restype = ctypes.c_void_p
            k.GetProcAddress.restype = ctypes.c_void_p
            k.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            h = k.GetModuleHandleW('ntdll.dll')
            _WINE.append(bool(h and k.GetProcAddress(h, b'wine_get_version')))
        except (OSError, AttributeError):
            _WINE.append(False)
    if not _WINE[0]:
        return None
    try:
        vals = {}
        with open(r'Z:\proc\meminfo', encoding='ascii', errors='replace') as f:
            for ln in f:
                name, _, rest = ln.partition(':')
                parts = rest.split()
                if parts and parts[0].isdigit():
                    vals[name.strip()] = int(parts[0]) / 2**20
        return vals['MemAvailable'] + vals.get('SwapFree', 0.0) if 'MemAvailable' in vals else None
    except OSError:
        return None


def free_commit_gb():
    """commit limit minus commit charge (Wine / Proton: wine_free_commit_gb)"""
    wine = wine_free_commit_gb()
    if wine is not None:
        return wine
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
    go_on = os.environ.get('NXR_COMMITGATE_GO_ON') == '1'   # (build.py, one person's PC: run after the bound)
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
            if go_on:
                say(f'[commitgate] {what}: going on after {max_min:g} min with free commit {free:.1f} GB < {floor:g} GB '
                    '(NXR_COMMITGATE_GO_ON)')
                break
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
    rc = subprocess.call(cmd)
    if rc:   # (build.py objects(): a first pass in which every compile crashed the same way stops there, public #75)
        try:
            with open(EXITS, 'a', encoding='utf-8') as f:
                f.write(f'{what}\t{rc}\n')
        except OSError:
            pass
    return rc


if __name__ == '__main__':
    sys.exit(main())
