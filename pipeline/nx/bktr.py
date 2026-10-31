"""Game updates: the patched RomFS of an update NCA (BKTR), read as one virtual section over the base game's RomFS.

An update's RomFS section (FS header encryption type 4, AesCtrEx) holds only what changed. Its patch info (FS header
0x100..0x140) names two bucket trees stored at the end of the section, decrypted with the section's plain AES-CTR:
  - the indirect table: virtual ranges of the patched RomFS, each read from the base RomFS section (storage 0) or from
    the update's patch data (storage 1) at a physical offset;
  - the AES-CTR-Ex table: physical ranges of the patch data, each with its own counter generation (the IV's upper
    half is secure value | generation, the lower the absolute NCA offset / 16), or not encrypted at all.
`BktrSection` has NcaSection's interface (`fs`, `size`, `kind`, `read`), so romfs.extract_image verifies every IVFC
level of the merged RomFS exactly as for a base game: a wrong merge cannot produce an image.

Layout as in LibHac / Atmosphere (fssystem BucketTree, IndirectStorage, AesCtrCounterExtendedStorage); written for
this port from those definitions. Keys stay in memory; errors never print them.
"""
import bisect
import struct

from cryptography.hazmat.primitives.ciphers import modes

from .common import decrypt, read_at, require

CRYPT_CTR_EX = 4
NODE_SIZE = 0x4000
INDIRECT_ENTRY = 0x14       # u64 virtual offset, u64 physical offset, s32 storage index
AESCTREX_ENTRY = 0x10       # u64 offset, u8 encryption (0 encrypted, 1 not), u8[3], s32 generation


def bucket_tree(meta: bytes, header: bytes, entry_size: int, total=None):
    """Parse one bucket tree: `meta` is its table (node storage, then entry sets), `header` the 16-byte BKTR header
    from the FS header. Returns (entries as raw bytes in order, end offset)."""
    magic, version, count, _ = struct.unpack('<4sIii', header)
    require(magic == b'BKTR' and version == 1, 'Unsupported update patch table (BKTR header)')
    require(count >= 0, 'Invalid update patch table entry count')
    if count == 0:
        return [], 0
    per_set = (NODE_SIZE - 16) // entry_size
    sets = -(-count // per_set)
    offsets_per_node = (NODE_SIZE - 16) // 8
    require(sets <= offsets_per_node, 'Update patch table needs a second node level (unsupported)')
    require(len(meta) >= NODE_SIZE * (1 + sets), 'Truncated update patch table')
    index, n, end = struct.unpack_from('<iiq', meta, 0)
    require(index == 0 and n == sets, 'Update patch table: L1 node does not match its entry count')
    starts = struct.unpack_from(f'<{sets}q', meta, 16)
    entries = []
    for i in range(sets):
        base = NODE_SIZE * (1 + i)
        si, sn, send = struct.unpack_from('<iiq', meta, base)
        require(si == i and 0 < sn <= per_set and sn == min(per_set, count - i * per_set),
                f'Update patch table: entry set {i} header')
        for k in range(sn):
            entries.append(meta[base + 16 + k * entry_size:base + 16 + (k + 1) * entry_size])
        require(struct.unpack_from('<q', entries[i * per_set])[0] == starts[i],
                f'Update patch table: entry set {i} starts elsewhere than the L1 node says')
        require(send == (starts[i + 1] if i + 1 < sets else end),
                f'Update patch table: entry set {i} ends elsewhere than the next one starts')
    offs = [struct.unpack_from('<q', e)[0] for e in entries]
    require(all(a < b for a, b in zip(offs, offs[1:])) and offs[0] >= 0 and offs[-1] < end,
            'Update patch table: offsets not increasing')
    if total is not None:
        require(end == total, 'Update patch table: end offset does not match its storage')
    return entries, end


class BktrSection:
    """The patched RomFS of an update: NcaSection's interface over base RomFS + update patch data."""

    def __init__(self, f, nca_offset, hdr, index, ctr_key, base):
        start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * index)
        require(start != 0, f'Update NCA section {index} is empty')
        self.fs = hdr[0x400 + 512 * index:0x600 + 512 * index]
        import hashlib
        require(hashlib.sha256(self.fs).digest() == hdr[0x280 + 32 * index:0x2A0 + 32 * index],
                'Update FS header hash mismatch')
        require(self.fs[4] == CRYPT_CTR_EX, f'Update NCA section {index} is not a BKTR (AesCtrEx) section '
                                             f'(encryption type {self.fs[4]})')
        require(self.fs[0x178:0x1A0] == bytes(0x28) and self.fs[0x148:0x178] == bytes(0x30),
                'Update NCA section uses sparse or compressed storage (unsupported)')
        self.f, self.index, self.key, self.base_section = f, index, ctr_key, base
        self.kind = (self.fs[2], self.fs[3])
        self.start, self.raw_size, self.nca = start * 512, (end - start) * 512, nca_offset
        self.upper = self.fs[0x140:0x148][::-1]                  # secure value (BE) | generation (BE)
        ind_off, ind_size, ind_hdr = struct.unpack_from('<QQ', self.fs, 0x100) + (self.fs[0x110:0x120],)
        ex_off, ex_size, ex_hdr = struct.unpack_from('<QQ', self.fs, 0x120) + (self.fs[0x130:0x140],)
        require(0 < ind_off and ind_off + ind_size <= ex_off and ex_off + ex_size <= self.raw_size,
                'Update patch tables out of bounds')
        self.data_size = ex_off                                  # the patch data: [0, AesCtrEx table)
        ind, self.size = bucket_tree(self._ctr(ind_off, ind_size), ind_hdr, INDIRECT_ENTRY)
        self.ind = [struct.unpack('<qqi', e) for e in ind]
        self.ind_virt = [v for v, _, _ in self.ind]
        require(self.ind and self.ind_virt[0] == 0, 'Update indirect table does not start at 0')
        require(all(s in (0, 1) for _, _, s in self.ind), 'Update indirect table: unknown storage index')
        ex, ex_end = bucket_tree(self._ctr(ex_off, ex_size), ex_hdr, AESCTREX_ENTRY)
        self.ex = [(o, e[8], struct.unpack_from('<i', e, 12)[0]) for e in ex for o in [struct.unpack_from('<q', e)[0]]]
        self.ex_off = [o for o, _, _ in self.ex]
        require(self.ex and self.ex_off[0] == 0 and ex_end <= self.data_size, 'Update AesCtrEx table range')
        require(all(enc in (0, 1) for _, enc, _ in self.ex), 'Update AesCtrEx table: unknown encryption value')
        self.ex_end = ex_end

    # ------------------------------------------------------------------- raw reads in the update section
    def _iv(self, upper, absolute):
        return upper + (absolute // 16).to_bytes(8, 'big')

    def _ctr(self, rel, size):
        """Plain section AES-CTR (the patch tables)."""
        absolute = self.start + rel
        aligned = absolute & ~15
        data = read_at(self.f, self.nca + aligned, size + absolute - aligned)
        return decrypt(self.key, modes.CTR(self._iv(self.upper, aligned)), data)[absolute - aligned:]

    def _patch(self, rel, size):
        """Patch data [rel, rel + size): AesCtrEx, one counter generation per table range."""
        require(0 <= rel and rel + size <= self.ex_end, 'Update patch data read out of bounds')
        out = []
        while size:
            i = bisect.bisect_right(self.ex_off, rel) - 1
            off, enc, gen = self.ex[i]
            stop = self.ex_off[i + 1] if i + 1 < len(self.ex) else self.ex_end
            n = min(size, stop - rel)
            if enc == 1:
                out.append(read_at(self.f, self.nca + self.start + rel, n))
            else:
                absolute = self.start + rel
                aligned = absolute & ~15
                upper = self.upper[:4] + struct.pack('>I', gen & 0xFFFFFFFF)
                data = read_at(self.f, self.nca + aligned, n + absolute - aligned)
                out.append(decrypt(self.key, modes.CTR(self._iv(upper, aligned)), data)[absolute - aligned:])
            rel += n
            size -= n
        return b''.join(out)

    # ------------------------------------------------------------------- NcaSection interface
    def read(self, relative, size):
        require(0 <= relative and relative + size <= self.size, 'Patched RomFS read out of bounds')
        out = []
        while size:
            i = bisect.bisect_right(self.ind_virt, relative) - 1
            virt, phys, storage = self.ind[i]
            stop = self.ind_virt[i + 1] if i + 1 < len(self.ind) else self.size
            n = min(size, stop - relative)
            p = phys + relative - virt
            out.append(self.base_section.read(p, n) if storage == 0 else self._patch(p, n))
            relative += n
            size -= n
        return b''.join(out)
