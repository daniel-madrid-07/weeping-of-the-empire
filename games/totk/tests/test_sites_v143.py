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
        self.assertEqual(a.cmp_w_imm(1, 8, lsl12=True), 0x7140203f)   # cmp w1, #8, lsl #12
        self.assertEqual(a.cmp_w_imm(1, 8), 0x7100203f)

    def test_reportguard_words(self):
        """the two sites branch to their veneers; each veneer re-runs its site and leaves to the loop's next object"""
        rg = words_of('reportguard')
        h = LAY.pad + s.PAD_REPORT
        n = h + 16
        self.assertEqual(rg[s.REPORT_H_SITE], a.b(s.REPORT_H_SITE, h))
        self.assertEqual(rg[s.REPORT_N_SITE], a.b(s.REPORT_N_SITE, n))
        self.assertEqual([rg[h + 4 * i] for i in range(4)],
                         [0x37f80061, Orig(s.REPORT_H_SITE), a.b(h + 8, s.REPORT_H_SITE + 4), a.b(h + 12, s.REPORT_LOOP_NEXT)])
        self.assertEqual([rg[n + 4 * i] for i in range(5)],
                         [0x7140203f, 0x54000062, Orig(s.REPORT_N_SITE), a.b(n + 12, s.REPORT_N_SITE + 4),
                          a.b(n + 16, s.REPORT_LOOP_NEXT)])

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

    def test_vk_guard_words(self):
        """1.0.0's vk_guard words at 1.4.3's sites: movz w1, #27 at the six, the inline read on byte 3 / bit 3"""
        vk = words_of('vk_guard')
        self.assertEqual({o: vk[o] for o in s.GUARD_SITES}, {o: 0x52800361 for o in s.GUARD_SITES})
        self.assertEqual(vk[0x3e3edc], 0x39400d08)                                   # ldrb w8, [x8, #3]
        self.assertEqual(vk[0x3e3ee0], a.tbnz(8, 3, 0x3e3ee0, 0x3e41a0))             # tbnz w8, #3, 0x3e41a0
        self.assertEqual(len(vk), 8)


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
        self.assertEqual(seen, 1 + 4 + 1 + 4 + 1 + 2 + 4 + 3 + 2)

    def test_no_overlap(self):
        offs = [w.offset for p in ALL for w in p.words]
        self.assertEqual(len(offs), len(set(offs)))
        self.assertLessEqual(max(e for p in ALL for _, e in p.veneers), LAY.pad + 756)   # the rest stays free

    def test_veneers_disjoint(self):
        """every feature's veneer ranges, pairwise apart (build() checks only that they end before pad_end)"""
        spans = sorted((a_, b_, p.feature) for p in ALL for a_, b_ in p.veneers)
        for (a0, b0, f0), (a1, b1, f1) in zip(spans, spans[1:]):
            self.assertLessEqual(b0, a1, f'{f0} {a0:#x}..{b0:#x} overlaps {f1} {a1:#x}..{b1:#x}')
        self.assertGreaterEqual(s.PAD_REPORT, s.PAD_SHADOW + 76 + 44 + 60)

    def test_data_page(self):
        """every adrp reaches the data page after 1.4.3's span"""
        self.assertEqual(LAY.data_off, 0x3ae0000)
        n = 0
        for p in ALL:
            for w in p.words:
                if isinstance(w.new, int) and w.new & 0x9f000000 == 0x90000000:
                    self.assertEqual(port.pcrel_target(w.new, w.offset), ('adrp', LAY.data_off))
                    n += 1
        self.assertEqual(n, 1 + 1 + 4 + 1 + 1 + 2 + 4 + 3)

    def test_engine_layout(self):
        """engine.layout over a stand-in FDE map (one function around each site, the update at its own start)"""
        rows = [dict(feature=p.feature, offset=w.offset,
                     new=struct.pack('<I', NOP if isinstance(w.new, Orig) else w.new)) for p in ALL for w in p.words]
        table = dict(game=s.game(LAY), patch=rows,
                     veneer=[dict(feature=p.feature, start=a_, end=b_) for p in ALL for a_, b_ in p.veneers])
        fde = {0xa7a00: 0x200, 0x95dc00: 0x200, 0x615500: 0x200, 0x246e00: 0x200, 0x345800: 0x200,
               0x249400: 0x100, 0x28bc00: 0x100, 0x2a0e40: 0x500, 0x3e5600: 0x100, 0x562a00: 0x100,
               0x2daa00: 0x100, 0x2b0300: 0x100, 0x585e00: 0x100, 0x584800: 0x100, 0x628c00: 0x100,
               0xb43400: 0x100, 0x1ff0400: 0x200, 0x253fb00: 0x200,
               0x23eb000: 0x1000, 0x23f5100: 0x200, 0x2789000: 0x400, 0x3e3d00: 0x600,   # (vk_guard's)
               0xbfca8: 448, 0x68318c: 3692,   # (shadowres's two writers)
               0xa1f84: 0x568}   # (reportguard's loop)
        lay = engine.layout(table, list(s.FEATURES), fde)
        self.assertEqual(sorted(lay['fde_add']), [LAY.pad + s.PAD_CLOTH + 56, LAY.pad + s.PAD_CLOTH + 76])
        self.assertEqual(sum(len(v) for v in lay['ext'].values()), 1 + 2 + 1 + 4 + 1 + 2 + 4 + 3 + 2)
        self.assertEqual(lay['ext'][0xa1f84], [(LAY.pad + s.PAD_REPORT, LAY.pad + s.PAD_REPORT + 16),
                                               (LAY.pad + s.PAD_REPORT + 16, LAY.pad + s.PAD_REPORT + 36)])

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
        self.assertEqual(engine.engine_spec('default', set(s.FEATURES)), 'default,vk_guard')
        # another version: only when its engine map kept the hook vk_guard needs (a refused pin leaves it off)
        self.assertEqual(engine.engine_spec('default', set(s.FEATURES), kept=set()), 'default')
        self.assertEqual(engine.engine_spec('default', set(s.FEATURES), kept={'1ccf894'}), 'default,vk_guard')
        # ... and the key hook's mirror fields (without them the pad's ZL could not guard)
        need = set(engine.FIELDS_NEEDED['vk_guard'])
        self.assertEqual(engine.engine_spec('default', set(s.FEATURES), {'1ccf894'}, need), 'default,vk_guard')
        self.assertEqual(engine.engine_spec('default', set(s.FEATURES), {'1ccf894'}, need - {'ExecutePlayerCalcKey+0x38'}),
                         'default')

    def test_kept_hooks(self):
        import json
        with tempfile.TemporaryDirectory() as t:
            toml = os.path.join(t, 'v143.toml')
            with open(os.path.join(t, 'v143.engine.json'), 'w') as f:
                json.dump({'hook': ['0xf23d70'], 'alias': {'0xf23d70': '0x1ccf894', '0x10': '0x20'}}, f)
            self.assertEqual(engine.kept_hooks(toml, {'game': {'version': '1.4.3'}}), {'1ccf894'})
            self.assertIsNone(engine.kept_hooks(toml, {'game': {'version': '1.0.0'}}))
            with open(os.path.join(t, 'v143.engine.json'), 'w') as f:   # a hook listed without its alias: refused
                json.dump({'hook': ['0xf23d70', '0xabc'], 'alias': {'0xf23d70': '0x1ccf894'}}, f)
            with self.assertRaises(engine.EngineError):
                engine.kept_hooks(toml, {'game': {'version': '1.4.3'}})

    @unittest.skipUnless(v143_main(), 'no 1.4.3 main in the work folder')
    def test_table_applies(self):
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'v143.toml')
            args = SimpleNamespace(old=None, new=v143_main(), version='1.4.3', out=out, report=None, diagnostic=True)
            self.assertEqual(port.main_port(args), 0)
            table = read_table(out)
            self.assertEqual(table['game']['data_page'], LAY.data_off)
            feats = resolve_features('default', table)
            self.assertEqual(feats, [f for f, v in s.FEATURES.items() if v[1]])   # (vk_guard: engine-only)
            image = load_main_image(v143_main(), table['game'])
            report = apply(image, table, feats + ['vk_guard'])   # (its original words checked against this main)
            self.assertEqual(report['changed_words'], sum(len(p.words) for p in ALL))
            image[s.STEP_SITE] ^= 1      # another build: refused
            with self.assertRaises(PatchError):
                apply(image, read_table(out), feats)


class ShadowresTest(unittest.TestCase):
    """shadowres on the real 1.4.3 main under Unicorn: both writers' sizes with the multiplier off / 2 / 0.5 and the
    fixed size off / on, against the original code; the saved registers, sp and the flags come back."""

    @classmethod
    def setUpClass(cls):
        if not v143_main():
            raise unittest.SkipTest('no 1.4.3 main in the work folder')
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, arm64_const as R
        cls.R, cls.Uc, cls.modes = R, Uc, (UC_ARCH_ARM64, UC_MODE_ARM)
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'v143.toml')
            args = SimpleNamespace(old=None, new=v143_main(), version='1.4.3', out=out, report=None, diagnostic=False)
            assert port.main_port(args) == 0
            table = read_table(out)
        cls.original = bytes(load_main_image(v143_main(), table['game']))
        img = load_main_image(v143_main(), table['game'])
        apply(img, table, ['shadowres'])
        cls.patched = bytes(img)

    def machine(self, image, scale, fixed):
        R = self.R
        mu = self.Uc(*self.modes)
        mu.mem_map(0, (len(image) + 0xfff) & ~0xfff)
        mu.mem_write(0, image)
        mu.mem_map(LAY.data_off, 0x4000)
        page = bytearray(0x200)
        struct.pack_into('<f', page, s.DP_SHADOW_SCALE, scale)
        struct.pack_into('<f', page, s.DP_SHADOW_FIXED, fixed)
        mu.mem_write(LAY.data_off, bytes(page))
        mu.mem_map(0x70000000, 0x10000)
        mu.reg_write(R.UC_ARM64_REG_SP, 0x70004000)
        mu.reg_write(R.UC_ARM64_REG_NZCV, 0x60000000)
        return mu

    def fs(self, mu, n, v=None):
        r = getattr(self.R, f'UC_ARM64_REG_S{n}')
        if v is None:
            return struct.unpack('<f', struct.pack('<I', mu.reg_read(r) & 0xffffffff))[0]
        mu.reg_write(r, struct.unpack('<I', struct.pack('<f', v))[0])

    def test_writer_a(self):
        """from the factor's fcmp to the stores: w10 / w8 = the rounded width / height"""
        R = self.R
        for fixed in (0.0, 1.0):
            for k in (0.0, 2.0, 0.5):
                for name, image in (('patched', self.patched), ('original', self.original)):
                    mu = self.machine(image, k, fixed)
                    self.fs(mu, 0, 0.6), self.fs(mu, 1, 0.4), self.fs(mu, 2, 2048.0), self.fs(mu, 3, 1024.0)
                    mu.reg_write(R.UC_ARM64_REG_X10, 2048), mu.reg_write(R.UC_ARM64_REG_X8, 1024)
                    mu.reg_write(R.UC_ARM64_REG_X12, 0x1111), mu.reg_write(R.UC_ARM64_REG_X13, 0x2222)
                    mu.reg_write(R.UC_ARM64_REG_Q4, 0x0123456789abcdef0011223344556677)
                    mu.emu_start(0xbfd5c, 0xbfda8, count=64)
                    kk = k if name == 'patched' and k else 1.0
                    f = 1.0 if name == 'patched' and fixed else 0.6
                    want = [max(32, (min(int(f * b * kk), int(b * kk)) + 7) & ~7) for b in (2048, 1024)]
                    got = [mu.reg_read(R.UC_ARM64_REG_X10) & 0xffffffff, mu.reg_read(R.UC_ARM64_REG_X8) & 0xffffffff]
                    self.assertEqual(got, want, (name, fixed, k))
                    if name == 'patched':
                        self.assertEqual((mu.reg_read(R.UC_ARM64_REG_X13), mu.reg_read(R.UC_ARM64_REG_Q4),
                                          mu.reg_read(R.UC_ARM64_REG_SP)),
                                         (0x2222, 0x0123456789abcdef0011223344556677, 0x70004000))

    def test_writer_b(self):
        """from lo's scvtf to the round-up: w19 = the rounded size; the fixed size takes t = 1 and skips the boost"""
        R = self.R
        obj = 0x7000a000
        for fixed in (0.0, 1.0):
            for k in (0.0, 2.0, 0.5):
                for name, image in (('patched', self.patched), ('original', self.original)):
                    mu = self.machine(image, k, fixed)
                    fx = name == 'patched' and fixed
                    # boost flag set; the original skips it only by [x0+0x21] == 2, so the fixed case clears that guard
                    # (the veneer must skip it by the flag alone)
                    mu.mem_write(obj + 0x21, bytes([0 if fx else 2]))
                    mu.mem_write(obj + 0x26, bytes([0x80]))
                    mu.reg_write(R.UC_ARM64_REG_X0, obj)
                    mu.reg_write(R.UC_ARM64_REG_X14, 512)                 # lo
                    self.fs(mu, 20, 2048.0), self.fs(mu, 18, 0.25)         # hi, t
                    mu.reg_write(R.UC_ARM64_REG_X10, 0x3333)
                    mu.reg_write(R.UC_ARM64_REG_Q31, 0x0123456789abcdef0011223344556677)
                    mu.emu_start(0x683594, 0x68360c, count=64)
                    kk = k if name == 'patched' and k else 1.0
                    t = 1.0 if fx else 0.25
                    want = (int(int(512 + (2048 - 512) * t) * kk) + 7) & ~7
                    self.assertEqual(mu.reg_read(R.UC_ARM64_REG_X19) & 0xffffffff, want, (name, fixed, k))
                    self.assertEqual(mu.reg_read(R.UC_ARM64_REG_PC), 0x68360c, (name, fixed, k))
                    if name == 'patched':
                        self.assertEqual((mu.reg_read(R.UC_ARM64_REG_X10), mu.reg_read(R.UC_ARM64_REG_Q31),
                                          mu.reg_read(R.UC_ARM64_REG_SP), mu.reg_read(R.UC_ARM64_REG_NZCV)),
                                         (0x3333, 0x0123456789abcdef0011223344556677, 0x70004000, 0x60000000))


class ReportguardTest(unittest.TestCase):
    """reportguard on the real 1.4.3 main under Unicorn: one object through the inlined report-row loop
    (0xa22bc .. its exit 0xa2350), patched against the original: a good handle (n up to 0x7fff) gives the same coverage,
    handle and registers; -1 is skipped by both; a negative handle or n >= 0x8000 is skipped by the patched loop
    (coverage 0, handle untouched), and #52's n (the float 0.05) faults the original."""
    LOOP, EXIT = 0xa22bc, 0xa2350
    ROWS, OBJ, ARR, THIS = 0x10000000, 0x20000000, 0x20001000, 0x20002000

    @classmethod
    def setUpClass(cls):
        if not v143_main():
            raise unittest.SkipTest('no 1.4.3 main in the work folder')
        from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, arm64_const as R
        cls.R, cls.Uc, cls.UcError, cls.modes = R, Uc, UcError, (UC_ARCH_ARM64, UC_MODE_ARM)
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'v143.toml')
            args = SimpleNamespace(old=None, new=v143_main(), version='1.4.3', out=out, report=None, diagnostic=False)
            assert port.main_port(args) == 0
            table = read_table(out)
        cls.original = bytes(load_main_image(v143_main(), table['game']))
        img = load_main_image(v143_main(), table['game'])
        apply(img, table, ['reportguard'])
        cls.patched = bytes(img)

    def run_loop(self, image, h, n, ref=0):
        """one object, handle h on side 1, row 2h's index n, row 2h+1 = {3, 5, 50, 100}, row 2n's total ref.
        Returns (coverage bits, handle, x17, w0, pc) or 'fault'."""
        R = self.R
        mu = self.Uc(*self.modes)
        mu.mem_map(0, (len(image) + 0xfff) & ~0xfff)
        mu.mem_write(0, image)
        mu.mem_map(self.ROWS - 0x100000, 0x300000)    # 1 MB before the rows, 2 MB from them (n up to 0xffff)
        mu.mem_map(self.OBJ, 0x3000)
        rows = self.ROWS
        if 0 <= h:
            mu.mem_write(rows + h * 32, struct.pack('<I', n))
            mu.mem_write(rows + (2 * h + 1) * 16, struct.pack('<4I', 3, 5, 50, 100))
        if 0 <= n < 0x10000:
            mu.mem_write(rows + n * 32, struct.pack('<I', ref))
        mu.mem_write(self.OBJ + 0x98, struct.pack('<hh', 0x1234, h))   # side 0's handle, side 1's
        mu.mem_write(self.OBJ + 0x9c, struct.pack('<f', 0.75))           # (zeroed by the loop first)
        mu.mem_write(self.ARR, struct.pack('<Q', self.OBJ))
        mu.mem_write(self.THIS + 0x122, bytes([1]))
        for reg, v in (('X9', self.ARR), ('X10', 8), ('X11', 1), ('X12', rows), ('X13', 0xffff), ('X19', self.THIS),
                       ('X8', 1280 * 720)):
            mu.reg_write(getattr(R, 'UC_ARM64_REG_' + reg), v)
        mu.reg_write(R.UC_ARM64_REG_S0, 0x3f733333)   # 0.95 (the threshold the loop keeps in s0)
        try:
            mu.emu_start(self.LOOP, self.EXIT, count=200)
        except self.UcError:
            return 'fault'
        cov, = struct.unpack('<I', mu.mem_read(self.OBJ + 0x9c, 4))
        hd, = struct.unpack('<h', mu.mem_read(self.OBJ + 0x9a, 2))
        return cov, hd, mu.reg_read(R.UC_ARM64_REG_X17), mu.reg_read(R.UC_ARM64_REG_X0) & 1, mu.reg_read(R.UC_ARM64_REG_PC)

    def test_good_handles_unchanged(self):
        for h, n, ref in ((1, 3, 0), (1, 3, 7), (0, 0, 2), (5, 0x7fff, 0), (5, 0x7fff, 9), (0x7fff, 1, 0)):
            got = self.run_loop(self.patched, h, n, ref)
            self.assertEqual(got, self.run_loop(self.original, h, n, ref), (h, n, ref))
            self.assertEqual(got[4], self.EXIT)
            self.assertNotEqual(got[1], h, 'a good handle is consumed (0xffff) as in the original')

    def test_none_handle(self):
        for image in (self.patched, self.original):
            self.assertEqual(self.run_loop(image, -1, 0)[:2], (0, -1))

    def test_bad_handles_skipped(self):
        for h, n in ((-2, 0), (-0x8000, 0), (1, 0x8000), (1, 0xffff), (1, 0x3d4ccccd), (1, 0xffffffff)):
            cov, hd, _, _, pc = self.run_loop(self.patched, h, n)
            self.assertEqual((cov, hd, pc), (0, h, self.EXIT), (h, n))
        self.assertEqual(self.run_loop(self.original, 1, 0x3d4ccccd), 'fault')   # public #52's n


if __name__ == '__main__':
    unittest.main()
