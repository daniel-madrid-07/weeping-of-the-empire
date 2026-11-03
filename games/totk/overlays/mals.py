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

Brazilian Portuguese (lang-ptbr, v0.1.9): 1.0.0 ships no Portuguese; 1.4.x adds Mals/USpt.Product.14x. With the
player's own update (--update <work>/extracted/v143, what Setup unpacked from the update they gave it; nothing of the
game is shipped by us) the set also writes Mals/USpt.Product.100.sarc.zs: USen 1.0.0's archive, every member and
label as 1.0.0 has them, each message's text taken from the update's USpt by its label. A message the update lacks, or
one using a control tag no 1.0.0 archive uses, keeps its English text (counted and listed in USpt.fallback.tsv); so
does StaticMsg/GuideKeyIcon.msbt (the glyph bank finds it in memory by its USen bytes). The patchers then run on it as
on USen ('pt' texts, else English). The game keeps answering en-US (its voices and sounds have no Portuguese in any
version); hle_fs.cpp serves this archive in place of the one it opens when game.language is Português (Brasil).
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
            'EUde': 'de', 'EUfr': 'fr', 'USfr': 'fr', 'EUes': 'es', 'USes': 'es', 'EUen': 'en', 'USen': 'en',
            'USpt': 'pt'}
PT = 'USpt'                      # (written only from the player's update, below; not one of LANGS)
PT_BASE = 'USen'                 # the archive it is laid out as, and the English of every fallback
PT_SOURCES = ('143', '142', '141', '140', '130', '121', '120', '112', '111', '110')   # the update's archive version
GUIDE_KEY_ICON = 'StaticMsg/GuideKeyIcon.msbt'   # (games/totk/glyphs/bank.py: matched in memory by its USen bytes)
# The letters Portuguese needs past ASCII, checked in the game's text fonts
PT_LETTERS = 'ãõçêáéíóúâôàÃÕÇÊÁÉÍÓÚÂÔÀ'
FONT_ARCHIVE = 'Font/Font.Nin_NX_NVN.bfarc.zs'


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

# ------------------------------------------------------------------------------------------------------------ pt-BR


def msbt_tags(raw):
    """{(group, type)} of the control tags in one TXT2 text (UTF-16: 0x0e group type size params, 0x0f group type)."""
    out, n, i = set(), len(raw) // 2, 0
    if b'\x0e\x00' not in raw and b'\x0f\x00' not in raw:
        return out
    u = struct.unpack(f'<{n}H', raw[:2 * n])
    while i < n:
        c = u[i]
        if c == 0x0e and i + 3 < n:
            out.add((u[i + 1], u[i + 2]))
            i += 4 + u[i + 3] // 2
        elif c == 0x0f and i + 2 < n:
            out.add((u[i + 1], u[i + 2]))
            i += 3
        else:
            i += 1
    return out


def sarc_msbts(sarc):
    return {name: sarc[lo:hi] for name, (lo, hi) in sarc_members(sarc).items() if name.endswith('.msbt')}


def archive_tags(sarc):
    """Every control tag the texts of one decompressed archive use."""
    out = set()
    for data in sarc_msbts(sarc).values():
        for _, raw in msbt_parse(data)[2]:
            out |= msbt_tags(raw)
    return out


def pt_member(name, en, pt, allowed):
    """(MSBT bytes, {'pt': n, 'missing': [labels], 'tags': [labels]}): en's labels, order and hash table with each text
    from pt (same label) where pt has it with no tag outside `allowed`, else en's. en itself when nothing changes."""
    header, buckets, msgs = msbt_parse(en)
    theirs = dict(msbt_parse(pt)[2]) if pt is not None else {}
    stats, out = dict(pt=0, missing=[], tags=[]), []
    for label, raw in msgs:
        t = theirs.get(label)
        if t is None:
            stats['missing'].append(label)
        elif not msbt_tags(t) <= allowed:
            stats['tags'].append(label)
        else:
            stats['pt'] += 1
            raw = t
        out.append((label, raw))
    if all(a[1] == b[1] for a, b in zip(out, msgs)):
        return en, stats
    return msbt_build(header, buckets, out), stats


def update_archive(update):
    """(RomFS path, raw bytes) of the update's Portuguese archive (1.4.3 has Product.142's)."""
    for v in PT_SOURCES:
        path = f'Mals/{PT}.Product.{v}.sarc.zs'
        try:
            return path, update.read(path)
        except (KeyError, FileNotFoundError):
            continue
    raise FileNotFoundError(f'the update has no Mals/{PT}.Product.*.sarc.zs (tried {", ".join(PT_SOURCES)})')


def pt_sarc(en_sarc, pt_sarc_, allowed):
    """(the merged archive, per-member stats): en_sarc's members, each MSBT's texts from pt_sarc_ (pt_member);
    GuideKeyIcon stays English byte for byte."""
    en_m, pt_m = sarc_msbts(en_sarc), sarc_msbts(pt_sarc_)
    new, stats = {}, {}
    for name, data in en_m.items():
        if name == GUIDE_KEY_ICON:
            stats[name] = dict(pt=0, missing=[], tags=[], kept=True)
            continue
        body, st = pt_member(name, data, pt_m.get(name), allowed)
        stats[name] = st
        if body != data:
            new[name] = body
    return sarc_replace(en_sarc, new), stats

# ------------------------------------------------------------------------------------------------------------ fonts


def bfotf_otf(data):
    """The OpenType font of a .bfotf / .bfttf (the words XORed with a key: the first is 0x7F9A0218 ^ key, the second
    the size ^ key; games/totk/glyphs/bank.py decrypt_bfotf, without numpy)."""
    key = struct.unpack_from('>I', data, 0)[0] ^ 0x7F9A0218
    size = struct.unpack_from('>I', data, 4)[0] ^ key
    body = bytes(data[8:8 + (len(data) - 8) // 4 * 4])
    mask = struct.pack('>I', key) * (len(body) // 4)
    return (int.from_bytes(body, 'big') ^ int.from_bytes(mask, 'big')).to_bytes(len(body), 'big')[:size]


def otf_chars(otf):
    """The code points an OpenType font maps to a glyph (cmap formats 4 and 12, every subtable)."""
    tables = {bytes(otf[12 + 16 * i:16 + 16 * i]): struct.unpack_from('>I', otf, 20 + 16 * i)[0]
              for i in range(struct.unpack_from('>H', otf, 4)[0])}
    base, out = tables[b'cmap'], set()
    for i in range(struct.unpack_from('>H', otf, base + 2)[0]):
        st = base + struct.unpack_from('>I', otf, base + 8 + 8 * i)[0]
        fmt = struct.unpack_from('>H', otf, st)[0]
        if fmt == 4:
            seg = struct.unpack_from('>H', otf, st + 6)[0] // 2
            ends = struct.unpack_from(f'>{seg}H', otf, st + 14)
            starts = struct.unpack_from(f'>{seg}H', otf, st + 16 + 2 * seg)
            deltas = struct.unpack_from(f'>{seg}h', otf, st + 16 + 4 * seg)
            ro_at = st + 16 + 6 * seg
            ros = struct.unpack_from(f'>{seg}H', otf, ro_at)
            for k in range(seg):
                for c in range(starts[k], ends[k] + 1):
                    if c == 0xffff:
                        continue
                    if ros[k] == 0:
                        g = (c + deltas[k]) & 0xffff
                    else:
                        g = struct.unpack_from('>H', otf, ro_at + 2 * k + ros[k] + 2 * (c - starts[k]))[0]
                        g = (g + deltas[k]) & 0xffff if g else 0
                    if g:
                        out.add(c)
        elif fmt == 12:
            for k in range(struct.unpack_from('>I', otf, st + 12)[0]):
                lo, hi, g = struct.unpack_from('>3I', otf, st + 16 + 12 * k)
                out.update(c for c in range(lo, hi + 1) if g + c - lo)
    return out


def font_check(fonts_sarc, used, english):
    """The game's text fonts (those with every ASCII letter and digit) against Portuguese: {font: the PT_LETTERS it
    lacks}, and the characters of the served text that no text font has and the English text does not use either."""
    alnum = {ord(c) for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'}
    fonts = {}
    for name, (lo, hi) in sarc_members(fonts_sarc).items():
        if name.endswith(('.bfotf', '.bfttf')):
            chars = otf_chars(bfotf_otf(fonts_sarc[lo:hi]))
            if alnum <= chars:
                fonts[name] = chars
    every = set().union(*fonts.values()) if fonts else set()
    return dict(fonts={n: ''.join(c for c in PT_LETTERS if ord(c) not in cs) for n, cs in sorted(fonts.items())},
                in_no_font=[f'U+{c:04X}' for c in sorted(used - every - english)])


def text_chars(sarc):
    """The code points of every text in a decompressed archive, control tags left out."""
    out = set()
    for data in sarc_msbts(sarc).values():
        for _, raw in msbt_parse(data)[2]:
            n = len(raw) // 2
            u, i = struct.unpack(f'<{n}H', raw[:2 * n]), 0
            while i < n:
                c = u[i]
                if c == 0x0e and i + 3 < n:
                    i += 4 + u[i + 3] // 2
                    continue
                if c == 0x0f:
                    i += 3
                    continue
                if c >= 0x20:
                    out.add(c)
                i += 1
    return out

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
    out, edited = apply_patches(bytes(sarc), patches)
    return (path, zstd.compress(out, dict_id), raw), edited


def apply_patches(sarc, patches):
    """(the archive with every patcher's functions applied, {MSBT: patchers}); every other member checked unchanged"""
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
    return out, edited


def portuguese(romfs, zstd, update, patchers):
    """(the entry of Mals/USpt.Product.100.sarc.zs, {MSBT: patchers}, report, fallback lines) from the player's
    update RomFS (`update`): pt_sarc over USen 1.0.0, the patchers, the font check. The manifest's original is USen's
    (engine_dir.SERVED_AS: the file the game opens in its place)."""
    source, raw_pt = update_archive(update)
    pt, _ = Zstd(update).decompress(raw_pt)
    raw_en = romfs.read(archive(PT_BASE))
    en, dict_id = zstd.decompress(raw_en)
    en, pt = bytes(en), bytes(pt)
    # the tags 1.0.0 knows: every one its own 14 archives use (1.4.3's USpt has one more, in one message)
    allowed = set()
    for lang in LANGS:
        allowed |= archive_tags(en if lang == PT_BASE else bytes(zstd.decompress(romfs.read(archive(lang)))[0]))
    merged, stats = pt_sarc(en, pt, allowed)
    out, edited = apply_patches(merged, patches_of(PT, patchers))
    fonts, _ = zstd.decompress(romfs.read(FONT_ARCHIVE))
    glyphs = font_check(bytes(fonts), text_chars(out), text_chars(en))
    lines = [f'{m}\t{label}\t{why}' for m, st in sorted(stats.items())
             for why in ('missing', 'tags') for label in st[why]]
    total = sum(st['pt'] + len(st['missing']) + len(st['tags']) for st in stats.values())
    report = dict(source=source, members=len(stats), messages=total,
                  portuguese=sum(st['pt'] for st in stats.values()),
                  english_missing=sum(len(st['missing']) for st in stats.values()),
                  english_tags=sum(len(st['tags']) for st in stats.values()),
                  english_kept=[m for m, st in stats.items() if st.get('kept')], glyphs=glyphs)
    return (archive(PT), zstd.compress(out, dict_id), raw_en), edited, report, lines


def build_mals(romfs, zstd, out_root, patchers=PATCHERS, update=None):
    # the languages at once: most of the time is zstd at level 19, which runs outside the GIL (2026-10-30); the
    # entries and the report keep LANGS order, the bytes are the same
    for mod in patchers:
        importlib.import_module(mod)   # (imported once here, not by 14 threads at once)
    with ThreadPoolExecutor(min(len(LANGS) + 1, os.cpu_count() or 4)) as ex:
        pt = ex.submit(portuguese, romfs, zstd, update, patchers) if update is not None else None
        done = list(ex.map(lambda lang: one_language(lang, romfs, zstd, patchers), LANGS))
        problem = None
        try:   # (Portuguese is extra: an update it cannot read leaves the other 14 archives as they are)
            pt = pt.result() if pt else None
        except (ValueError, KeyError, OSError, AssertionError, struct.error) as e:
            pt, problem = None, f'{type(e).__name__}: {e}'
    entries, edited = [], {}
    for r in done + ([pt[:2]] if pt else []):
        if r:
            entries.append(r[0])
            for msbt, who in r[1].items():
                edited.setdefault(msbt, who)
    out_dir = Path(out_root) / 'mals'
    _write_set(out_dir, entries)
    report = dict(set='mals', status='pass', files=len(entries), msbt=edited, out=str(out_dir))
    fallback = out_dir / f'{PT}.fallback.tsv'   # (not in the manifest: engine_dir copies listed files only)
    if pt:
        report[PT] = dict(status='pass', **pt[2])
        with open(fallback, 'w', newline='\n', encoding='utf-8') as f:
            f.write('# member\tlabel\twhy English (missing: not in the update; tags: a control tag 1.0.0 does not use)\n')
            f.write(''.join(ln + '\n' for ln in pt[3]))
    else:
        if problem:
            report[PT] = dict(status='skipped', problem=problem)
        if fallback.exists():
            fallback.unlink()
    return report


def main(argv=None):
    p = argparse.ArgumentParser(prog='python -m games.totk.overlays.mals', description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path, help='<work>/overlays')
    p.add_argument('--update', type=Path, help='the player\'s 1.4.x update RomFS (<work>/extracted/v143): also '
                   'writes the Brazilian Portuguese archive from its USpt')
    a = p.parse_args(argv)
    romfs = RomFS(a.romfs)
    update, problem = None, None
    if a.update:
        try:
            update = RomFS(a.update)
        except (ValueError, OSError) as e:   # (half unpacked, or another tool's folder: no Portuguese, the rest built)
            problem = f'{type(e).__name__}: {e}'
    report = build_mals(romfs, Zstd(romfs), a.out, update=update)
    if problem:
        report[PT] = dict(status='skipped', problem=problem)
    print(json.dumps(report))
    return 0


if __name__ == '__main__':
    sys.exit(main())
