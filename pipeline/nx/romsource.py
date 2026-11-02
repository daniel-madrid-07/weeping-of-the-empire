"""The game's RomFS image read straight from the player's dump (v0.1.8: no 17 GB romfs.bin copy).

The extraction verifies every IVFC level and the pinned hash by streaming the dump once, as it did when it wrote
romfs.bin, and writes romfs-source.txt in its place: where the image (IVFC level 5) sits in the dump and how its
AES-CTR section decrypts. romfs-index.json names it as its image; readers open either through open_image().

romfs-source.txt, key=value lines (the runtime's hle_fs.cpp and the launcher read the same file):
    version=1
    dump=<the NSP / XCI, absolute>          dump_size=<bytes>   dump_mtime_ns=<ns>
    image_offset=<the image's first byte in the dump>   image_size=<bytes>
    crypt=ctr | none                         (none: a dump whose NCA sections were written decrypted)
    key=<the section's AES-128 key, hex>     ctr=<the section's CTR nonce, 8 bytes hex, as used big-endian>
    ctr_offset=<the image's offset in its NCA>   (block counter of image byte p = (ctr_offset + p) // 16)
    check=<sha256 of the dump's raw bytes [image_offset, image_offset + min(64 KiB, image_size))>
    romfs_sha256=<the image's sha256, verified at extraction>
The key comes from the player's own prod.keys on their PC. It is never printed; the file stays in their extraction
folder (never in the repo, a package or a diagnostics bundle).
"""
import hashlib
import io
import os
from pathlib import Path

SOURCE = 'romfs-source.txt'
CHECK_BYTES = 64 * 1024
FIELDS = ('version', 'dump', 'dump_size', 'dump_mtime_ns', 'image_offset', 'image_size', 'crypt', 'key', 'ctr',
          'ctr_offset', 'check', 'romfs_sha256')
INTS = ('version', 'dump_size', 'dump_mtime_ns', 'image_offset', 'image_size', 'ctr_offset')


def is_source(path):
    return Path(path).name.lower() == SOURCE


def raw_check(f, image_offset, image_size):
    f.seek(image_offset)
    return hashlib.sha256(f.read(min(CHECK_BYTES, image_size))).hexdigest()


def describe(section, dump, image_rel, image_size, romfs_sha256):
    """The source of an NcaSection's image at section-relative `image_rel` (IVFC level 5's offset)."""
    from .container import CRYPT_CTR, CRYPT_NONE
    from .common import require
    require(section.crypt in (CRYPT_CTR, CRYPT_NONE), f'NCA section {section.index}: neither AES-CTR nor plain')
    dump = Path(dump).resolve()
    st = dump.stat()
    image_offset = section.base + section.start + image_rel
    with open(dump, 'rb') as f:
        check = raw_check(f, image_offset, image_size)
    ctr = section.crypt == CRYPT_CTR
    return dict(version=1, dump=str(dump), dump_size=st.st_size, dump_mtime_ns=st.st_mtime_ns,
                image_offset=image_offset, image_size=image_size, crypt='ctr' if ctr else 'none',
                key=section.key.hex() if ctr else '', ctr=section.fs[0x140:0x148][::-1].hex() if ctr else '',
                ctr_offset=section.start + image_rel, check=check, romfs_sha256=romfs_sha256)


def write_source(path, src):
    path = Path(path)
    tmp = path.with_name(path.name + '.partial')
    lines = ['# the game RomFS read from your own game dump (pipeline/nx/romsource.py); holds a key made from your '
             'prod.keys: do not share this file'] + [f'{k}={src[k]}' for k in FIELDS]
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(tmp, path)


def read_source(path):
    src = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if '=' in line and not line.startswith('#'):
            k, v = line.split('=', 1)
            src[k.strip()] = v.strip()
    missing = [k for k in FIELDS if k not in src]
    if missing or src.get('version') != '1':
        raise ValueError(f'{path}: not a version 1 RomFS source (missing {", ".join(missing) or "version 1"})')
    for k in INTS:
        src[k] = int(src[k])
    return src


class DumpImage(io.RawIOBase):
    """The image as a read-only, seekable file: raw reads from the dump, AES-CTR decrypted (crypt=ctr)."""

    def __init__(self, source):
        self.src = read_source(source) if not isinstance(source, dict) else source
        self.size = self.src['image_size']
        if not os.path.isfile(self.src['dump']):
            raise FileNotFoundError(f"the game dump {self.src['dump']} is not there (moved or deleted)")
        if os.path.getsize(self.src['dump']) != self.src['dump_size']:
            raise OSError(f"the game dump {self.src['dump']} changed size since the extraction")
        self.f = open(self.src['dump'], 'rb')
        self.pos = 0
        if self.src['crypt'] == 'ctr':
            from cryptography.hazmat.primitives.ciphers import algorithms
            self.aes = algorithms.AES(bytes.fromhex(self.src['key']))
            self.nonce = bytes.fromhex(self.src['ctr'])
        else:
            self.aes = None

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = max(0, off if whence == 0 else self.pos + off if whence == 1 else self.size + off)
        return self.pos

    def pread(self, off, n):
        n = max(0, min(n, self.size - off))
        if not n:
            return b''
        if self.aes is None:
            self.f.seek(self.src['image_offset'] + off)
            return self.f.read(n)
        from cryptography.hazmat.primitives.ciphers import Cipher, modes
        skip = (self.src['ctr_offset'] + off) % 16
        self.f.seek(self.src['image_offset'] + off - skip)
        data = self.f.read(n + skip)
        block = (self.src['ctr_offset'] + off - skip) // 16
        dec = Cipher(self.aes, modes.CTR(self.nonce + block.to_bytes(8, 'big'))).decryptor()
        return (dec.update(data) + dec.finalize())[skip:]

    def read(self, n=-1):
        data = self.pread(self.pos, self.size - self.pos if n is None or n < 0 else n)
        self.pos += len(data)
        return data

    def readinto(self, b):
        data = self.read(len(b))
        b[:len(data)] = data
        return len(data)

    def close(self):
        if not self.closed and getattr(self, 'f', None):
            self.f.close()
        super().close()


def open_image(path):
    """romfs.bin (or any plain image) as a binary file, or a romfs-source.txt as a DumpImage."""
    return DumpImage(path) if is_source(path) else open(path, 'rb')


def image_size(path):
    return read_source(path)['image_size'] if is_source(path) else os.path.getsize(path)
