"""RomFS: stream the IVFC-verified level-5 image out of the Program NCA, index it, optionally unpack it.

Port of MacNativeGames `games/totk/scripts/extract_romfs.py` (MIT). Differences: decryption runs on a reader thread
(cryptography and hashlib release the GIL) so the 17 GB pass is I/O-bound, the image is written under a temporary
name and renamed only after every IVFC level verified, and `unpack()` can lay the files out as a directory tree.
"""
import hashlib
import os
import queue
import struct
import threading
from pathlib import Path

from .common import MiB, read_at, require
from .container import FS_ROMFS

CHUNK = 8 * MiB
NO_ENTRY = 0xFFFFFFFF


def find_romfs_section(sections):
    found = [s for s in sections if s.kind == FS_ROMFS]
    require(len(found) == 1, f'Expected one RomFS section, found {len(found)}')
    return found[0]


def ivfc_levels(fs):
    require(fs[8:12] == b'IVFC' and struct.unpack_from('<III', fs, 12) == (0x20000, 32, 7), 'Unsupported IVFC layout')
    return [struct.unpack_from('<QQII', fs, 0x18 + j * 24) for j in range(6)]


def _chunks(section, offset, size, depth=4):
    """Yield decrypted chunks of [offset, offset+size) in order, produced by a background reader thread."""
    q = queue.Queue(maxsize=depth)
    stop = threading.Event()

    def produce():
        try:
            for pos in range(0, size, CHUNK):
                if stop.is_set():
                    return
                q.put(section.read(offset + pos, min(CHUNK, size - pos)))
            q.put(None)
        except BaseException as exc:  # forwarded to the consumer
            q.put(exc)

    thread = threading.Thread(target=produce, daemon=True)
    thread.start()
    try:
        while True:
            item = q.get()
            if item is None:
                return
            if isinstance(item, BaseException):
                raise item
            yield item
    finally:
        stop.set()
        while thread.is_alive():
            try:
                q.get_nowait()
            except queue.Empty:
                thread.join(0.05)


def extract_image(section, image_path, log=print):
    """Verify all six IVFC levels and write level 5 (the RomFS image) to `image_path`.

    Returns dict(romfs_size, romfs_sha256, ivfc_blocks). Hash levels 0-4 stay in memory (tens of MB for TotK).
    """
    fs = section.fs
    levels = ivfc_levels(fs)
    image_path = Path(image_path)
    tmp = image_path.with_name(image_path.name + '.partial')
    digest = hashlib.sha256()
    counts = []
    parent = fs[0xc8:0xe8]  # master hash
    with open(tmp, 'wb') as image:
        for level, (offset, size, power, reserved) in enumerate(levels):
            require(12 <= power <= 24 and reserved == 0, 'IVFC block size')
            block = 1 << power
            require(len(parent) >= (size + block - 1) // block * 32, 'IVFC parent size')
            captured = bytearray() if level < 5 else None
            index = 0
            done = 0
            for chunk in _chunks(section, offset, size):
                view = memoryview(chunk)
                for j in range(0, len(chunk), block):
                    payload = view[j:j + block]
                    h = hashlib.sha256(payload)
                    if len(payload) < block:
                        h.update(bytes(block - len(payload)))
                    require(h.digest() == parent[index * 32:index * 32 + 32], f'IVFC level {level} block {index} failed')
                    index += 1
                if captured is not None:
                    captured.extend(chunk)
                else:
                    image.write(chunk)
                    digest.update(chunk)
                done += len(chunk)
                if level == 5 and done % (1024 * MiB) < CHUNK:
                    log(f'  romfs: {done / (1024 * MiB):.1f} / {size / (1024 * MiB):.1f} GiB verified')
            counts.append(index)
            if captured is not None:
                parent = bytes(captured)
            log(f'  IVFC level {level}: {index} blocks verified')
    os.replace(tmp, image_path)
    return dict(romfs_size=levels[-1][1], romfs_sha256=digest.hexdigest(), ivfc_blocks=counts)


def index_romfs(path):
    """List every file of a RomFS image: [{'path', 'offset', 'size'}] sorted by path; offsets are image-absolute."""
    path = Path(path)
    with path.open('rb') as f:
        return _index(f, path.stat().st_size)


def _index(f, length):
    h = struct.unpack('<10Q', read_at(f, 0, 80))
    require(h[0] == 80, 'RomFS header')
    for off, size in [(h[1], h[2]), (h[3], h[4]), (h[5], h[6]), (h[7], h[8])]:
        require(off + size <= length, 'RomFS table bounds')
    dirs = read_at(f, h[3], h[4])
    entries = read_at(f, h[7], h[8])
    files = []
    seen_dirs = set()
    seen_files = set()
    paths = set()

    def name(table, off, n):
        require(off + n <= len(table), 'Name bounds')
        s = table[off:off + n].decode('utf-8')
        require(s not in ('.', '..') and '/' not in s and '\\' not in s and '\0' not in s and ':' not in s,
                'Unsafe path')
        return s

    todo = [(0, '')]
    while todo:
        off, parent = todo.pop()
        require(off not in seen_dirs and off + 24 <= len(dirs), 'Invalid/cyclic directory')
        seen_dirs.add(off)
        _, sibling, child, file, _, n = struct.unpack_from('<6I', dirs, off)
        part = name(dirs, off + 24, n)
        prefix = parent + part + '/' if part else parent
        if sibling != NO_ENTRY:
            todo.append((sibling, parent))
        if child != NO_ENTRY:
            todo.append((child, prefix))
        while file != NO_ENTRY:
            require(file not in seen_files and file + 32 <= len(entries), 'Invalid/cyclic file')
            seen_files.add(file)
            _, next_file, data, size, _, n = struct.unpack_from('<IIQQII', entries, file)
            part = name(entries, file + 32, n)
            require(part != '', 'Empty file name')
            filename = prefix + part
            require(filename not in paths and h[9] + data + size <= length, 'Duplicate/out-of-bounds file')
            paths.add(filename)
            files.append(dict(path=filename, offset=h[9] + data, size=size))
            file = next_file
    return sorted(files, key=lambda item: item['path'])


def unpack(image_path, files, out_dir, log=print):
    """Copy every indexed file out of the image into `out_dir/<path>`. Skips files already present at full size."""
    out_dir = Path(out_dir)
    total = 0
    with open(image_path, 'rb') as f:
        for n, entry in enumerate(files):
            target = out_dir / entry['path']
            require(out_dir in target.parents, 'Unsafe path')
            if target.exists() and target.stat().st_size == entry['size']:
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            tmp = target.with_name(target.name + '.partial')
            with open(tmp, 'wb') as out:
                f.seek(entry['offset'])
                left = entry['size']
                while left:
                    data = f.read(min(CHUNK, left))
                    require(data, 'Truncated RomFS image')
                    out.write(data)
                    left -= len(data)
            os.replace(tmp, target)
            total += entry['size']
            if (n + 1) % 20000 == 0:
                log(f'  unpacked {n + 1}/{len(files)} files')
    return total


class RomFS:
    """Read-only access to files inside a RomFS image by path (for tools that do not want an unpacked tree)."""

    def __init__(self, image_path, files=None):
        self.path = Path(image_path)
        self.files = {e['path']: e for e in (files if files is not None else index_romfs(self.path))}

    def read(self, path):
        entry = self.files[path]
        with open(self.path, 'rb') as f:
            f.seek(entry['offset'])
            data = f.read(entry['size'])
        require(len(data) == entry['size'], 'Truncated RomFS image')
        return data
