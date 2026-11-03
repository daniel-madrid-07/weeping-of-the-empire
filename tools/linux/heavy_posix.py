"""heavy.py's memory answers on Linux, for tools/engine/build.py (linux-port, 2026-11-02).

build.py sizes its compiles from heavy.py (free_commit_gb, COMMIT_FLOOR_GB, mem_status, WINE_FLOORS). heavy.py itself
is the maintainers' Windows scheduler (msvcrt, kernel32) and does not run here; a Linux build is one person's PC, so only
the memory reads are needed. Linux has no commit limit to run out of the way Windows' pagefile does: what a compile can
take before the OOM killer acts is MemAvailable + free swap, the count heavy.py already uses on Wine / Proton
(heavy.linux_meminfo), so the floors are its Wine ones.
"""
import collections

# heavy.py's WINE_FLOORS: no pagefile behind the count, so build.py's giants wait for one giant's peak, not 30 GB
WINE_FLOORS = True
COMMIT_FLOOR_GB = collections.defaultdict(lambda: 2.0, {'heavy': 2.0, 'gpu': 2.0, 'gpulite': 1.0})


def meminfo_gb(path='/proc/meminfo'):
    """{'MemTotal': GB, 'MemAvailable': GB, 'SwapFree': GB, ...}"""
    out = {}
    with open(path) as f:
        for line in f:
            k, _, v = line.partition(':')
            parts = v.split()
            if parts and parts[0].isdigit():
                out[k] = int(parts[0]) / (1024 * 1024)   # (kB)
    return out


def free_commit_gb():
    m = meminfo_gb()
    return m.get('MemAvailable', 0.0) + m.get('SwapFree', 0.0)


class _MemStatus:
    def __init__(self, m):
        self.ullTotalPhys = int(m.get('MemTotal', 0.0) * 2**30)
        self.ullAvailPhys = int(m.get('MemAvailable', 0.0) * 2**30)


def mem_status():
    return _MemStatus(meminfo_gb())
