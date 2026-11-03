"""NUP (.nup): a container of unencrypted Switch dumps (ExeFS files and RomFS trees, per file raw or zstd; a base game,
updates and add-ons in one file), read in place.

Written from the format's specification only: SPEC.md of github.com/a55uka/nup ("NUP - NX Package Format", 1.0-draft,
commit 744fb7e3, 2026-11-02). That repository carries no licence, so none of its code is used here: only the
documented format is followed.
Only what this reader implements is accepted: magic NUP\\x01 (format 1), header_version 1, superblock flags 0, codecs 0
(raw) and 1 (zstd); any other version or codec is refused with what it is, never guessed at.

Layout (all little-endian): a 0x80-byte superblock; the TOC at header_size (its SHA-256 in the superblock) holding the
package records (0x40), directories (0x18), files (0x50), the children array (node ids: < dir_count a directory, else a
file) and a string pool; then the blob region of 16-aligned extents. Every offset and size is checked against the file
before it is used, and every file's SHA-256 (over its uncompressed bytes) as it is read.
"""
import hashlib
import struct

from .common import MiB, read_at, require

MAGIC = b'NUP\x01'
TOC_MAGIC = b'TOC\x01'
HEADER_VERSION = 1
NONE = 0xFFFFFFFF
KIND_BASE, KIND_UPDATE, KIND_ADDON = 0, 1, 2
KINDS = {0: 'base', 1: 'update', 2: 'addon', 3: 'addon_update', 4: 'system'}
CODEC_RAW, CODEC_ZSTD = 0, 1
CODECS = {0: 'raw', 1: 'zstd', 2: 'lz4 (reserved)', 3: 'zstd-chunked (reserved)'}
MAX_TOC = 512 * MiB
CHUNK = 8 * MiB


def is_nup(f):
    f.seek(0)
    return f.read(4) == MAGIC


class Entry:
    """one file of a package: name, size, where its extent is and how it is stored"""
    __slots__ = ('name', 'size', 'offset', 'stored', 'sha256', 'codec')

    def __init__(self, name, size, offset, stored, sha256, codec):
        self.name, self.size, self.offset, self.stored, self.sha256, self.codec = name, size, offset, stored, sha256, codec


class Package:
    __slots__ = ('title_id', 'version', 'kind', 'flags', 'root', 'exefs', 'romfs', 'meta')

    def __init__(self, title_id, version, kind, flags, root, exefs, romfs, meta):
        self.title_id, self.version, self.kind, self.flags = title_id, version, kind, flags
        self.root, self.exefs, self.romfs, self.meta = root, exefs, romfs, meta

    @property
    def title(self):
        return f'{self.title_id:016X}'


class Nup:
    """an open .nup: packages, their trees, and reads of their files (checked against each file's SHA-256)"""

    def __init__(self, f, size):
        self.f, self.size = f, size
        require(size >= 0x80, 'Truncated NUP superblock')
        sb = read_at(f, 0, 0x80)
        require(sb[:3] == b'NUP', 'Expected a NUP file')
        require(sb[3] == 1, f'NUP format {sb[3]}: this Setup reads NUP format 1 only (a newer NUP tool wrote it)')
        header_size, header_version = struct.unpack_from('<HH', sb, 4)
        require(header_version == HEADER_VERSION,
                f'NUP header version {header_version}: this Setup reads version {HEADER_VERSION} only')
        toc_offset, toc_size = struct.unpack_from('<QQ', sb, 8)
        toc_sha = sb[0x18:0x38]
        blob_offset, file_size = struct.unpack_from('<QQ', sb, 0x38)
        package_count, flags = struct.unpack_from('<II', sb, 0x48)
        require(header_size >= 0x80 and toc_offset == header_size, 'Invalid NUP superblock (header size / TOC offset)')
        require(flags == 0, f'NUP flags {flags:#x}: unknown to this Setup (NUP format 1 has none)')
        require(file_size == size, 'NUP file size does not match its superblock (a short copy?)')
        require(toc_size % 16 == 0 and blob_offset % 16 == 0 and toc_offset + toc_size <= blob_offset <= size,
                'NUP regions out of bounds')
        require(1 <= package_count <= 4096 and 0x30 <= toc_size <= MAX_TOC, 'Invalid NUP package count / TOC size')
        toc = read_at(f, toc_offset, toc_size)
        require(hashlib.sha256(toc).digest() == toc_sha, 'NUP table of contents hash mismatch (a damaged file)')
        require(toc[:4] == TOC_MAGIC, 'Expected the NUP table of contents')
        pc, dir_count, file_count, pool_off, pool_size = struct.unpack_from('<IQQQQ', toc, 4)
        require(pc == package_count, 'NUP package count differs between superblock and TOC')
        dirs_at = 0x30 + pc * 0x40
        files_at = dirs_at + dir_count * 0x18
        kids_at = files_at + file_count * 0x50
        require(dir_count < NONE and file_count < NONE and kids_at <= toc_size, 'NUP TOC records out of bounds')
        require(pool_off + pool_size <= toc_size, 'NUP string pool out of bounds')
        self.blob_offset = blob_offset
        pool = toc[pool_off:pool_off + pool_size]

        def name(off, n):
            require(off + n <= pool_size, 'NUP name out of bounds')
            s = pool[off:off + n].decode('utf-8')
            require(s not in ('', '.', '..') and not any(c in s for c in '/\\\0'), 'Unsafe NUP name')
            return s
        dirs = []
        slots = 0
        for i in range(dir_count):
            no, nl, first, count = struct.unpack_from('<IIII', toc, dirs_at + i * 0x18)
            dirs.append((no, nl, first, count))
            slots = max(slots, first + count)
        require(kids_at + slots * 4 <= toc_size, 'NUP children out of bounds')
        kids = struct.unpack_from(f'<{slots}I', toc, kids_at) if slots else ()
        files = []
        for i in range(file_count):
            o = files_at + i * 0x50
            no, nl, fsize, eoff, esize = struct.unpack_from('<IIQQQ', toc, o)
            codec = toc[o + 0x40]
            require(codec in (CODEC_RAW, CODEC_ZSTD), f'NUP codec {CODECS.get(codec, codec)}: this Setup reads raw and zstd only')
            if esize == 0:
                require(fsize == 0, 'NUP file without data')
            else:
                require(eoff % 16 == 0 and blob_offset <= eoff and eoff + esize <= size, 'NUP extent out of bounds')
            if codec == CODEC_RAW:
                require(esize == fsize, 'NUP raw extent size differs from the file size')
            files.append((no, nl, fsize, eoff, esize, toc[o + 0x20:o + 0x40], codec))
        self.dirs, self.files, self.kids, self.name = dirs, files, kids, name
        self.dir_count = dir_count
        self.packages = []
        for i in range(pc):
            o = 0x30 + i * 0x40
            title_id, root, exefs, romfs, version = struct.unpack_from('<QIIII', toc, o)
            kind, pflags = toc[o + 0x18], toc[o + 0x19]
            meta_off, meta_size = struct.unpack_from('<QI', toc, o + 0x20)
            require(kind < 5, f'NUP package kind {kind}: unknown to this Setup')
            for d in (root, exefs, romfs):
                require(d == NONE or d < dir_count, 'NUP package directory out of range')
            require(root != NONE, 'NUP package without a root directory')
            require(meta_size == 0 or (meta_off + meta_size <= size), 'NUP package meta block out of bounds')
            self.packages.append(Package(title_id, version, kind, pflags, root,
                                         None if exefs == NONE else exefs, None if romfs == NONE else romfs,
                                         (meta_off, meta_size)))
        require(sum(p.kind == KIND_BASE for p in self.packages) <= 1, 'A NUP with more than one base package')

    def children(self, d):
        no, nl, first, count = self.dirs[d]
        for k in range(first, first + count):
            node = self.kids[k]
            require(node < self.dir_count + len(self.files), 'NUP child node out of range')
            yield node

    def walk(self, d, prefix=''):
        """[(path, Entry)] under directory d, '/'-joined paths, depth first; a directory met twice is refused"""
        out, seen, todo = [], set(), [(d, prefix)]
        while todo:
            d, pre = todo.pop()
            require(d not in seen, 'NUP directory tree has a loop')
            seen.add(d)
            names = set()
            for node in self.children(d):
                if node < self.dir_count:
                    n = self.name(*self.dirs[node][:2])
                    todo.append((node, pre + n + '/'))
                else:
                    no, nl, size, eoff, esize, sha, codec = self.files[node - self.dir_count]
                    n = self.name(no, nl)
                    out.append((pre + n, Entry(n, size, eoff, esize, sha, codec)))
                require(n not in names, 'NUP directory with two children of one name')
                names.add(n)
        return out

    def stream(self, e, chunk=CHUNK):
        """the file's bytes in chunks, its size and SHA-256 checked at the end (a mismatch raises after the last)"""
        h, got = hashlib.sha256(), 0
        if e.codec == CODEC_RAW:
            for pos in range(0, e.size, chunk):
                data = read_at(self.f, e.offset + pos, min(chunk, e.size - pos))
                h.update(data)
                got += len(data)
                yield data
        else:
            import zstandard

            class Extent:   # (the extent as a forward file: each read seeks, the handle is shared)
                def __init__(s):
                    s.pos = 0

                def read(s, n=-1):
                    n = e.stored - s.pos if n is None or n < 0 else min(n, e.stored - s.pos)
                    if n <= 0:
                        return b''
                    data = read_at(self.f, e.offset + s.pos, n)
                    s.pos += n
                    return data
            try:
                r = zstandard.ZstdDecompressor(max_window_size=1 << 31).stream_reader(Extent(), read_size=MiB)
                while got <= e.size:
                    data = r.read(chunk)
                    if not data:
                        break
                    h.update(data)
                    got += len(data)
                    yield data
            except zstandard.ZstdError as x:
                raise ValueError(f'NUP data damaged ({e.name}: {x})') from None
        require(got == e.size, f'NUP file {e.name}: {got} bytes, its record says {e.size}')
        require(h.digest() == e.sha256, f'NUP file {e.name}: SHA-256 mismatch (a damaged file)')

    def read(self, e):
        require(e.size <= 512 * MiB, f'NUP file {e.name} too big to read whole')
        return b''.join(self.stream(e))

    def meta(self, p):
        """{TLV type: payload} of a package's meta block (unknown types kept, as the spec lets readers skip them)"""
        off, size = p.meta
        out, pos = {}, 0
        raw = read_at(self.f, off, size) if size else b''
        while pos + 8 <= size:
            t, _, n = struct.unpack_from('<HHI', raw, pos)
            if pos + 8 + n > size:
                break
            out[t] = raw[pos + 8:pos + 8 + n]
            pos += 8 + n
        return out


def base_package(nup, title_id):
    """the base game's package of `title_id` (its updates and add-ons are left alone)"""
    bases = [p for p in nup.packages if p.kind == KIND_BASE]
    require(bases, 'The NUP has no base game (an update-only NUP): choose the base game')
    p = bases[0]
    require(p.title == title_id, f'Expected base game {title_id}; this NUP holds {p.title}')
    require(p.exefs is not None and p.romfs is not None, 'The NUP base game has no ExeFS or no RomFS')
    return p


def is_nup_path(path):
    """the file at path is a NUP (its magic, whatever its name)"""
    try:
        with open(path, 'rb') as f:
            return f.read(3) == b'NUP'
    except OSError:
        return False


def update_package(nup, update_title, version=None):
    """the update package of the game whose update title is `update_title` (its record may carry the update's id or the
    base's, as the dump tool read it: SPEC.md 3.1, "verbatim"): the one of `version` (NCM, e.g. 720896 = 1.4.3) if
    there, else the newest. Its RomFS is its own whole tree (an update deletes base files too: 727 of 1.0.0's in 1.4.3),
    which the pinned hash then checks."""
    base_title = f'{int(update_title, 16) & ~0xFFF:016X}'
    ups = [p for p in nup.packages if p.kind == KIND_UPDATE and p.title in (update_title, base_title)]
    require(ups, f'The NUP has no update of {base_title} (no update package)')
    exact = [p for p in ups if version is not None and p.version == version]
    p = exact[0] if exact else max(ups, key=lambda p: p.version)
    require(p.exefs is not None and p.romfs is not None, 'The NUP update has no ExeFS or no RomFS')
    return p


def open_package(path, title_id, update=False, version=None):
    """(Nup, its base package of title_id, or with update its update package of the update title_id); the caller
    closes nup.f"""
    import os
    f = open(path, 'rb')
    try:
        n = Nup(f, os.path.getsize(path))
        return n, (update_package(n, title_id, version) if update else base_package(n, title_id))
    except BaseException:
        f.close()
        raise


def open_base(path, title_id):
    """(Nup, base package) of the NUP at path; the caller closes nup.f"""
    return open_package(path, title_id)


def exefs_files(nup, p):
    """[(name, Entry)] of the package's ExeFS (a flat directory, as the console's)"""
    out = nup.walk(p.exefs)
    require(all('/' not in path for path, _ in out), 'The NUP ExeFS has subdirectories')
    require(any(path == 'main.npdm' for path, _ in out), 'The NUP ExeFS has no main.npdm')
    return out
