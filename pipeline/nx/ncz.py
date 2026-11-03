"""NCZ: an NCA compressed by nsz (the .ncz files of an NSZ / XCZ dump), read in place, never decompressed to disk.

Layout (nsz, nicoboss/nsz `NszDecompressor.py` / `BlockDecompressorReader.py`; all integers little-endian):
    [0, 0x4000)   the NCA's first 0x4000 bytes as dumped (its encrypted header: read_nca_header reads it as usual)
    'NCZSECTN'  u64 count, count x (u64 offset, u64 size, u64 crypto type, u64 pad, key[16], counter[16])
                the sections nsz decrypted, in NCA offsets; together with a filler from 0x4000 when the first one
                starts later, they cover [0x4000, NCA size) back to back
    either      'NCZBLOCK' u8 version, u8 type, u8 unused, u8 block size exponent, u32 blocks, u64 decompressed size,
                blocks x u32 compressed size, then the blocks (zstd, or stored when it did not shrink): random access
                or          one zstd stream (solid, nsz's default for NSZ): read forward; a read behind the stream starts
                it over
The stream is the NCA's bytes from 0x4000 decrypted, so NcaSection serves it without AES. Integrity stays where it was:
the readers verify the plaintext (ExeFS hash tables, all six IVFC levels, the pinned RomFS hash) as for an NCA.
"""
import struct

from .common import MiB, read_at, require

HEADER = 0x4000
TAIL = 4 * MiB   # the solid stream keeps its last bytes, so a read a little behind the stream needs no restart
SPILL_AFTER = 3  # restarts of a solid stream after which, given a spill folder, its plaintext goes to a temp file once


def is_ncz(name):
    return name.lower().endswith('.ncz')


class _Slice:
    """[offset, offset + size) of the container as a forward file for zstd (each read seeks: the handle is shared)."""

    def __init__(self, f, offset, size):
        self.f, self.offset, self.size, self.pos = f, offset, size, 0

    def read(self, n=-1):
        n = self.size - self.pos if n is None or n < 0 else min(n, self.size - self.pos)
        if n <= 0:
            return b''
        data = read_at(self.f, self.offset + self.pos, n)
        self.pos += n
        return data


class NczBody:
    """The plaintext of an NCZ's NCA from 0x4000 on; read(nca_offset, size) as NcaSection asks for it."""

    def __init__(self, f, offset, size, spill=None):
        """spill: a folder for a solid stream read out of order (an update's patch data, bktr.py): after SPILL_AFTER
        restarts its plaintext is written there once, read from that file, removed by close() or at exit"""
        self.f, self.spill, self.spilled = f, spill, None
        self.zstd = None
        require(size > HEADER + 16, 'Truncated NCZ')
        head = read_at(f, offset + HEADER, 16)
        require(head[:8] == b'NCZSECTN', 'Expected NCZSECTN (not an nsz-compressed NCA)')
        count = struct.unpack_from('<Q', head, 8)[0]
        require(1 <= count <= 64, 'Invalid NCZ section count')
        raw = read_at(f, offset + HEADER + 16, count * 0x40)
        self.sections = sorted((struct.unpack_from('<QQQ', raw, i * 0x40) for i in range(count)), key=lambda s: s[0])
        end = HEADER
        for start, length, _ in self.sections:
            require(start < 1 << 48 and length < 1 << 48, 'Invalid NCZ section')
            require(start >= end and length > 0, 'Overlapping NCZ sections')
            require(start == end or end == HEADER, 'NCZ sections leave a gap')
            end = start + length
        self.nca_size = end
        pos = HEADER + 16 + count * 0x40
        require(pos + 8 <= size, 'Truncated NCZ')
        if read_at(f, offset + pos, 8) == b'NCZBLOCK':
            hdr = read_at(f, offset + pos, 24)
            version, kind, _, exponent, blocks, total = struct.unpack_from('<BBBBIQ', hdr, 8)
            require(version == 2 and kind == 1 and 14 <= exponent <= 32, 'Unsupported NCZ block header')
            require(total == end - HEADER and blocks == (total + (1 << exponent) - 1) >> exponent, 'Invalid NCZ blocks')
            sizes = struct.unpack(f'<{blocks}I', read_at(f, offset + pos + 24, 4 * blocks))
            pos += 24 + 4 * blocks
            self.block = 1 << exponent
            self.blocks = []
            for csize in sizes:
                self.blocks.append((offset + pos, csize))
                pos += csize
            require(pos <= size, 'NCZ blocks out of bounds')
            self.cached = (-1, b'')
        else:
            self.block = None
            self.stream = (offset + pos, size - pos)
            self.reader = None
            self.tail, self.tail_pos = bytearray(), 0   # the last bytes the stream gave, from stream offset tail_pos
            self.restarts = 0

    def read(self, nca_offset, size):
        require(HEADER <= nca_offset and nca_offset + size <= self.nca_size, 'NCZ read out of bounds')
        rel = nca_offset - HEADER
        if self.zstd is None:   # (here, not at the top: only an NSZ / XCZ's data needs it; the venv has it, toolchain.py)
            import zstandard
            self.zstd = zstandard
            self.dctx = self._dctx()
        try:
            return self._blocks(rel, size) if self.block else self._solid(rel, size)
        except self.zstd.ZstdError as e:
            raise ValueError(f'NCZ data damaged ({e}): a truncated or damaged NSZ / XCZ') from None

    def _dctx(self):
        # (nsz --long: windows past zstd's default 128 MiB limit; memory goes only as far as the frame's window)
        return self.zstd.ZstdDecompressor(max_window_size=1 << 31)

    def _spill(self):
        import atexit
        import os
        import tempfile
        fd, path = tempfile.mkstemp(prefix='ncz-', suffix='.plain', dir=self.spill)
        f = os.fdopen(fd, 'w+b')
        self._spill_path = path
        atexit.register(self.close)
        r = self._dctx().stream_reader(_Slice(self.f, *self.stream), read_size=MiB, read_across_frames=True)
        left = self.nca_size - HEADER
        while left:
            data = r.read(min(64 * MiB, left))
            require(data, 'NCZ stream ended early (a truncated or damaged NSZ / XCZ)')
            f.write(data)
            left -= len(data)
        self.spilled, self.reader, self.tail = f, None, bytearray()

    def close(self):
        """the spilled plaintext goes (nothing else is held)"""
        import os
        if self.spilled is not None:
            self.spilled.close()
            self.spilled = None
            try:
                os.remove(self._spill_path)
            except OSError:
                pass

    def _blocks(self, rel, size):
        out = []
        while size > 0:
            index, skip = divmod(rel, self.block)
            data = self._block(index)
            piece = data[skip:skip + size]
            require(piece, 'NCZ block short')
            out.append(piece)
            rel += len(piece)
            size -= len(piece)
        return b''.join(out)

    def _block(self, index):
        if self.cached[0] == index:
            return self.cached[1]
        require(index < len(self.blocks), 'NCZ block out of range')
        total = self.nca_size - HEADER
        want = min(self.block, total - index * self.block)
        at, csize = self.blocks[index]
        raw = read_at(self.f, at, csize)
        if csize < want:
            data = self.dctx.decompress(raw, max_output_size=want)
        else:
            require(csize == want, 'NCZ stored block has the wrong size')
            data = raw
        require(len(data) == want, f'NCZ block {index} decompressed to the wrong size')
        self.cached = (index, data)
        return data

    def _solid(self, rel, size):
        if self.spilled is not None:
            self.spilled.seek(rel)
            return self.spilled.read(size)
        if rel < self.tail_pos:   # behind what the stream kept: start it over (the readers go forward; rare)
            self.reader, self.tail, self.tail_pos = None, bytearray(), 0
            self.restarts += 1
            if self.spill is not None and self.restarts >= SPILL_AFTER:
                self._spill()
                return self._solid(rel, size)
        if self.reader is None:
            self.reader = self._dctx().stream_reader(_Slice(self.f, *self.stream), read_size=MiB, read_across_frames=True)
        buf, pos = self.tail, self.tail_pos
        while pos + len(buf) < rel + size:
            data = self.reader.read(min(max(rel + size - pos - len(buf), MiB), 64 * MiB))
            require(data, 'NCZ stream ended early (a truncated or damaged NSZ / XCZ)')
            buf += data
            cut = min(rel, pos + len(buf) - TAIL) - pos   # keep what this read wants and the last TAIL bytes
            if cut > 0:
                del buf[:cut]
                pos += cut
        data = bytes(buf[rel - pos:rel - pos + size])
        if len(buf) > TAIL:   # (memory: TAIL bytes stay, not a whole 512 MiB read)
            pos += len(buf) - TAIL
            del buf[:len(buf) - TAIL]
        self.tail, self.tail_pos = buf, pos
        return data
