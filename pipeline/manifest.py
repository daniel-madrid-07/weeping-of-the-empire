"""Build manifest: a step is skipped when its recorded input identity is unchanged and its outputs still verify.

<work>/build-manifest.json = {"format": 1, "steps": {name: {"inputs": {...}, "outputs": {relpath: record},
                                                         "report": {...}, "seconds": s, "finished": iso}}}
An output record is {"size", "mtime_ns"} plus "sha256" for files up to HASH_LIMIT (bigger files, i.e. the 17 GB RomFS
image whose hash was verified when it was written, are bound by size + mtime instead of being re-read on every run).
"""
import hashlib
import os
import time
from pathlib import Path

from .locks import FileLock
from .nx.common import read_json, sha256_file, write_json

HASH_LIMIT = 256 * 1024 * 1024
FORMAT = 1


def file_record(path, hash_limit=HASH_LIMIT):
    st = Path(path).stat()
    rec = dict(size=st.st_size, mtime_ns=st.st_mtime_ns)
    if st.st_size <= hash_limit:
        rec['sha256'] = sha256_file(path)
    return rec


def record_matches(path, rec):
    path = Path(path)
    if not path.is_file():
        return False
    st = path.stat()
    if st.st_size != rec['size']:
        return False
    if 'sha256' in rec:
        return sha256_file(path) == rec['sha256']
    return st.st_mtime_ns == rec['mtime_ns']


def tree_digest(root, pattern='**/*'):
    """Identity of a directory tree from names, sizes and mtimes (cheap; used for big unpacked trees)."""
    root = Path(root)
    h = hashlib.sha256()
    count = 0
    for p in sorted(root.glob(pattern)):
        if p.is_file():
            st = p.stat()
            h.update(f'{p.relative_to(root).as_posix()}\0{st.st_size}\0{st.st_mtime_ns}\n'.encode())
            count += 1
    return dict(files=count, digest=h.hexdigest())


class Manifest:
    """build-manifest.json, shared by every build tree: each write re-reads the file and merges its own change under
    <work>/locks/manifest.lock, so concurrent runs on different trees never drop each other's entries."""

    def __init__(self, work):
        self.work = Path(work)
        self.path = self.work / 'build-manifest.json'
        self.lock = FileLock(self.work / 'locks' / 'manifest.lock', what='build-manifest.json')
        self.data = self._load()

    def _load(self):
        data = read_json(self.path) if self.path.exists() else {}
        return data if data.get('format') == FORMAT else dict(format=FORMAT, steps={})

    def fresh(self, name, inputs):
        """True when step `name` ran with these inputs and every recorded output still verifies."""
        self.data = self._load()
        step = self.data['steps'].get(name)
        if not step or step.get('inputs') != inputs:
            return False
        return all(record_matches(self.work / rel, rec) for rel, rec in step['outputs'].items())

    def record(self, name, inputs, outputs, report=None, seconds=None):
        recs = {}
        for p in outputs:
            p = Path(p)
            rel = os.path.relpath(p, self.work).replace(os.sep, '/')
            recs[rel] = file_record(p)
        entry = dict(inputs=inputs, outputs=recs, report=report or {},
                     seconds=None if seconds is None else round(seconds, 1),
                     finished=time.strftime('%Y-%m-%dT%H:%M:%S'))
        with self.lock:
            self.data = self._load()
            self.data['steps'][name] = entry
            write_json(self.path, self.data)

    def setting(self, key, default=None):
        return self._load().get('settings', {}).get(key, default)

    def set_setting(self, key, value):
        """Store (or with value None remove) a persisted build setting such as decomp."""
        with self.lock:
            self.data = self._load()
            settings = self.data.setdefault('settings', {})
            if value is None:
                settings.pop(key, None)
            else:
                settings[key] = value
            write_json(self.path, self.data)

    def step(self, name):
        self.data = self._load()
        return self.data['steps'].get(name)
