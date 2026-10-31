"""Cross-process file locks for build.py (msvcrt.locking on Windows, as tools/heavy.py; flock elsewhere).

    <work>/locks/tree[-<variant>][-decomp].lock   one build tree (generated*, build/cmake*, out/TotK*), whole run
    <work>/locks/shared.lock                      data every tree reads (extracted/, images/, elf/, layout.json,
                                                  patched/), held only while it is written or snapshotted
    <work>/locks/manifest.lock                    read-merge-write of build-manifest.json

Lock order: a tree lock is taken first and held; shared/manifest locks are taken and released inside it, never the
other way round, so two runs on different trees cannot deadlock.
"""
import os
import sys
import time
from pathlib import Path


class FileLock:
    """Exclusive lock on `path`; waits (printing once a minute who holds it) until it is free."""

    def __init__(self, path, log=print, what=None):
        self.path = Path(path)
        self.log = log
        self.what = what or self.path.name
        self.f = None
        self.depth = 0

    @property
    def owner_file(self):
        return self.path.with_name(self.path.name + '.owner')

    def _try(self):
        try:
            if os.name == 'nt':
                import msvcrt
                self.f.seek(0)
                msvcrt.locking(self.f.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False

    def __enter__(self):
        self.depth += 1
        if self.depth > 1:   # re-entrant within one process
            return self
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.f = open(self.path, 'a+')
        last = 0.0
        while not self._try():
            if time.time() - last > 60:
                try:
                    held = self.owner_file.read_text(encoding='utf-8', errors='replace').strip()
                except OSError:
                    held = '?'
                self.log(f'waiting: {self.what} is locked by another build.py ({held})')
                last = time.time()
            time.sleep(5)
        try:
            self.owner_file.write_text(f'pid {os.getpid()}, since {time.strftime("%H:%M:%S")}: {" ".join(sys.argv)}',
                                       encoding='utf-8')
        except OSError:
            pass
        return self

    def __exit__(self, *exc):
        self.depth -= 1
        if self.depth:
            return
        try:
            if os.name == 'nt':
                import msvcrt
                self.f.seek(0)
                msvcrt.locking(self.f.fileno(), msvcrt.LK_UNLCK, 1)
        except OSError:
            pass
        self.f.close()
        self.f = None
