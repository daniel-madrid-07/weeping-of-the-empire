"""games/totk/patches/port.py: the 1.0.0 patch set relocated to a synthetic other build.

    python -m games.totk.tests.test_port

The whole 1.0.0 set (every feature) is moved to a layout whose code is shifted, whose padding starts elsewhere and whose
data page moved; every word must land where the shift says and every PC-relative word must reach the moved target.
The replaced words come from the 1.0.0 image (the work folder's, games.totk.patches.default_image): skipped without it.
"""
import unittest

from games.totk.patches import PatchError, default_image, layout_v100, load_main_image, sites_v100
from games.totk.patches.port import PortError, needed_addresses, pcrel_target, relocate, retarget
from games.totk.patches.sites_v100 import Layout, Orig


def _image():
    try:
        return load_main_image(default_image())
    except (PatchError, OSError):
        return None


IMAGE = _image()


def v100_set():
    return sites_v100.with_image(sites_v100.build(layout_v100(), list(sites_v100.FEATURES)), IMAGE)

SHIFT = 0x1230 * 4               # code moved by this much (inserted functions before every site)


class PcrelTest(unittest.TestCase):
    @unittest.skipUnless(IMAGE, 'no 1.0.0 main image in the work folder')
    def test_round_trip_every_word(self):
        """Every PC-relative word of the 1.0.0 set re-encodes to itself at its own place."""
        n = 0
        for p in v100_set():
            for w in p.words:
                for word in (w.original, w.new):
                    if isinstance(word, Orig):
                        continue
                    t = pcrel_target(word, w.offset)
                    if t:
                        n += 1
                        self.assertEqual(retarget(word, w.offset, t[1]), word, f'{p.feature} {w.offset:#x} {word:08x}')
        self.assertGreater(n, 100)

    def test_kinds(self):
        self.assertEqual(pcrel_target(0x94000002, 0x1000), ('b', 0x1008))           # bl +8
        self.assertEqual(pcrel_target(0x17FFFFFF, 0x1000), ('b', 0xFFC))            # b -4
        self.assertEqual(pcrel_target(0x54000048, 0x1000), ('bcond', 0x1008))       # b.hi +8
        self.assertEqual(pcrel_target(0x34000060, 0x1000), ('cb', 0x100C))          # cbz w0, +12
        self.assertEqual(pcrel_target(0x37080040, 0x1000), ('tb', 0x1008))          # tbnz w0, #1, +8
        self.assertEqual(pcrel_target(0x90000009, 0x1234), ('adrp', 0x1000))        # adrp x9, same page
        self.assertEqual(pcrel_target(0x10000040, 0x1000), ('adr', 0x1008))         # adr x0, +8
        self.assertIsNone(pcrel_target(0xD503201F, 0x1000))                         # nop
        self.assertEqual(retarget(0x54000048, 0x2000, 0x1F00), 0x54FFF808)          # b.hi -0x100, cond kept
        with self.assertRaises(PortError):
            retarget(0x34000060, 0x1000, 0x1000 + (1 << 21))                       # cbz out of range


@unittest.skipUnless(IMAGE, 'no 1.0.0 main image in the work folder')
class RelocateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.old = layout_v100()
        cls.new = Layout(text_end=cls.old.text_end + SHIFT + 0x10, data_off=cls.old.data_off + 0x20000)
        cls.patches = v100_set()
        cls.moved = relocate(cls.patches, cls.old, cls.new, lambda t: t + SHIFT)

    def where(self, t):
        if self.old.text_end <= t < self.old.pad_end:
            return t - self.old.pad + self.new.pad
        if self.old.data_off <= t < self.old.data_off + sites_v100.DATA_PAGE_SIZE:
            return t - self.old.data_off + self.new.data_off
        return t + SHIFT

    def test_identity(self):
        same = relocate(self.patches, self.old, self.old, lambda t: t)
        for p, q in zip(self.patches, same):
            self.assertEqual([(w.offset, w.original, w.new) for w in p.words],
                             [(w.offset, w.original, w.new) for w in q.words])
            self.assertEqual(p.veneers, q.veneers)

    def test_every_word(self):
        for p, q in zip(self.patches, self.moved):
            self.assertEqual(len(p.words), len(q.words))
            self.assertEqual(q.veneers, [(self.where(a), self.where(b - 4) + 4) for a, b in p.veneers])
            for w, v in zip(p.words, q.words):
                self.assertEqual(v.offset, self.where(w.offset))
                self.assertEqual(v.v100_offset, w.offset)
                for old_word, new_word in ((w.original, v.original), (w.new, v.new)):
                    if isinstance(old_word, Orig):         # a copy moves with the word it copies
                        self.assertEqual(new_word, Orig(self.where(old_word.offset)))
                        continue
                    t = pcrel_target(old_word, w.offset)
                    if t is None:
                        self.assertEqual(new_word, old_word)
                    else:
                        self.assertEqual(pcrel_target(new_word, v.offset), (t[0], self.where(t[1])),
                                         f'{p.feature} main+{w.offset:#x}')

    def test_missing_site_refuses(self):
        site = sites_v100.DYNRES_SITE
        with self.assertRaises(PortError) as e:
            relocate(self.patches, self.old, self.new, lambda t: None if t == site else t + SHIFT)
        self.assertIn(f'{site:#x}', str(e.exception))

    def test_short_padding_refuses(self):
        tight = Layout(text_end=self.new.pad_end - 0x100, data_off=self.new.data_off)
        with self.assertRaises(PortError) as e:
            relocate(self.patches, self.old, tight, lambda t: t + SHIFT)
        self.assertIn('padding', str(e.exception))

    def test_needed(self):
        need = needed_addresses(self.patches, self.old)
        self.assertIn(sites_v100.DYNRES_SITE, need)
        self.assertIn(sites_v100.DYNRES_SITE + 4, need)            # the veneer's way back
        self.assertIn(sites_v100.CLOTH_UPDATE, need)               # a call target
        self.assertTrue(all(t < self.old.text_end for t in need))


if __name__ == '__main__':
    unittest.main()
