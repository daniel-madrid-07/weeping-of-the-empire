"""games/totk/overlays build_grass: grass-<percent> scales every density of the vegetation / leaves / stone / chips sets
(capped at 1.0); grass-<percent>-noturf leaves ResVegeSet's TurfSet as shipped and scales the rest the same. No value
past 1.0, nothing else in the pack changes. The RomFS case reads <work>/extracted/v100 (NXRECOMP_WORK): skipped without.
    python -m unittest games.totk.tests.test_grass_sets"""
import os
import tempfile
import unittest
from pathlib import Path

from games.totk import overlays as O
from games.totk.overlays import byml

ROMFS = Path(os.environ.get('NXRECOMP_WORK', r'C:\nxrecomp-work')) / 'extracted' / 'v100'


def densities(pack):
    """{(member, path): value} of every DensityMin / DensityMax float in the density kinds"""
    out = {}
    for name, (s, e) in O.sarc_members(pack).items():
        if name.split('/')[1] in O.GRASS_DENSITY_KINDS:
            for path, t, value, off in byml.walk(pack[s:e]):
                if t == byml.FLOAT and path and path[-1] in ('DensityMin', 'DensityMax'):
                    out[(name, tuple(path))] = value
    return out


@unittest.skipUnless((ROMFS / 'romfs-index.json').is_file() or (ROMFS / 'romfs').is_dir(), 'no extracted RomFS')
class GrassSets(unittest.TestCase):
    def test_noturf_keeps_turf_and_scales_the_rest(self):
        rom = O.RomFS(ROMFS)
        z = O.Zstd(rom)
        shipped_raw = rom.read(O.GRASS_PACK)
        shipped = bytes(z.decompress(shipped_raw)[0])
        with tempfile.TemporaryDirectory() as tmp:
            full = O.build_grass(rom, z, tmp, 3.0)
            nt = O.build_grass(rom, z, tmp, 3.0, turf=False)
            self.assertEqual((full['set'], nt['set']), ('grass-300', 'grass-300-noturf'))
            read = lambda d: bytes(z.decompress((Path(d) / O.GRASS_PACK).read_bytes())[0])  # noqa: E731
            a, b = read(full['dir']), read(nt['dir'])
        self.assertEqual((len(a), len(b)), (len(shipped), len(shipped)))
        d0, d3, dn = densities(shipped), densities(a), densities(b)
        self.assertEqual(set(d0), set(d3))
        turf = [k for k in d0 if len(k[1]) > 1 and k[1][-2] == 'TurfSet']
        self.assertTrue(turf)
        for k, v in d0.items():
            want = min(1.0, v * 3) if v > 0 else v
            self.assertAlmostEqual(d3[k], want, places=5, msg=k)
            self.assertAlmostEqual(dn[k], v if k in turf else want, places=5, msg=k)
            self.assertLessEqual(max(d3[k], dn[k]), 1.0)
        self.assertEqual(nt['values'], full['values'] - sum(1 for k in turf if d0[k] > 0))
        # (every other byte as shipped: only the density floats moved)
        diff = [i for i in range(len(shipped)) if shipped[i] != b[i]]
        self.assertTrue(diff and len(diff) <= 4 * nt['values'])


if __name__ == '__main__':
    unittest.main()
