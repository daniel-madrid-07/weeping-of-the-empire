"""mals/: the message archives, one set for every edit to the game's message texts (MSBT).

    python -m games.totk.overlays.mals --romfs <work>/extracted/v100 --out <work>/overlays

All of a language's message files are members of one archive, Mals/<lang>.Product.100.sarc.zs, so two sets that each
change some messages would replace the same 14 files, and engine_dir refuses two sets giving one path. Instead an edit
to messages is a *patcher*: a module in PATCHERS with

    mals_patches(lang) -> {MSBT member path: function(msbt bytes) -> msbt bytes}

and this set applies every patcher's functions to each archive (several to one MSBT: in PATCHERS order, each on the
previous one's output) and writes each archive once. Every SARC member no patcher names stays byte-identical (checked);
what a patcher keeps of its own MSBT is its own check. An archive no patcher touches is not written.
Patchers: options_menu (LayoutMsg/SystemOption_00.msbt: the PC rows of the Options screen).
"""
import argparse
import importlib
import json
import os
import struct
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from . import RomFS, Zstd, sarc_members, _write_set

PATCHERS = ('games.totk.overlays.options_menu',)

LANGS = ('CNzh', 'EUde', 'EUen', 'EUes', 'EUfr', 'EUit', 'EUnl', 'EUru', 'JPja', 'KRko', 'TWzh', 'USen', 'USes', 'USfr')
# the language of each archive, for texts written per language (a patcher's 'en' is the fallback)
LANG_KEY = {'CNzh': 'zhs', 'TWzh': 'zht', 'JPja': 'ja', 'KRko': 'ko', 'EUru': 'ru', 'EUnl': 'nl', 'EUit': 'it',
            'EUde': 'de', 'EUfr': 'fr', 'USfr': 'fr', 'EUes': 'es', 'USes': 'es', 'EUen': 'en', 'USen': 'en'}


def archive(lang):
    return f'Mals/{lang}.Product.100.sarc.zs'

# ------------------------------------------------------------------------------------------------------------ MSBT


def label_hash(label, buckets):
    h = 0
    for ch in label.encode('ascii'):
        h = (h * 0x492 + ch) & 0xffffffff
    return h % buckets


def msbt_parse(data):
    """(header bytes, bucket count, [(label, raw UTF-16 bytes incl. the terminator)]) of an LBL1 + TXT2 MSBT; any other
    section is refused (SystemOption_00 has exactly these two)."""
    assert data[:8] == b'MsgStdBn' and data[8:10] == b'\xff\xfe', 'not a little-endian MSBT'
    assert data[0x0c] == 1, 'not UTF-16'
    count = struct.unpack_from('<H', data, 0x0e)[0]
    pos, labels, texts, buckets = 0x20, {}, None, None
    for _ in range(count):
        magic, size = bytes(data[pos:pos + 4]), struct.unpack_from('<I', data, pos + 4)[0]
        body = pos + 16
        if magic == b'LBL1':
            buckets = struct.unpack_from('<I', data, body)[0]
            for i in range(buckets):
                n, off = struct.unpack_from('<2I', data, body + 4 + 8 * i)
                o = body + off
                for _ in range(n):
                    ln = data[o]
                    labels[struct.unpack_from('<I', data, o + 1 + ln)[0]] = bytes(data[o + 1:o + 1 + ln]).decode('ascii')
                    o += 1 + ln + 4
        elif magic == b'TXT2':
            n = struct.unpack_from('<I', data, body)[0]
            offs = [struct.unpack_from('<I', data, body + 4 + 4 * i)[0] for i in range(n)] + [size]
            texts = [bytes(data[body + offs[i]:body + offs[i + 1]]) for i in range(n)]
        else:
            raise ValueError(f'unexpected MSBT section {magic}')
        pos = (body + size + 15) & ~15
    assert texts is not None and buckets and sorted(labels) == list(range(len(texts))), 'labels and texts disagree'
    return bytes(data[:0x20]), buckets, [(labels[i], texts[i]) for i in range(len(texts))]


def _section(magic, body):
    pad = (-len(body)) % 16
    return magic + struct.pack('<I', len(body)) + b'\0' * 8 + body + b'\xab' * pad


def msbt_build(header, buckets, msgs):
    table = [[] for _ in range(buckets)]
    for i, (label, _) in enumerate(msgs):
        table[label_hash(label, buckets)].append((label, i))
    head, names = bytearray(struct.pack('<I', buckets)), bytearray()
    base = 4 + 8 * buckets
    for entries in table:
        head += struct.pack('<2I', len(entries), base + len(names))
        for label, i in entries:
            b = label.encode('ascii')
            names += bytes([len(b)]) + b + struct.pack('<I', i)
    lbl1 = bytes(head + names)
    offs, blob = [], bytearray()
    for _, raw in msgs:
        offs.append(4 + 4 * len(msgs) + len(blob))
        blob += raw
    txt2 = struct.pack('<I', len(msgs)) + b''.join(struct.pack('<I', o) for o in offs) + bytes(blob)
    out = bytearray(header) + _section(b'LBL1', lbl1) + _section(b'TXT2', txt2)
    struct.pack_into('<H', out, 0x0e, 2)
    struct.pack_into('<I', out, 0x12, len(out))
    return bytes(out)


def msbt_add(data, new, name='MSBT'):
    """data with the (label, text) messages appended; a label the file already has is refused."""
    header, buckets, msgs = msbt_parse(data)
    have = {label for label, _ in msgs}
    for label, t in new:
        if label in have:
            raise ValueError(f'{name}: label {label} already there (shipped, or another patcher added it)')
        msgs.append((label, t.encode('utf-16-le') + b'\0\0'))
    return msbt_build(header, buckets, msgs)

# ------------------------------------------------------------------------------------------------------------ SARC


def sarc_replace(data, members):
    """The SARC with some members' bytes replaced ({name: bytes}): header, SFAT order and SFNT kept, the data re-laid in
    the original order with each member at the alignment its original offset had (a power of two, 8 .. 0x2000)."""
    data = bytes(data)
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    count = struct.unpack_from('<H', data, header + 6)[0]
    nodes = header + 12
    spans = [struct.unpack_from('<2I', data, nodes + 16 * i + 8) for i in range(count)]
    where = sarc_members(data)
    target = {}
    for name, body in members.items():
        lo, hi = where[name]
        hit = [i for i, (a, b) in enumerate(spans) if (start + a, start + b) == (lo, hi)]
        assert len(hit) == 1, name
        target[hit[0]] = body
    out = bytearray(data[:start])
    blob = bytearray()
    for i in sorted(range(count), key=lambda i: spans[i][0]):
        a, b = spans[i]
        align = min(max(a & -a, 8), 0x2000) if a else 8
        blob += b'\0' * ((-len(blob)) % align)
        body = target.get(i, data[start + a:start + b])
        struct.pack_into('<2I', out, nodes + 16 * i + 8, len(blob), len(blob) + len(body))
        blob += body
    out += blob
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)

# ------------------------------------------------------------------------------------------------------------ set


def patches_of(lang, patchers=PATCHERS):
    """{MSBT path: [(patcher, function)]} for one language, in PATCHERS order."""
    out = {}
    for mod in patchers:
        for msbt, fn in importlib.import_module(mod).mals_patches(lang).items():
            out.setdefault(msbt, []).append((mod.rsplit('.', 1)[-1], fn))
    return out


def one_language(lang, romfs, zstd, patchers):
    """(the language's archive entry, {MSBT: patchers}) or None when nothing edits it"""
    patches = patches_of(lang, patchers)
    if not patches:
        return None
    path = archive(lang)
    raw = romfs.read(path)
    sarc, dict_id = zstd.decompress(raw)
    sarc = bytes(sarc)
    where = sarc_members(sarc)
    new, edited = {}, {}
    for msbt, fns in patches.items():
        lo, hi = where[msbt]
        data = sarc[lo:hi]
        for _, fn in fns:
            data = fn(data)
        new[msbt] = data
        edited[msbt] = sorted({p for p, _ in fns})
    out = sarc_replace(sarc, new)
    after = sarc_members(out)
    assert sorted(after) == sorted(where)
    for name, (a, b) in where.items():
        a2, b2 = after[name]
        assert out[a2:b2] == (new[name] if name in new else sarc[a:b]), name
    return (path, zstd.compress(out, dict_id), raw), edited


def build_mals(romfs, zstd, out_root, patchers=PATCHERS):
    # the languages at once: most of the time is zstd at level 19, which runs outside the GIL (2026-10-30); the
    # entries and the report keep LANGS order, the bytes are the same
    for mod in patchers:
        importlib.import_module(mod)   # (imported once here, not by 14 threads at once)
    with ThreadPoolExecutor(min(len(LANGS), os.cpu_count() or 4)) as ex:
        done = list(ex.map(lambda lang: one_language(lang, romfs, zstd, patchers), LANGS))
    entries, edited = [], {}
    for r in done:
        if r:
            entries.append(r[0])
            for msbt, who in r[1].items():
                edited.setdefault(msbt, who)
    out_dir = Path(out_root) / 'mals'
    _write_set(out_dir, entries)
    return dict(set='mals', status='pass', files=len(entries), msbt=edited, out=str(out_dir))


def main(argv=None):
    p = argparse.ArgumentParser(prog='python -m games.totk.overlays.mals', description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path, help='<work>/overlays')
    a = p.parse_args(argv)
    romfs = RomFS(a.romfs)
    print(json.dumps(build_mals(romfs, Zstd(romfs), a.out)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
