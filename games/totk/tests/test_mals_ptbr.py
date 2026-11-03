"""games/totk/overlays/mals.py, Brazilian Portuguese (lang-ptbr): the USpt archive made over USen 1.0.0's from the
player's 1.4.x update, per message by label, English where the update lacks a message or uses a control tag 1.0.0 does
not; GuideKeyIcon English byte for byte; the font check; engine_dir serving it as USen's (original check, RSTB).
Synthetic MSBT / SARC / OpenType files first (no game data); the RomFS case reads <work>/extracted/v100 and v143
(NXRECOMP_WORK): skipped without.
    python -m unittest games.totk.tests.test_mals_ptbr"""
import hashlib
import os
import struct
import tempfile
import unittest
from pathlib import Path

from games.totk.overlays import RomFS, Zstd, sarc_members
from games.totk.overlays import engine_dir, mals as M

WORK = Path(os.environ['NXRECOMP_WORK']) if os.environ.get('NXRECOMP_WORK') else None   # (no default: skipped without)
V100, V143 = (WORK / 'extracted' / 'v100', WORK / 'extracted' / 'v143') if WORK else (Path('-'), Path('-'))


def u(text):
    return text.encode('utf-16-le') + b'\0\0'


def tag(group, kind, params=b''):
    return struct.pack('<4H', 0x0e, group, kind, len(params)) + params


def msbt(msgs, buckets=7):
    header = b'MsgStdBn' + b'\xff\xfe' + b'\0\0' + bytes([1, 3]) + b'\0\0' + b'\0\0' + b'\0' * 4 + b'\0' * 10
    return M.msbt_build(header, buckets, msgs)


def sarc(members):
    """a SARC of {name: bytes} (SFAT sorted by name hash, as the game's; data 8-aligned)"""
    def h(name):
        v = 0
        for ch in name.encode():
            v = (v * 0x65 + ch) & 0xffffffff
        return v
    names = sorted(members, key=h)
    sfnt, offs = bytearray(), []
    for n in names:
        offs.append(len(sfnt) // 4)
        sfnt += n.encode() + b'\0'
        sfnt += b'\0' * (-len(sfnt) % 4)
    data, spans = bytearray(), []
    for n in names:
        data += b'\0' * (-len(data) % 8)
        spans.append((len(data), len(data) + len(members[n])))
        data += members[n]
    sfat = b'SFAT' + struct.pack('<HHI', 12, len(names), 0x65) + b''.join(
        struct.pack('<4I', h(n), 0x01000000 | offs[i], *spans[i]) for i, n in enumerate(names))
    head_len = 0x14 + len(sfat) + 8 + len(sfnt)
    start = head_len + (-head_len % 8)
    out = bytearray(b'SARC' + struct.pack('<HHIIHH', 0x14, 0xfeff, 0, start, 0x100, 0))
    out += sfat + b'SFNT' + struct.pack('<HH', 8, 0) + sfnt
    out += b'\0' * (start - len(out)) + data
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def otf(cmap_subtables):
    """an OpenType file with only a cmap table of the given subtables ([(platform, encoding, bytes)])"""
    n = len(cmap_subtables)
    body, recs = bytearray(), bytearray()
    for plat, enc, sub in cmap_subtables:
        recs += struct.pack('>HHI', plat, enc, 4 + 8 * n + len(body))
        body += sub
    cmap = struct.pack('>HH', 0, n) + bytes(recs) + bytes(body)
    return struct.pack('>IHHHH', 0x00010000, 1, 16, 0, 0) + b'cmap' + struct.pack('>III', 0, 28, len(cmap)) + cmap


def fmt4(segments):
    """cmap format 4 of [(start, end, delta)] (idRangeOffset 0) plus the 0xffff end segment"""
    segs = list(segments) + [(0xffff, 0xffff, 1)]
    n = len(segs)
    out = struct.pack('>7H', 4, 16 + 8 * n, 0, 2 * n, 0, 0, 0)
    out += struct.pack(f'>{n}H', *[e for _, e, _ in segs]) + b'\0\0'
    out += struct.pack(f'>{n}H', *[s for s, _, _ in segs])
    out += struct.pack(f'>{n}h', *[d for _, _, d in segs])
    return out + struct.pack(f'>{n}H', *([0] * n))


def fmt12(groups):
    out = struct.pack('>HHIII', 12, 0, 16 + 12 * len(groups), 0, len(groups))
    return out + b''.join(struct.pack('>3I', *g) for g in groups)


def bfotf(font, key=0x1234abcd):
    font += b'\0' * (-len(font) % 4)
    words = struct.unpack('>%dI' % (len(font) // 4), font)
    return struct.pack('>II', 0x7f9a0218 ^ key, len(font) ^ key) + b''.join(struct.pack('>I', w ^ key) for w in words)


class Msbt(unittest.TestCase):
    def test_tags(self):
        self.assertEqual(M.msbt_tags(u('plain')), set())
        t = u('a') [:-2] + tag(1, 2, b'\x05\x00\x00\x00') + 'b'.encode('utf-16-le') + struct.pack('<3H', 0x0f, 0, 3) + b'\0\0'
        self.assertEqual(M.msbt_tags(t), {(1, 2), (0, 3)})
        # (a tag's parameters are skipped: a 0x0e inside them is no tag)
        t2 = tag(0, 4, struct.pack('<2H', 0x0e, 9)) + b'\0\0'
        self.assertEqual(M.msbt_tags(t2), {(0, 4)})

    def test_member_by_label(self):
        en = msbt([('A', u('Sword')), ('B', u('Shield')), ('C', tag(0, 3) + u('Bow')), ('D', u('Link'))])
        pt = msbt([('E', u('Novo')), ('D', u('Link')), ('C', tag(7, 7) + u('Arco')), ('A', u('Espada'))], buckets=3)
        out, st = M.pt_member('x.msbt', en, pt, allowed={(0, 3)})
        header, buckets, msgs = M.msbt_parse(out)
        self.assertEqual(buckets, 7)                                    # (en's hash table and order)
        self.assertEqual([lb for lb, _ in msgs], ['A', 'B', 'C', 'D'])  # (E: 1.0.0 has no such message)
        self.assertEqual(dict(msgs), {'A': u('Espada'), 'B': u('Shield'), 'C': tag(0, 3) + u('Bow'), 'D': u('Link')})
        self.assertEqual((st['pt'], st['missing'], st['tags']), (2, ['B'], ['C']))
        self.assertEqual(header[:0x0e], M.msbt_parse(en)[0][:0x0e])

    def test_member_unchanged_is_en(self):
        en = msbt([('A', u('Same'))])
        out, st = M.pt_member('x.msbt', en, msbt([('A', u('Same'))], buckets=2), set())
        self.assertIs(out, en)
        out, st = M.pt_member('x.msbt', en, None, set())                # (a member the update lacks)
        self.assertIs(out, en)
        self.assertEqual(st['missing'], ['A'])


class Sarc(unittest.TestCase):
    def test_archive(self):
        gki = msbt([('A', u('\ue040'))])
        en = sarc({'LayoutMsg/A.msbt': msbt([('L1', u('Hello')), ('L2', u('Bye'))]),
                   'EventFlowMsg/B.msbt': msbt([('L1', u('Same'))]), M.GUIDE_KEY_ICON: gki})
        pt = sarc({'LayoutMsg/A.msbt': msbt([('L1', u('Olá')), ('L2', u('Tchau'))]),
                   'EventFlowMsg/B.msbt': msbt([('L1', u('Same'))]), M.GUIDE_KEY_ICON: msbt([('A', u('x'))]),
                   'LayoutMsg/Only143.msbt': msbt([('L', u('Novo'))])})
        out, stats = M.pt_sarc(en, pt, set())
        a, b = sarc_members(out), sarc_members(en)
        self.assertEqual(list(a), list(b))                              # (en's members, en's order)
        self.assertEqual(out[slice(*a[M.GUIDE_KEY_ICON])], gki)         # (the glyph bank's signature)
        self.assertEqual(out[slice(*a['EventFlowMsg/B.msbt'])], en[slice(*b['EventFlowMsg/B.msbt'])])
        self.assertEqual(dict(M.msbt_parse(out[slice(*a['LayoutMsg/A.msbt'])])[2]), {'L1': u('Olá'), 'L2': u('Tchau')})
        self.assertTrue(stats[M.GUIDE_KEY_ICON]['kept'])
        self.assertEqual(M.text_chars(out) - M.text_chars(en), {ord(c) for c in 'OláTchu'} - M.text_chars(en))


class Fonts(unittest.TestCase):
    def test_cmap_and_check(self):
        latin = [(0x30, 0x39, 1), (0x41, 0x5a, 1), (0x61, 0x7a, 1)]
        full = otf([(3, 1, fmt4(latin + [(0xe0, 0xe3, 1)])),
                    (3, 10, fmt12([(0xe7, 0xe7, 50), (0xf5, 0xf5, 51)]))])
        chars = M.otf_chars(M.bfotf_otf(bfotf(full)))
        self.assertTrue({ord(c) for c in 'AZaz09àáâãçõ'} <= chars)
        self.assertNotIn(ord('é'), chars)
        symbol = otf([(3, 1, fmt4([(0xe040, 0xe041, 1)]))])           # (no letters: not a text font)
        fonts = sarc({'scft/Text.bfotf': bfotf(full), 'scft/Symbol.bfotf': bfotf(symbol), 'fcpx/N.bfcpx': b'x'})
        r = M.font_check(fonts, used={ord('ã'), ord('é'), 0xe09a, 0xe040}, english={0xe09a})
        self.assertEqual(list(r['fonts']), ['scft/Text.bfotf'])
        self.assertEqual(r['fonts']['scft/Text.bfotf'], 'êéíóúôÃÕÇÊÁÉÍÓÚÂÔÀ')
        self.assertEqual(r['in_no_font'], ['U+00E9', 'U+E040'])        # (U+E09A: drawn as in English)


@unittest.skipUnless(((V100 / 'romfs-index.json').is_file() or (V100 / 'romfs').is_dir()) and
                     ((V143 / 'romfs-index.json').is_file() or (V143 / 'romfs').is_dir()), 'no extracted 1.0.0 + 1.4.3')
class RealUpdate(unittest.TestCase):
    def test_portuguese_and_engine_dir(self):
        rom, upd = RomFS(V100), RomFS(V143)
        z = Zstd(rom)
        (path, data, raw_en), edited, rep, lines = M.portuguese(rom, z, upd, M.PATCHERS)
        self.assertEqual(path, 'Mals/USpt.Product.100.sarc.zs')
        en = bytes(z.decompress(raw_en)[0])
        out = bytes(z.decompress(data)[0])
        a, b = sarc_members(out), sarc_members(en)
        self.assertEqual(list(a), list(b))
        for name in b:   # (every 1.0.0 message id resolves: the same labels, member by member)
            la = [lb for lb, _ in M.msbt_parse(out[slice(*a[name])])[2]]
            lb_ = [lb for lb, _ in M.msbt_parse(en[slice(*b[name])])[2]]
            if name in edited:   # (the patchers add the PC rows' messages after the game's)
                self.assertEqual(la[:len(lb_)], lb_, name)
            else:
                self.assertEqual(la, lb_, name)
        self.assertEqual(out[slice(*a[M.GUIDE_KEY_ICON])], en[slice(*b[M.GUIDE_KEY_ICON])])
        self.assertEqual(rep['messages'], rep['portuguese'] + rep['english_missing'] + rep['english_tags'])
        self.assertGreater(rep['portuguese'], 0.99 * rep['messages'])
        self.assertEqual(len(lines), rep['english_missing'] + rep['english_tags'])
        # the game's main text fonts have every Portuguese letter (1.0.0's, the same 1.4.3 draws its own with)
        for f in ('scft/nintendo_NTLG-DB_DH_002.bfotf', 'scft/ninP_RodinNTLG-M_DH_003_subset.bfotf'):
            self.assertEqual(rep['glyphs']['fonts'].get(f), '', f)
        self.assertEqual(rep['glyphs']['in_no_font'], [])
        with tempfile.TemporaryDirectory() as tmp:
            sets, eng = Path(tmp) / 'sets', Path(tmp) / 'engine'
            M._write_set(sets / 'mals', [(path, data, raw_en)])
            eng.mkdir()
            r = engine_dir.build(V100, ['mals'], sets, eng, Path(tmp) / 'out')
            self.assertEqual(r['status'], 'pass')
            served = Path(tmp) / 'out' / path
            self.assertEqual(hashlib.sha256(served.read_bytes()).digest(), hashlib.sha256(data).digest())
            # (no bigger than USen's own: the table as shipped; else USen's entry raised by the difference)
            self.assertEqual([(x['path'], x['raised_by']) for x in r['rstb_raised']],
                             [] if len(out) <= len(en) else [('Mals/USen.Product.100.sarc', len(out) - len(en))])


if __name__ == '__main__':
    unittest.main()
