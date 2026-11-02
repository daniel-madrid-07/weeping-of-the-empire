"""Small helpers shared by the container readers and the build steps."""
import ctypes
import hashlib
import json
import os
import sys
import time
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms

MiB = 1024 * 1024
GiB = 1024 * MiB


def require(condition, message):
    if not condition:
        raise ValueError(message)


def decrypt(key, mode, data):
    ctx = Cipher(algorithms.AES(key), mode).decryptor()
    return ctx.update(data) + ctx.finalize()


def read_at(f, offset, length):
    require(0 <= offset and 0 <= length <= 512 * MiB, 'Invalid/big read')
    f.seek(offset)
    data = f.read(length)
    require(len(data) == length, 'Truncated input')
    return data


def sha256_file(path, chunk=8 * MiB):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            data = f.read(chunk)
            if not data:
                break
            digest.update(data)
    return digest.hexdigest()


def write_json(path, value):
    """Write JSON atomically (temp file + rename) with LF line endings."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    with open(tmp, 'w', newline='\n', encoding='utf-8') as f:
        f.write(json.dumps(value, indent=2) + '\n')
    os.replace(tmp, path)


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def below_normal_priority():
    """Lower this process to BELOW_NORMAL (machine is shared). Children inherit it."""
    if sys.platform == 'win32':
        BELOW_NORMAL_PRIORITY_CLASS = 0x4000
        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)
    else:
        try:
            os.nice(5)
        except OSError:
            pass


def _memory_status():
    """(available, total) physical RAM in bytes, or None where it can't be read."""
    if sys.platform != 'win32':
        try:
            page = os.sysconf('SC_PAGE_SIZE')
            return os.sysconf('SC_AVPHYS_PAGES') * page, os.sysconf('SC_PHYS_PAGES') * page
        except (ValueError, OSError):
            return None

    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                    ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                    ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                    ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                    ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
    status = MEMORYSTATUSEX()
    status.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return None
    return status.ullAvailPhys, status.ullTotalPhys


def free_ram_bytes():
    st = _memory_status()
    return st[0] if st else None


def _solo_pc():
    """A player's own PC building once."""
    fleet = os.environ.get('NXR_FLEET_LOCKS') or r'C:\nxrecomp-work\locks'
    return bool(os.environ.get('NXR_SOLO_PC')) and not os.path.isdir(fleet)


def wait_for_ram(min_free=6 * GiB, log=print, poll=5.0, max_wait=None):
    """Block while the machine has less than `min_free` physical RAM available (shared-machine rule). The bar is at
    most a third of the machine's RAM, and on a player's own PC the step goes on after `max_wait` seconds (default
    120 there, whose low-memory reaper the rule is for): an 8 GB PC never has 6 GiB free, and Setup's extraction
    stood still at 'waiting: only 4.8 GiB RAM free' forever (a player's setup.log, 2026-11-01)."""
    st = _memory_status()
    if st is None:
        return
    min_free = min(min_free, st[1] / 3)
    if max_wait is None and _solo_pc():
        max_wait = 120.0
    t0, warned = time.monotonic(), False
    while True:
        free = free_ram_bytes()
        if free is None or free >= min_free:
            return
        if max_wait is not None and time.monotonic() - t0 >= max_wait:
            log(f'only {free / GiB:.1f} GiB RAM free after {max_wait:.0f} s: going on (it may be slower)')
            return
        if not warned:
            log(f'waiting: only {free / GiB:.1f} GiB RAM free (< {min_free / GiB:.1f} GiB)')
            warned = True
        time.sleep(poll)


def free_disk_bytes(path):
    import shutil
    path = Path(path)
    while not path.exists() and path.parent != path:
        path = path.parent
    return shutil.disk_usage(path).free
