"""A minimal BYML (v2..v7, little-endian "YB") walker for in-place edits: every scalar with its path and the byte
offset of its 4-byte value, so an overlay can check the shipped value and pack a new one at the same place (the file's
size and layout never change). Containers and 8-byte values (int64 / uint64 / double) are listed with their offsets
too; strings resolve through the string table."""
import struct

STR, ARRAY, DICT, BOOL, INT, FLOAT, UINT, INT64, UINT64, DOUBLE, NULL = (
    0xA0, 0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xFF)
SCALAR_FMT = {BOOL: '<I', INT: '<i', FLOAT: '<f', UINT: '<I'}
WIDE_FMT = {INT64: '<q', UINT64: '<Q', DOUBLE: '<d'}


def _strings(data, off):
    if not off:
        return []
    assert data[off] == 0xC2, 'not a string table'
    n = int.from_bytes(data[off + 1:off + 4], 'little')
    out = []
    for i in range(n):
        s = struct.unpack_from('<I', data, off + 4 + 4 * i)[0] + off
        out.append(data[s:data.index(b'\0', s)].decode('utf-8'))
    return out


def walk(data):
    """Yields (path tuple, node type, value, offset of the value word) for every leaf; path elements are dict keys
    (str) or array indices (int)."""
    data = bytes(data)
    assert data[:2] == b'YB', 'not a little-endian BYML'
    keys_off, strs_off, root_off = struct.unpack_from('<III', data, 4)
    keys, strs = _strings(data, keys_off), _strings(data, strs_off)

    def value(t, word_off, path):
        raw = struct.unpack_from('<I', data, word_off)[0]
        if t in (ARRAY, DICT):
            yield from node(raw, path)
        elif t == STR:
            yield path, t, strs[raw], word_off
        elif t in SCALAR_FMT:
            yield path, t, struct.unpack_from(SCALAR_FMT[t], data, word_off)[0], word_off
        elif t in WIDE_FMT:
            yield path, t, struct.unpack_from(WIDE_FMT[t], data, raw)[0], raw
        else:
            yield path, t, None, word_off

    def node(off, path):
        t = data[off]
        n = int.from_bytes(data[off + 1:off + 4], 'little')
        if t == DICT:
            for i in range(n):
                e = off + 4 + 8 * i
                k = int.from_bytes(data[e:e + 3], 'little')
                yield from value(data[e + 3], e + 4, path + (keys[k],))
        elif t == ARRAY:
            vals = off + 4 + ((n + 3) & ~3)
            for i in range(n):
                yield from value(data[off + 4 + i], vals + 4 * i, path + (i,))
        else:
            raise ValueError(f'unexpected node {t:#x} at {off:#x}')

    if root_off:
        yield from node(root_off, ())


def find(data, path):
    """(type, value, offset) of the leaf at `path` (a tuple), or None."""
    for p, t, v, o in walk(data):
        if p == tuple(path):
            return t, v, o
    return None
