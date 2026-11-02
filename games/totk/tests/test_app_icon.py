"""games/totk/app_icon.py: the BC4 decoder, the .ico and .res it writes, the neutral icon, and the ring from a RomFS.

    python -m games.totk.tests.test_app_icon [-v]

The RomFS case reads <work>/extracted/v100 (NXRECOMP_WORK, else C:\\nxrecomp-work): skipped without it.
Nothing derived from the game is written outside a temp folder."""
import io
import os
import struct
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

from games.totk import app_icon as A

ROMFS = Path(os.environ.get('NXRECOMP_WORK', r'C:\nxrecomp-work')) / 'extracted' / 'v100'


def res_entries(data):
    """[(type, name, lang, data)] of a .res file."""
    out, off = [], 0
    while off < len(data):
        size, head = struct.unpack_from('<II', data, off)
        _, rtype, _, name = struct.unpack_from('<HHHH', data, off + 8)
        lang = struct.unpack_from('<H', data, off + 22)[0]
        out.append((rtype, name, lang, data[off + head:off + head + size]))
        off += head + size + (-size % 4)
    return out


class AppIcon(unittest.TestCase):
    def test_bc4_block(self):
        # one block, endpoints 255 / 0 (six interpolated values): texel i takes index i % 8
        bits = sum((i % 8) << (3 * i) for i in range(16))
        block = bytes([255, 0]) + bits.to_bytes(6, 'little')
        raw = block + bytes(512 - 8)   # (a GOB: the block sits at offset 0)
        img = A.bc4_decode(raw, 4, 4, 1)
        pal = [255, 0] + [((7 - i) * 255) // 7 for i in range(1, 7)]
        self.assertEqual(img.reshape(-1).tolist(), [pal[i % 8] for i in range(16)])

    def test_bc4_four_value_mode(self):
        bits = sum((i % 8) << (3 * i) for i in range(16))
        block = bytes([0, 200]) + bits.to_bytes(6, 'little')   # a0 <= a1: four values, then 0 and 255
        img = A.bc4_decode(block + bytes(504), 4, 4, 1).reshape(-1)
        pal = [0, 200] + [((5 - i) * 0 + i * 200) // 5 for i in range(1, 5)] + [0, 255]
        self.assertEqual(img.tolist(), [pal[i % 8] for i in range(16)])

    def test_neutral_icon(self):
        img = A.neutral(256)
        a = np.asarray(img)[..., 3]
        self.assertEqual(img.size, (256, 256))
        self.assertEqual(a[0, 0], 0)              # transparent corner
        self.assertEqual(a[128, 128], 0)          # and hole
        self.assertGreater(a[128, 20], 200)       # opaque ring (the left side, past the crop's edge)

    def test_ico_and_res(self):
        entries = A.ico_entries(A.neutral(512))
        with tempfile.TemporaryDirectory() as t:
            A.write_ico(entries, Path(t) / 'a.ico')
            A.write_res(entries, Path(t) / 'a.res')
            ico = Image.open(io.BytesIO((Path(t) / 'a.ico').read_bytes()))
            self.assertEqual(sorted(ico.info['sizes']), sorted((s, s) for s in A.SIZES))
            for s in (16, 48, 256):
                ico.size = (s, s)
                self.assertEqual(ico.copy().convert('RGBA').size, (s, s))
            res = res_entries((Path(t) / 'a.res').read_bytes())
        self.assertEqual(res[0][:2], (0, 0))                                  # the empty first entry
        icons = [r for r in res if r[0] == 3]
        self.assertEqual([r[1] for r in icons], list(range(1, len(A.SIZES) + 1)))
        group = [r for r in res if r[0] == 14]
        self.assertEqual(len(group), 1)
        self.assertEqual(group[0][1], 1)                                      # ICON 1: what shim_win.c loads
        g = group[0][3]
        self.assertEqual(struct.unpack_from('<HHH', g), (0, 1, len(A.SIZES)))
        for i, (px, data) in enumerate(entries):
            w, _, _, _, _, bpp, size, nid = struct.unpack_from('<BBBBHHIH', g, 6 + 14 * i)
            self.assertEqual((w, bpp, size, nid), (px % 256, 32, len(data), i + 1))
            self.assertEqual(icons[i][3], data)
        self.assertEqual(entries[-1][1][:8], b'\x89PNG\r\n\x1a\n')           # 256 px: PNG
        self.assertEqual(struct.unpack_from('<I', entries[0][1])[0], 40)     # 16 px: a DIB

    @unittest.skipUnless((ROMFS / 'romfs').is_dir() or (ROMFS / 'romfs-index.json').is_file(), 'no extracted RomFS')
    def test_ring_from_romfs(self):
        ring, shadow = A.ring_masks(ROMFS)
        self.assertEqual((ring.size, shadow.size), ((427, 427), (547, 547)))
        for simple in (False, True):
            img = np.asarray(A.compose(ring, 256, simple))
            a = img[..., 3]
            self.assertEqual(a[128, 128], 0)                                  # the hole
            self.assertTrue(0.3 < (a > 128).mean() < 0.8)
            near = lambda c: (abs(img[..., :3].astype(int) - c) <= 12).all(-1) & (a == 255)
            mint, teal = near(A.MINT), near(A.TEAL)
            self.assertGreater(mint.mean(), 0.15)                             # the ring flat mint ...
            self.assertGreater(teal.mean(), 0.05)                             # ... on a flat teal halo
            self.assertGreater((mint | teal | (a == 0)).mean(), 0.8)          # 2 colours, the rest edges

    def test_bad_romfs_falls_back(self):
        with tempfile.TemporaryDirectory() as t:
            out = Path(t) / 'out'
            buf = io.StringIO()
            import contextlib
            with contextlib.redirect_stdout(buf):
                rc = A.main(['--romfs', str(Path(t) / 'missing'), '--out', str(out)])
            self.assertEqual(rc, 0)
            self.assertIn('"source": "neutral"', buf.getvalue())
            self.assertTrue((out / 'totk.ico').is_file() and (out / 'totk.res').is_file())


if __name__ == '__main__':
    unittest.main()
