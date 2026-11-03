"""Read one file out of a built AppImage, on any system (linux-port, 2026-11-03): release_publish.py checks the commit an
AppImage was made for (usr/app/release.json "commit", make_appimage.sh) against the tag's before it uploads it, as it
does with the Setup's ProductVersion, and runs on Windows, where an AppImage can neither run nor mount.

A type-2 AppImage is the runtime (an ELF) with a squashfs 4.0 image appended. This finds the ELF's end, the squashfs
superblock there, walks the directory table to the path and returns the file (its blocks and its fragment).
Compressors: gzip, xz / lzma and zstd (appimagetool's default; the zstandard package), what AppImages are built with.

    python tools/linux/appimage_info.py <AppImage> [<path inside, default usr/app/release.json>]
    import appimage_info; appimage_info.release(path) -> {"tag", "repo", "commit"[, "bundle"]}
"""
import json
import lzma
import struct
import sys
import zlib

RELEASE_JSON = 'usr/app/release.json'
NO_FRAGMENT = 0xffffffff
UNCOMPRESSED = 1 << 24   # (a data block's or a fragment's size word: stored as is)


class Squash:
    def __init__(self, f):
        self.f = f
        ident = f.read(64)
        if ident[:4] != b'\x7fELF' or ident[4] != 2:
            raise ValueError('not a 64-bit ELF: an AppImage starts with its runtime')
        shoff, = struct.unpack_from('<Q', ident, 0x28)
        shentsize, shnum = struct.unpack_from('<HH', ident, 0x3a)
        # the runtime's end, as AppImage's own get_elf_size: the section header table's end or the last section's,
        # whichever is later; the image follows
        f.seek(shoff + shentsize * (shnum - 1) + 0x18)
        last_off, last_size = struct.unpack('<QQ', f.read(16))
        self.base = max(shoff + shentsize * shnum, last_off + last_size)
        sb = self.at(0, 96)
        if sb[:4] != b'hsqs':
            raise ValueError(f'no squashfs image after the runtime (offset {self.base})')
        self.block_size, = struct.unpack_from('<I', sb, 12)
        self.comp, = struct.unpack_from('<H', sb, 20)
        major, = struct.unpack_from('<H', sb, 28)
        if major != 4:
            raise ValueError(f'squashfs {major}, not 4')
        self.root, = struct.unpack_from('<Q', sb, 32)
        self.inode_table, self.dir_table, self.frag_table = struct.unpack_from('<QQQ', sb, 64)

    def at(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def unpack(self, data):
        if self.comp == 1:
            return zlib.decompress(data)
        if self.comp in (2, 4):
            return lzma.decompress(data)
        if self.comp == 6:
            import zstandard
            return zstandard.ZstdDecompressor().decompress(data, max_output_size=self.block_size)
        raise ValueError(f'squashfs compressor {self.comp}: not read here (gzip 1, lzma 2, xz 4, zstd 6)')

    def meta(self, start, offset, n):
        """n bytes of a metadata stream (blocks of <= 8 KiB, each with a 2-byte header) from offset in its block at start"""
        out = b''
        while len(out) < offset + n:
            hdr, = struct.unpack('<H', self.at(start, 2))
            raw = self.at(start + 2, hdr & 0x7fff)
            out += raw if hdr & 0x8000 else self.unpack(raw)
            start += 2 + (hdr & 0x7fff)
        return out[offset:offset + n]

    def inode(self, ref, n=64):
        return self.meta(self.inode_table + (ref >> 16), ref & 0xffff, n)

    def entries(self, ino):
        """a directory inode's (name, inode ref) pairs"""
        kind, = struct.unpack_from('<H', ino, 0)
        if kind == 1:     # basic directory
            start, _, size, off = struct.unpack_from('<IIHH', ino, 16)
        elif kind == 8:   # extended directory
            _, size, start = struct.unpack_from('<III', ino, 16)
            off, = struct.unpack_from('<H', ino, 34)
        else:
            raise NotADirectoryError
        listing, pos = self.meta(self.dir_table + start, off, size - 3), 0   # (size counts 3 bytes more)
        while pos < len(listing):
            count, istart, _ = struct.unpack_from('<IIi', listing, pos)
            pos += 12
            for _ in range(count + 1):
                eoff, _, _, nsize = struct.unpack_from('<HhHH', listing, pos)
                yield listing[pos + 8:pos + 9 + nsize].decode('utf-8', 'surrogateescape'), (istart << 16) | eoff
                pos += 9 + nsize

    def lookup(self, path):
        ref = self.root
        for name in [p for p in path.split('/') if p]:
            try:
                ref = dict(self.entries(self.inode(ref)))[name]
            except (KeyError, NotADirectoryError):
                raise FileNotFoundError(f'{path}: no {name} there') from None
        return ref

    def read(self, path):
        ref = self.lookup(path)
        ino = self.inode(ref)
        kind, = struct.unpack_from('<H', ino, 0)
        if kind == 2:     # basic file
            start, frag, foff, size = struct.unpack_from('<IIII', ino, 16)
            sizes_at = 32
        elif kind == 9:   # extended file
            start, size = struct.unpack_from('<QQ', ino, 16)
            frag, foff = struct.unpack_from('<II', ino, 44)
            sizes_at = 56
        else:
            raise FileNotFoundError(f'{path}: not a regular file')
        nblocks = size // self.block_size if frag != NO_FRAGMENT else -(-size // self.block_size)
        sizes = struct.unpack(f'<{nblocks}I', self.inode(ref, sizes_at + 4 * nblocks)[sizes_at:]) if nblocks else ()
        out = b''
        for word in sizes:
            n = word & 0xffffff
            if n == 0:   # (a sparse block)
                out += b'\0' * self.block_size
                continue
            raw = self.at(start, n)
            out += raw if word & UNCOMPRESSED else self.unpack(raw)
            start += n
        if frag != NO_FRAGMENT:
            loc, = struct.unpack('<Q', self.at(self.frag_table + 8 * (frag // 512), 8))
            fstart, fsize = struct.unpack('<QI', self.meta(loc, (frag % 512) * 16, 12))
            raw = self.at(fstart, fsize & 0xffffff)
            out += (raw if fsize & UNCOMPRESSED else self.unpack(raw))[foff:foff + size - len(out)]
        return out[:size]


def read_file(appimage, path=RELEASE_JSON):
    with open(appimage, 'rb') as f:
        return Squash(f).read(path)


def release(appimage):
    """the AppImage's release.json (one made before the commit pin, 2026-11-03, has no "commit")"""
    return json.loads(read_file(appimage).decode('utf-8'))


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    sys.stdout.write(read_file(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else RELEASE_JSON).decode('utf-8', 'replace'))
