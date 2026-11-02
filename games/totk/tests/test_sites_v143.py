"""games/totk/patches/sites_v143.py: TotK 1.4.3's own frame-rate sites.

    python -m games.totk.tests.test_sites_v143

Without a game: the new words match MacNativeGames' 1.4.3 replacements (the sites' source, MIT), every veneer sits in
the padding and returns where it should, and the engine's veneer layout (engine.layout) accepts the set. With the work
folder's 1.4.3 main (<work>/extracted/v143/exefs/main): `port --version 1.4.3` writes the table from these sites and
apply patches that main.
"""
import json
import os
import struct
import tempfile
import unittest
from types import SimpleNamespace

from games.totk.patches import PatchError, apply, read_table, resolve_features, load_main_image
from games.totk.patches import arm64 as a, engine, port, sites_v143 as s
from games.totk.patches.sites_v100 import Orig

LAY = s.layout()
ALL = s.build(LAY, list(s.FEATURES))
NOP = 0xd503201f


def words_of(feature):
    return {w.offset: w.new for p in ALL if p.feature == feature for w in p.words}


def v143_main():
    work = os.environ.get('NXRECOMP_WORK') or r'C:\nxrecomp-work'
    p = os.path.join(work, 'extracted', 'v143', 'exefs', 'main')
    return p if os.path.isfile(p) else None


class EncodingTest(unittest.TestCase):
    def test_new_helpers(self):
        self.assertEqual(a.clz_w(10, 8), 0x5ac0110a)
        self.assertEqual(a.lsr_w(10, 10, 5), 0x53057d4a)
        self.assertEqual(a.add_w(8, 8, 10), 0x0b0a0108)
        self.assertEqual(a.lsr_w(11, 11, 5), 0x53057d6b)
        with self.assertRaises(ValueError):
            a.lsr_w(1, 1, 32)

    def test_mac_native_games_words(self):
        """the words MacNativeGames' 1.4.3 replacements hold (their @page adrp and the re-run original aside)"""
        rt, cl, hf, fd = (words_of(f) for f in ('realtime', 'cloth', 'highfps', 'fade'))
        self.assertEqual([rt[0x2daa58], rt[0x2daa6c], rt[0x2b036c]], [0xbd400120, NOP, 0x0f000401])
        v = LAY.pad + s.PAD_CLOTH
        self.assertEqual([cl[v + 4], cl[v + 8], cl[v + 12], cl[v + 16]], [0xbd400100, 0x1e200940, 0xbd0367e0, 0xbd400500])
        v += 28
        self.assertEqual([cl[v + 4], cl[v + 8], cl[v + 12], cl[v + 16]], [0xbd400101, 0x1e210801, 0xbd0117e1, 0xbd400501])
        v += 28
        self.assertEqual([cl[v + 4], cl[v + 8], cl[v + 12]], [0xbd400104, 0x1e240842, 0x1e240863])
        v = LAY.pad + s.PAD_BOUNCE
        self.assertEqual([hf[v + 4], hf[v + 8]], [0xbd403921, 0x1e210800])
        v = LAY.pad + s.PAD_FADE
        self.assertEqual([fd[v + 4], fd[v + 8], fd[v + 12]], [0x5ac0110a, 0x53057d4a, 0x0b0a0108])
        v += 40
        self.assertEqual([fd[v + 4], fd[v + 8], fd[v + 12]], [0x5ac0114b, 0x53057d6b, 0x0b0b014a])
        rr = words_of('render_res')
        self.assertEqual([rr[0x585e50], rr[0x585e60]], [0x2d420460, 0xbd0cca61])
        self.assertEqual(rr[0x585e54], Orig(0x585e50))


class LayoutTest(unittest.TestCase):
    def test_veneers_return(self):
        """each veneer is entered from its site and leaves to the word after it (a bl veneer: to the update)"""
        seen = 0
        for p in ALL:
            words = {w.offset: w.new for w in p.words}
            for start, end in p.veneers:
                self.assertTrue(LAY.pad <= start < end <= LAY.pad_end, f'{p.feature} {start:#x}..{end:#x}')
            for site, w in words.items():
                br = engine.branch(w, site) if site < LAY.text_end and isinstance(w, int) else None
                if not br or br[1] < LAY.text_end:
                    continue
                o = br[1]                            # walk the veneer to its first branch: the way out
                while not (isinstance(words[o], int) and (engine.branch(words[o], o) or (None,))[0] == 'b'):
                    o += 4
                self.assertEqual(engine.branch(words[o], o), ('b', s.CLOTH_UPDATE if br[0] == 'bl' else site + 4),
                                 f'{p.feature} {site:#x}')
                seen += 1
        self.assertEqual(seen, 1 + 4 + 1 + 4 + 1 + 2 + 4)

    def test_no_overlap(self):
        offs = [w.offset for p in ALL for w in p.words]
        self.assertEqual(len(offs), len(set(offs)))
        self.assertLessEqual(max(e for p in ALL for _, e in p.veneers), LAY.pad + 528)   # the rest stays free

    def test_data_page(self):
        """every adrp reaches the data page after 1.4.3's span"""
        self.assertEqual(LAY.data_off, 0x3ae0000)
        n = 0
        for p in ALL:
            for w in p.words:
                if isinstance(w.new, int) and w.new & 0x9f000000 == 0x90000000:
                    self.assertEqual(port.pcrel_target(w.new, w.offset), ('adrp', LAY.data_off))
                    n += 1
        self.assertEqual(n, 1 + 1 + 4 + 1 + 1 + 2 + 4)

    def test_engine_layout(self):
        """engine.layout over a stand-in FDE map (one function around each site, the update at its own start)"""
        rows = [dict(feature=p.feature, offset=w.offset,
                     new=struct.pack('<I', NOP if isinstance(w.new, Orig) else w.new)) for p in ALL for w in p.words]
        table = dict(game=s.game(LAY), patch=rows,
                     veneer=[dict(feature=p.feature, start=a_, end=b_) for p in ALL for a_, b_ in p.veneers])
        fde = {0xa7a00: 0x200, 0x95dc00: 0x200, 0x615500: 0x200, 0x246e00: 0x200, 0x345800: 0x200,
               0x249400: 0x100, 0x28bc00: 0x100, 0x2a0e40: 0x500, 0x3e5600: 0x100, 0x562a00: 0x100,
               0x2daa00: 0x100, 0x2b0300: 0x100, 0x585e00: 0x100, 0x584800: 0x100, 0x628c00: 0x100,
               0xb43400: 0x100, 0x1ff0400: 0x200, 0x253fb00: 0x200}
        lay = engine.layout(table, list(s.FEATURES), fde)
        self.assertEqual(sorted(lay['fde_add']), [LAY.pad + s.PAD_CLOTH + 56, LAY.pad + s.PAD_CLOTH + 76])
        self.assertEqual(sum(len(v) for v in lay['ext'].values()), 1 + 2 + 1 + 4 + 1 + 2 + 4)

    def test_needs(self):
        with self.assertRaises(ValueError):
            s.build(LAY, ['cloth'])


class PortTest(unittest.TestCase):
    def test_sites_module(self):
        self.assertIs(port.sites_module('1.4.3'), s)
        self.assertIsNone(port.sites_module('1.0.0'))
        self.assertIsNone(port.sites_module('1.4.2'))

    def test_engine_spec(self):
        self.assertEqual(engine.engine_spec('default'), 'default,vk_guard')
        self.assertEqual(engine.engine_spec('default', set(s.FEATURES)), 'default')

    @unittest.skipUnless(v143_main(), 'no 1.4.3 main in the work folder')
    def test_table_applies(self):
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'v143.toml')
            args = SimpleNamespace(old=None, new=v143_main(), version='1.4.3', out=out, report=None, diagnostic=True)
            self.assertEqual(port.main_port(args), 0)
            table = read_table(out)
            self.assertEqual(table['game']['data_page'], LAY.data_off)
            feats = resolve_features('default', table)
            self.assertEqual(feats, list(s.FEATURES))
            image = load_main_image(v143_main(), table['game'])
            report = apply(image, table, feats)
            self.assertEqual(report['changed_words'], sum(len(p.words) for p in ALL))
            image[s.STEP_SITE] ^= 1      # another build: refused
            with self.assertRaises(PatchError):
                apply(image, read_table(out), feats)


if __name__ == '__main__':
    unittest.main()
