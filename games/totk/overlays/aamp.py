"""A minimal AAMP (parameter archive v2, little-endian) walker for in-place edits: every parameter with the hashes of
the lists and object above it and the byte offset of its value, so an overlay can check the shipped value and pack a
new one at the same place (the file's size and layout never change). Names are CRC-32 hashes; `name_hash` makes them.

Values are deduplicated: two parameters with the same value may share one data slot, so before an in-place edit check
`shared(data, offset)`: editing a shared slot changes every parameter that points at it."""
import struct
import zlib

BOOL, F32, INT, VEC2, VEC3, VEC4, COLOR, STR32, STR64 = 0, 1, 2, 3, 4, 5, 6, 7, 8
STR256, U32, STR_REF = 15, 17, 20
_FMT = {BOOL: '<I', F32: '<f', INT: '<i', U32: '<I', VEC2: '<2f', VEC3: '<3f', VEC4: '<4f', COLOR: '<4f'}
_STRINGS = (STR32, STR64, STR256, STR_REF)


def name_hash(name):
    return zlib.crc32(name.encode())


def walk(data):
    """Yields (path tuple of list / object hashes, parameter hash, type, value, offset of the value) for every
    parameter; the path ends with the object's hash."""
    data = bytes(data)
    if data[:4] != b'AAMP' or struct.unpack_from('<I', data, 4)[0] != 2:
        raise ValueError('not an AAMP v2 parameter archive')
    if not struct.unpack_from('<I', data, 8)[0] & 1:
        raise ValueError('big-endian AAMP')

    def plist(off, path):
        h, lists, objs = struct.unpack_from('<III', data, off)
        here = path + (h,)
        for i in range(objs >> 16):
            yield from pobj(off + (objs & 0xffff) * 4 + 8 * i, here)
        for i in range(lists >> 16):
            yield from plist(off + (lists & 0xffff) * 4 + 12 * i, here)

    def pobj(off, path):
        h, params = struct.unpack_from('<II', data, off)
        here = path + (h,)
        base = off + (params & 0xffff) * 4
        for i in range(params >> 16):
            po = base + 8 * i
            ph, word = struct.unpack_from('<II', data, po)
            t, vo = word >> 24, po + (word & 0xffffff) * 4
            if t in _FMT:
                v = struct.unpack_from(_FMT[t], data, vo)
                v = v[0] if len(v) == 1 else v
            elif t in _STRINGS:
                v = data[vo:data.index(b'\0', vo)].decode('utf-8', 'replace')
            else:
                v = None
            yield here, ph, t, v, vo

    yield from plist(0x30 + struct.unpack_from('<I', data, 0x14)[0], ())


def shared(data, offset):
    """How many parameters point at the value slot `offset`."""
    return sum(1 for *_, vo in walk(data) if vo == offset)
