"""A RomFS level-5 image from a file tree, laid out as Nintendo's own images are, byte for byte: so an image built from
a dump's files (a NUP, nx/nup.py) can be checked against the pinned SHA-256 of the console's image.

The layout, measured on the 1.0.0 image, matches
the format as documented (switchbrew "RomFS"):
    header (0x50 bytes, 10 x u64: header size, dir hash table offset/size, dir table offset/size, file hash table
    offset/size, file table offset/size, data offset), zero to 0x200, the file data from 0x200, each file at a 16-aligned
    offset in the order of the file table, then the dir hash table, dir table, file hash table and file table back to back
    directories   depth first, pre-order, each directory's children by name (bytewise)
    files         by full path (bytewise)
    entries       dir: parent, sibling, first child dir, first file, hash next, name length, name (padded to 4);
                  file: parent, sibling, data offset (from the data start), size, hash next, name length, name
    hash tables   buckets = 3 below 3 entries, n | 1 below 19, else the first n >= count not divisible by 2..17's primes;
                  hash = rotr5-and-xor over the name from parent ^ 123456789; each entry put first in its bucket,
                  in table order
"""
import struct

NONE = 0xFFFFFFFF
DATA_START = 0x200


def buckets(n):
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        n += 1
    return n


def name_hash(parent, name):
    v = parent ^ 123456789
    for c in name:
        v = ((v >> 5) | (v << 27)) & 0xFFFFFFFF
        v ^= c
    return v


def _pad4(b):
    return b + bytes(-len(b) % 4)


class Layout:
    """the tables and every file's data offset for `files` = [(path, size)] ('/'-separated, no leading slash)"""

    def __init__(self, files):
        tree = {}        # dir path (bytes, '' root) -> (subdir names, file names)
        sizes = {}
        for path, size in files:
            p = path.encode('utf-8')
            parts = p.split(b'/')
            d = b''
            tree.setdefault(d, (set(), []))
            for part in parts[:-1]:
                tree[d][0].add(part)
                d = d + b'/' + part if d else part
                tree.setdefault(d, (set(), []))
            tree[d][1].append(parts[-1])
            sizes[p] = size
        # directories: depth first, pre-order, children by name
        order = []

        def walk(d):
            order.append(d)
            for k in sorted(tree[d][0]):
                walk(d + b'/' + k if d else k)
        import sys
        sys.setrecursionlimit(max(10000, sys.getrecursionlimit()))
        walk(b'')
        doff, o = {}, 0
        for d in order:
            doff[d] = o
            o += 24 + len(_pad4(d.rsplit(b'/', 1)[-1] if d else b''))
        dir_size = o
        # files: by full path
        fpaths = sorted(sizes)
        foff, o = {}, 0
        for p in fpaths:
            foff[p] = o
            o += 32 + len(_pad4(p.rsplit(b'/', 1)[-1]))
        file_size = o
        data, pos = {}, 0
        for p in fpaths:
            data[p] = pos
            pos = (pos + sizes[p] + 15) // 16 * 16
        self.data_offset = data            # {path bytes: offset from the data start}
        self.data_size = pos
        parent_of = lambda p: p.rsplit(b'/', 1)[0] if b'/' in p else b''   # noqa: E731
        join = lambda d, n: d + b'/' + n if d else n   # noqa: E731
        subs = {d: sorted(v[0]) for d, v in tree.items()}
        owns = {d: sorted(v[1]) for d, v in tree.items()}
        # dir entries
        dt = bytearray(dir_size)
        db = buckets(len(order))
        dh = [NONE] * db
        sib = {}   # entry -> the next one of its parent, by name
        for d in order:
            for a, b in zip(subs[d], subs[d][1:]):
                sib[join(d, a)] = join(d, b)
        for d in order:
            par = doff[parent_of(d)] if d else 0
            name = d.rsplit(b'/', 1)[-1] if d else b''
            nxt = doff[sib[d]] if d in sib else NONE
            first_file = foff[join(d, owns[d][0])] if owns[d] else NONE
            first_dir = doff[join(d, subs[d][0])] if subs[d] else NONE
            b = name_hash(par, name) % db
            struct.pack_into('<6I', dt, doff[d], par, nxt, first_dir, first_file, dh[b], len(name))
            dt[doff[d] + 24:doff[d] + 24 + len(name)] = name
            dh[b] = doff[d]
        ft = bytearray(file_size)
        fb = buckets(len(fpaths))
        fh = [NONE] * fb
        fsib = {}
        for d in order:
            for a, b in zip(owns[d], owns[d][1:]):
                fsib[join(d, a)] = join(d, b)
        for p in fpaths:
            d = parent_of(p)
            name = p.rsplit(b'/', 1)[-1]
            b = name_hash(doff[d], name) % fb
            struct.pack_into('<IIQQII', ft, foff[p], doff[d], foff[fsib[p]] if p in fsib else NONE, data[p], sizes[p],
                             fh[b], len(name))
            ft[foff[p] + 32:foff[p] + 32 + len(name)] = name
            fh[b] = foff[p]
        self.order = fpaths
        dho = DATA_START + self.data_size
        self.tables = (struct.pack(f'<{db}I', *dh) + bytes(dt) + struct.pack(f'<{fb}I', *fh) + bytes(ft))
        dhs, dms, fhs, fms = 4 * db, dir_size, 4 * fb, file_size
        self.header = struct.pack('<10Q', 0x50, dho, dhs, dho + dhs, dms, dho + dhs + dms, fhs, dho + dhs + dms + fhs, fms,
                                  DATA_START).ljust(DATA_START, b'\0')
        self.size = dho + len(self.tables)


def build(files, read, out, digest=None, log=None):
    """write the image of files = [(path, size)] to the binary file `out` (read(path) yields the file's bytes in
    chunks); digest (a hashlib object) takes the image as it is written. Returns the Layout."""
    lay = Layout(files)
    size_of = {p.encode('utf-8'): s for p, s in files}
    by_name = {p.encode('utf-8'): p for p, _ in files}

    def put(b):
        out.write(b)
        if digest is not None:
            digest.update(b)
    put(lay.header)
    pos = 0
    for k, p in enumerate(lay.order):
        put(bytes(lay.data_offset[p] - pos))
        got = 0
        for chunk in read(by_name[p]):
            put(chunk)
            got += len(chunk)
        if got != size_of[p]:
            raise ValueError(f'{by_name[p]}: {got} bytes, expected {size_of[p]}')
        pos = lay.data_offset[p] + got
        if log and k % 20000 == 0 and k:
            log(f'  romfs: {k} / {len(lay.order)} files, {(DATA_START + pos) / 2**30:.1f} GiB')
    put(bytes(lay.data_size - pos))
    put(lay.tables)
    return lay
