"""games/totk/patches/engine_map.py's hook policy: every GEN_MAIN hook is classified exactly once, a required drop
fails, a frame-time drop forces 30 fps, a memo's group goes all or none; the struct field table
(totk_fields_v<N>.inc) defines only what its map places.

    python -m games.totk.tests.test_engine_map
"""
import unittest

from games.totk.patches import engine_map as em


class PolicyTest(unittest.TestCase):
    def test_every_hook_classified_once(self):
        hooks = set(em.gen_main_lists()['hook'])
        classes = [set(em.HOOK_FAIL), set(em.HOOK_CAP30), set(em.HOOK_FREE)] + [set(m) for _, m in em.HOOK_GROUPS]
        seen = [h for c in classes for h in c]
        self.assertEqual(len(seen), len(set(seen)), 'a hook is in two classes')
        self.assertEqual(set(seen) - hooks, set(), 'classified but not in GEN_MAIN')
        self.assertEqual(hooks - set(seen), set(), 'a GEN_MAIN hook without a policy: add it to engine_map.py')

    def test_split_list_read(self):
        # the cold giants gen.py splits reach another version's map (porter.fwd), as --vlocal's do
        lists = em.gen_main_lists()
        self.assertIn(0x5346a0, lists.get('split', []))
        self.assertFalse(set(lists['split']) & set(lists['hook']), 'a hooked function is never split')

    def test_policy(self):
        hooks = em.gen_main_lists()['hook']
        kept, status, safe, why = em.hook_policy({}, hooks)
        self.assertEqual((sorted(kept), status, safe, why), (sorted(hooks), 'pass', [], {}))
        kept, status, safe, why = em.hook_policy({0x704b7c: 'changed'}, hooks)      # free
        self.assertEqual((status, safe, 0x704b7c in kept), ('pass', [], False))
        kept, status, safe, _ = em.hook_policy({0x7a83e4: 'changed'}, hooks)        # the speed fix: 30 fps
        self.assertEqual((status, safe, 0x7a83e4 in kept), ('pass', ['cap30'], False))
        _, status, _, why = em.hook_policy({0x83e3f8: 'changed'}, hooks)            # menus: refuse
        self.assertEqual(status, 'fail')
        self.assertIn('REQUIRED', why[hex(0x83e3f8)])
        kept, status, _, why = em.hook_policy({0x279a77c: 'changed'}, hooks)        # one mutator of the memo
        group = em.HOOK_GROUPS[0][1]
        self.assertEqual([g for g in group if g in kept], [])
        self.assertEqual(status, 'pass')
        self.assertIn('its group', why[hex(0xb6afc8)])

    def test_covered_hooks(self):
        # a version's covered cap30 hook drops without the cap; on 1.0.0 (no version) the same drop still caps
        hooks = em.gen_main_lists()['hook']
        for version, covered in em.HOOK_COVERED.items():
            self.assertTrue(set(covered) <= set(em.HOOK_CAP30), version)
            for off in covered:
                kept, status, safe, why = em.hook_policy({off: 'no confirmed place'}, hooks, version)
                self.assertEqual((status, safe, off in kept), ('pass', [], False))
                self.assertIn('covered on', why[hex(off)])
                self.assertEqual(em.hook_policy({off: 'no confirmed place'}, hooks)[2], ['cap30'])
        from games.totk.patches import sites_v143
        # each cover names what it needs; a patch set without it is refused by build.py (covers_unmet)
        needs = {hex(o): n for o, (n, _) in em.HOOK_COVERED['1.4.3'].items() if n}
        self.assertEqual(needs, {hex(0x7a83e4): 'realtime', hex(0x8f689c): '!fps60'})
        self.assertEqual(em.covers_unmet(needs, {'realtime', 'cloth'}), {})
        self.assertEqual(em.covers_unmet(needs, {'cloth'}), {hex(0x7a83e4): 'realtime'})
        self.assertEqual(em.covers_unmet(needs, {'realtime', 'fps60'}), {hex(0x8f689c): '!fps60'})
        # the patch set's 'features' are the resolved names (patches engine: resolve_features(engine_spec(...))), never
        # the literal 'default': a default 1.4.3 export meets every cover
        from games.totk.patches import engine as pe, resolve_features
        table = {'feature': [dict(name=n, default=d, needs=list(nd)) for n, (_, d, nd) in sites_v143.FEATURES.items()]}
        feats = resolve_features(pe.engine_spec('default', {f['name'] for f in table['feature']}, set(), set()), table)
        self.assertNotIn('default', feats)
        self.assertIn('realtime', feats)
        self.assertEqual(em.covers_unmet(needs, set(feats)), {})
        self.assertEqual(em.covers_unmet(needs, set(resolve_features('default,-realtime,-cloth', table))),
                         {hex(0x7a83e4): 'realtime'})
        # the cover holds only while 1.4.3's sites are what the entries say: realtime on by default, no fps60
        from games.totk.patches import sites_v143
        self.assertEqual(sites_v143.FEATURES['realtime'][1], True)
        if 0x8f689c in em.HOOK_COVERED['1.4.3']:
            self.assertNotIn('fps60', sites_v143.FEATURES, 'fps60 came to 1.4.3: 0x8f689c needs its pin, not a cover')


class BetaTest(unittest.TestCase):
    def test_beta_groups_cover_known_required_hooks_and_have_env(self):
        # a beta may drop only required hooks with a feature group; every group says how its feature is turned off
        self.assertTrue(set(em.BETA_GROUPS) <= set(em.HOOK_FAIL))
        self.assertTrue(set(em.BETA_GROUPS.values()) <= set(em.BETA_ENV))
        for g, env in em.BETA_ENV.items():
            self.assertTrue(env, g)


def body(fn):
    import hashlib
    import struct
    return hashlib.sha256(struct.pack('<II', fn, 0xd65f03c0)).hexdigest()


class PinTest(unittest.TestCase):
    class FakePorter:
        """version_port.Porter's pairing state: 1.0.0 functions 0x100 0x200 0x300, the new build's 0x1100 .. 0x1400;
        0x200 -> 0x1200 confirmed, 0x300 -> 0x1300 voted but unconfirmed."""
        def __init__(self):
            self.size_o = {0x100: 8, 0x200: 8, 0x300: 8}
            self.size_n = {0x1100: 8, 0x1200: 8, 0x1300: 8, 0x1400: 8}
            self.fwd, self.rev, self.how = {0x200: 0x1200, 0x300: 0x1300}, {0x1200: 0x200, 0x1300: 0x300}, {}

        def _norm(self, side, s):
            import numpy as np
            return np.array([s, 0xd65f03c0], dtype='<u4')

        # (anchored address pins: the new build's text at 0, 0x1300's two words complete an ADRP reference to 0x9000)
        new = type('img', (), {'text_off': 0})

        class an:
            import numpy as np
            ref_tgt = np.full(0x1500 // 4, -1, dtype=np.int64)
            ref_tgt[0x1304 // 4] = 0x9000

        def func_of(self, side, off):
            return next((s for s in self.size_n if s <= off < s + 8), None)

        def _slice(self, img, s, z):
            return s // 4, (s + z) // 4

        def confirmed(self, fo):
            return fo == 0x200

        def _pair(self, fo, fn, how):
            self.fwd[fo], self.rev[fn], self.how[fo] = fn, fo, how

    def test_places_and_keeps(self):
        p = self.FakePorter()
        pin = {'new': '0x1100', 'body': body(0x1100), 'why': 'callers + strings', 'checked': 'read'}
        pinned, problems = em.apply_pins(p, {'0x100': pin}, '1.4.3')
        self.assertEqual((pinned, problems), ({0x100: pin}, []))
        self.assertEqual((p.fwd[0x100], p.rev[0x1100], p.how[0x100]), (0x1100, 0x100, 'pinned'))

    def test_replaces_an_unconfirmed_pair(self):
        p = self.FakePorter()
        pinned, problems = em.apply_pins(p, {'0x100': {'new': '0x1300', 'body': body(0x1300), 'why': 'vtable slot 12', 'checked': 'read'}}, '1.4.3')
        self.assertEqual(problems, [])
        self.assertNotIn(0x300, p.fwd)                       # the voted pair that held the target is gone
        self.assertEqual(p.fwd[0x100], 0x1300)

    def test_refusals(self):
        p = self.FakePorter()
        _, problems = em.apply_pins(p, {'0x100': {'new': '0x1200', 'body': body(0x1200), 'why': 'x', 'checked': 'read'}}, '1.4.3')   # a confirmed pair's
        self.assertIn('already the pair of 0x200', problems[0])
        _, problems = em.apply_pins(p, {'0x100': {'new': '0x1104', 'why': 'x', 'checked': 'read'}}, '1.4.3')   # not a start
        self.assertIn('not a function start in 1.4.3', problems[0])
        _, problems = em.apply_pins(p, {'0x104': {'new': '0x1100', 'body': body(0x1100), 'why': 'x', 'checked': 'read'}}, '1.4.3')   # not a 1.0.0 start:
        self.assertEqual((problems, p.pinned_addrs), ([], {0x104: 0x1100}))   # an address pin (AddressPinTest), no pair
        self.assertNotIn(0x104, p.fwd)
        _, problems = em.apply_pins(p, {'0x100': {'new': '0x1100', 'body': body(0x1100), 'why': ' ', 'checked': 'read'}}, '1.4.3')   # no why
        self.assertIn('needs its why and its checked note', problems[0])
        _, problems = em.apply_pins(p, {'0x100': {'new': '0x1100', 'body': body(0x1100), 'why': 'x'}}, '1.4.3')
        self.assertIn('checked note', problems[0])                 # (no checked)
        _, problems = em.apply_pins(p, {'0x100': {'new': '0x1100', 'why': 'x', 'checked': 'read'}}, '1.4.3')   # no body: told the hash
        self.assertIn('has the masked body hash ' + body(0x1100), problems[0])
        _, problems = em.apply_pins(p, {'0x100': {'new': '0x1100', 'body': body(0x1400), 'why': 'x', 'checked': 'read'}}, '1.4.3')   # other
        self.assertIn('another build', problems[0])
        self.assertNotIn(0x100, p.fwd)                       # a refused pin pairs nothing
        p = self.FakePorter()
        _, problems = em.apply_pins(p, {'0x100': {'new': '0x1300', 'body': body(0x1300), 'why': 'x', 'checked': 'read'},
                                        '0x300': {'new': '0x1300', 'body': body(0x1300), 'why': 'y', 'checked': 'read'}}, '1.4.3')
        self.assertIn('already the pair of 0x300', problems[0])   # two pins, one target


class AddressPinTest(unittest.TestCase):
    def test_code_and_data(self):
        p = PinTest.FakePorter()
        pins = {'0x150': {'new': '0x1104', 'body': body(0x1100), 'why': 'a return address', 'checked': 'read'},
                '0x900': {'new': '0x9000', 'body': body(0x1300), 'why': 'a data slot', 'checked': 'read'}}
        pinned, problems = em.apply_pins(p, pins, '1.4.3')
        self.assertEqual((pinned, problems, p.pinned_addrs), ({}, [], {0x150: 0x1104, 0x900: 0x9000}))

    def test_refusals(self):
        p = PinTest.FakePorter()
        _, problems = em.apply_pins(p, {'0x150': {'new': '0x1204', 'body': body(0x1100), 'why': 'x', 'checked': 'r'}}, '1.4.3')
        self.assertIn('neither in nor referenced', problems[0])           # not in the anchor, not referenced
        _, problems = em.apply_pins(p, {'0x150': {'new': '0x1102', 'body': body(0x1100), 'why': 'x', 'checked': 'r'}}, '1.4.3')
        self.assertIn('instruction boundary', problems[0])               # a typo inside the right function
        _, problems = em.apply_pins(p, {'0x150': {'new': '0x1104', 'body': 'ab' * 32, 'why': 'x', 'checked': 'r'}}, '1.4.3')
        self.assertIn('0 function(s)', problems[0])                       # no function has that body: told the right one
        self.assertIn(body(0x1100), problems[0])
        self.assertEqual(p.pinned_addrs, {})


class FieldEvidenceTest(unittest.TestCase):
    """a shipped field applies only when its evidence holds on that image"""
    class P:
        """1.0.0 function 0x100 reads [x0, #0x40]; its 1.4.3 pair 0x1100 reads [x0, #0x48]; 0x200 is unpaired"""
        def __init__(self, new_ldr=0xb9404801, confirmed=True):
            import numpy as np
            self.old = type('img', (), {'text_off': 0, 'words': np.zeros(0x300 // 4, dtype='<u4')})
            self.new = type('img', (), {'text_off': 0, 'words': np.zeros(0x1200 // 4, dtype='<u4')})
            for img, base, ldr in ((self.old, 0x100, 0xb9404001), (self.new, 0x1100, new_ldr)):
                img.words[base // 4: base // 4 + 4] = [0xd503201f, ldr, 0xd503201f, 0xd65f03c0]
            self.old.words[0x200 // 4 + 1] = 0xb9404001
            self.fwd, self.ok = {0x100: 0x1100}, confirmed
            self.size_o, self.size_n = {0x100: 16, 0x200: 16}, {0x1100: 16}

        def func_of(self, side, off):
            return next((s for s in ((0x100, 0x200) if side == 0 else (0x1100,)) if s <= off < s + 16), None)

        def confirmed(self, fo):
            return self.ok

        def _norm(self, side, s):
            img = self.old if side == 0 else self.new
            return img.words[s // 4: s // 4 + 4]

    F = {'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x104']}}}

    def test_decode(self):
        self.assertEqual(em.mem_imm(0xb9404001)[1], 0x40)                 # ldr w1, [x0, #0x40]
        self.assertEqual(em.mem_imm(0xf9400421)[1], 8)                    # ldr x1, [x1, #8]
        self.assertEqual(em.mem_imm(0xb81f0021)[1], -16)                  # stur w1, [x1, #-16]
        self.assertEqual(em.mem_imm(0x91010021)[1], 0x40)                 # add x1, x1, #0x40
        self.assertEqual(em.mem_imm(0x29485a61)[1], 0x40)                 # ldp w1, w22, [x19, #0x40]
        self.assertEqual(em.mem_imm(0xa9444ff4)[1], 0x40)                 # ldp x20, x19, [sp, #0x40]
        self.assertEqual(em.mem_imm(0xad7f7ffe)[1], -32)                  # ldp q30, q31, [sp, #-32]
        self.assertIsNone(em.mem_imm(0xd503201f))

    def test_holds(self):
        self.assertEqual(em.gated_fields(self.F, self.P(), {}), ({'OptionSelect': {'0x40': '0x48'}}, []))

    def test_refusals(self):
        _, r = em.gated_fields(self.F, self.P(new_ldr=0xb9404c01), {})          # the aligned access reads +0x4c
        self.assertIn('offset 0x4c, not 0x48', r[0])
        _, r = em.gated_fields(self.F, self.P(new_ldr=0xb9404821), {})          # same offset, another base register
        self.assertIn('not built by an add', r[0])                              # (only the re-based form could take it)

    def test_stretch_with_aligned(self):
        """two pieces in one function: an aligned one and a rewritten-stretch one agree -> held; the stretch one alone
        -> refused; the report says which kind each was"""
        p = self.P()
        ldr40, ldr48, nop, ret = 0xb9404001, 0xb9404801, 0xd503201f, 0xd65f03c0
        p.old.words[0x100 // 4: 0x100 // 4 + 4] = [ldr40, nop, ldr40, ret]
        p.new.words[0x1100 // 4: 0x1100 // 4 + 5] = [ldr48, nop, 0x52800020, ldr48, ret]
        p._norm = lambda side, s: (p.old if side == 0 else p.new).words[s // 4: s // 4 + (4 if side == 0 else 5)]
        p.size_n = {0x1100: 20}
        rep = {}
        both = {'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x100', '0x108']}}}
        self.assertEqual(em.gated_fields(both, p, {}, rep), ({'OptionSelect': {'0x40': '0x48'}}, []))
        self.assertEqual(rep['OptionSelect+0x40'], [['0x100', 'aligned'], ['0x108', 'stretch']])
        alone = {'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x108']}}}
        self.assertIn('second agreeing piece', em.gated_fields(alone, p, {})[1][0])

    def test_idiom_unique(self):
        """the constant idiom holds only when the access + its next 3 instructions occur once in each function"""
        ldr, mov, movk, tst, nop, ret = 0xf9401688, 0xd2a00109, 0xf2c00049, 0xea09011f, 0xd503201f, 0xd65f03c0
        p = self.P()
        p.old.words[0x100 // 4: 0x100 // 4 + 6] = [nop, ldr, mov, movk, tst, ret]
        # 1.4.3: another first word (unaligned) and the idiom with renamed registers (x9 base / x10 temp)
        idi = [0xf9401509, 0xd2a0010a, 0xf2c0004a, 0xea0a013f]
        p.new.words[0x1100 // 4: 0x1100 // 4 + 7] = [0x52800020, 0x52800041] + idi + [ret]
        p.size_o, p.size_n = {0x100: 24, 0x200: 16}, {0x1100: 28}
        p._norm = lambda side, s: (p.old if side == 0 else p.new).words[s // 4: s // 4 + (6 if side == 0 else 7)]
        f = {'Player': {'0x28': {'new': '0x28', 'for': ['0x104']}}}
        self.assertEqual(em.gated_fields(f, p, {})[1], [])
        p.new.words[0x1100 // 4: 0x1100 // 4 + 9] = idi + [nop] + idi
        p.size_n = {0x1100: 36}
        p._norm = lambda side, s: (p.old if side == 0 else p.new).words[s // 4: s // 4 + (6 if side == 0 else 9)]
        self.assertIn('not unique', em.gated_fields(f, p, {})[1][0])       # twice in 1.4.3: refused

    def test_register_offset_base(self):
        """'reg' / 'mov': an identical register-offset word holds only when its base register is set the same way in
        both builds"""
        ldr, nop, ret, add8 = 0xb86a6909, 0xd503201f, 0xd65f03c0, 0x91002268   # ldr w9, [x8, x10]; add x8, x19, #8
        p = self.P()
        p.old.words[0x100 // 4: 0x100 // 4 + 4] = [nop, nop, ldr, ret]
        p.new.words[0x1100 // 4: 0x1100 // 4 + 4] = [nop, nop, ldr, ret]
        f = {'FlagSet': {'0x0': {'new': '0x0', 'for': ['0x108']}}}
        rep = {}
        self.assertEqual(em.gated_fields(f, p, {}, rep)[1], [])
        self.assertEqual(rep['FlagSet+0x0'], [['0x108', 'reg']])
        p.new.words[0x1100 // 4 + 1] = add8                                     # the near miss: base + 8 in 1.4.3
        self.assertIn('base register is set differently', em.gated_fields(f, p, {})[1][0])

    def test_transfer_register_renamed(self):
        """an aligned access whose only difference is the transfer register is exact evidence; a pair is not"""
        p = self.P(new_ldr=0xb9404809)                                          # ldr w9, [x0, #0x48]: rt renamed
        self.assertEqual(em.gated_fields(self.F, p, {})[1], [])
        p = self.P(new_ldr=0xb9404c09)                                          # renamed, another offset
        self.assertIn('offset 0x4c, not 0x48', em.gated_fields(self.F, p, {})[1][0])

    def test_rewritten_stretch(self):
        """the access inside a stretch the update rewrote to another length: the one same access in it counts"""
        p = self.P()
        p.new.words[0x1100 // 4: 0x1100 // 4 + 5] = [0x52800020, 0x52800041, 0xb9404801, 0xd503201f, 0xd65f03c0]
        p._norm = lambda side, s: (p.old if side == 0 else p.new).words[s // 4: s // 4 + (4 if side == 0 else 5)]
        p.size_n = {0x1100: 20}
        rep = {}
        self.assertIn('second agreeing piece', em.gated_fields(self.F, p, {}, rep)[1][0])   # a lone stretch match
        self.assertEqual(rep, {'OptionSelect+0x40': [['0x104', 'stretch']]})
        f2 = {'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x104', '0x204']}}}   # + a piece in another function
        p.fwd[0x200] = 0x1100
        self.assertEqual(em.gated_fields(f2, p, {})[1], [])
        del p.fwd[0x200]
        p.new.words[0x1100 // 4] = 0xb9404c01                                    # a second same access: ambiguous
        self.assertIn('ambiguous', em.gated_fields(self.F, p, {})[1][0])
        _, r = em.gated_fields(self.F, self.P(confirmed=False), {})            # no confirmed pair, no pin
        self.assertIn('no confirmed pair or passing pin', r[0])
        self.assertEqual(em.gated_fields(self.F, self.P(confirmed=False), {0x100: {}})[1], [])   # a passing pin does
        _, r = em.gated_fields({'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x104', '0x204']}}}, self.P(), {})
        self.assertIn('0x200 has no confirmed pair', r[0])                   # all listed evidence must hold
        _, r = em.gated_fields({'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x108']}}}, self.P(), {})
        self.assertIn('not an access at 0x40', r[0])


def _mains():
    import os
    work = os.environ.get('NXRECOMP_WORK') or r'C:\nxrecomp-work'
    old, new = (os.path.join(work, 'extracted', v, 'exefs', 'main') for v in ('v100', 'v143'))
    return (old, new) if os.path.isfile(old) and os.path.isfile(new) else None


class RenamedFormTest(unittest.TestCase):
    """the renamed form: a read-only ("ro") field whose 1.4.3 function was rewritten with its registers renamed; exactly
    one access of the class at the offset in each whole function, offsets >= 0x10, a second piece from another
    function"""
    LDR40, NOP, RET = 0xb9404001, 0xd503201f, 0xd65f03c0          # ldr w1, [x0, #0x40]
    LDR48_W9_X3 = 0xb9404869                                       # ldr w9, [x3, #0x48]

    def porter(self, new_words, confirmed=True):
        p = FieldEvidenceTest.P(confirmed=confirmed)
        n = len(new_words)
        p.new.words[0x1100 // 4: 0x1100 // 4 + n] = new_words
        p.size_n = {0x1100: 4 * n}
        p._norm = lambda side, s: (p.old if side == 0 else p.new).words[s // 4: s // 4 + (4 if side == 0 else n)]
        p.fwd[0x200] = 0x1100          # a second 1.0.0 function paired with it: the other piece's function
        return p

    def rewritten(self, *extra):
        return [0x52800020, 0x52800041, self.LDR48_W9_X3, *extra, self.NOP, self.RET]

    def test_holds_with_a_piece_in_another_function(self):
        f = {'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x104', '0x204'], 'ro': True}}}
        rep = {}
        self.assertEqual(em.gated_fields(f, self.porter(self.rewritten()), {}, rep),
                         ({'OptionSelect': {'0x40': '0x48'}}, []))
        self.assertEqual(rep['OptionSelect+0x40'], [['0x104', 'renamed'], ['0x204', 'renamed']])

    def test_refusals(self):
        one = {'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x104'], 'ro': True}}}
        self.assertIn('second agreeing piece', em.gated_fields(one, self.porter(self.rewritten()), {})[1][0])
        written = {'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x104', '0x204']}}}   # no "ro": strict forms only
        self.assertIn('ambiguous', em.gated_fields(written, self.porter(self.rewritten()), {})[1][0])
        two = self.rewritten(0xb9404862)                                                   # + ldr w2, [x3, #0x48]
        self.assertIn('2 accesses', em.field_evidence(self.porter(two), 0x104, 0x40, 0x48, {}, renamed_ok=True))
        wide = [0x52800020, 0x52800041, 0xf9402469, self.NOP, self.RET]                   # ldr x9, [x3, #0x48]
        self.assertIn('0 accesses', em.field_evidence(self.porter(wide), 0x104, 0x40, 0x48, {}, renamed_ok=True))
        unpinned = self.porter(self.rewritten(), confirmed=False)
        self.assertIn('no confirmed pair', em.field_evidence(unpinned, 0x104, 0x40, 0x48, {}, renamed_ok=True))
        low = self.porter([0x52800020, 0x52800041, 0xb9400869, self.NOP, self.RET])        # ldr w9, [x3, #8]
        low.old.words[0x104 // 4] = 0xb9400801                                             # ldr w1, [x0, #8]
        self.assertIn('below 0x10', em.field_evidence(low, 0x104, 0x8, 0x8, {}, renamed_ok=True))

    def test_schema(self):
        ok = {'OptionSelect': {'0x40': {'new': '0x48', 'for': ['0x104'], 'ro': True}}}
        self.assertEqual(em.check_fields(ok, structs={'OptionSelect'}), [])
        for bad in ({'new': '0x48', 'for': ['0x104'], 'ro': False}, {'new': '0x48', 'for': ['0x104'], 'rw': True}):
            self.assertTrue(em.check_fields({'OptionSelect': {'0x40': bad}}, structs={'OptionSelect'}))

    @unittest.skipUnless(_mains(), 'no 1.0.0 / 1.4.3 mains in the work folder')
    def test_move_box_cursor_on_mains(self):
        """1.0.0's moveBoxCursor 0x2230100 and 1.4.3's 0x28f8d0c (rewritten, registers renamed): the control's active
        node load [x22, #0x58] is [x8, #0x58] there; the strict forms refuse it, the renamed form takes it"""
        import sys
        sys.path.insert(0, str(em.ROOT / 'tools'))
        import version_port as vp
        porter = vp.Porter(*(vp.load_image(p) for p in _mains()))
        porter._pair(0x2230100, 0x28f8d0c, 'pinned')
        pinned = {0x2230100}
        self.assertIsNotNone(em.field_evidence(porter, 0x2230154, 0x58, 0x58, pinned))
        kinds = []
        self.assertIsNone(em.field_evidence(porter, 0x2230154, 0x58, 0x58, pinned, kinds, renamed_ok=True))
        self.assertEqual(kinds, ['renamed'])
        self.assertIsNotNone(em.field_evidence(porter, 0x2230154, 0x58, 0x60, pinned, renamed_ok=True))   # near miss


@unittest.skipUnless(_mains(), 'no 1.0.0 / 1.4.3 mains in the work folder')
class CommittedTablesOnMainsTest(unittest.TestCase):
    """the committed pins_v143.json / fields_v143.json on the real mains: every pin passes, every field's evidence
    holds. Seconds: the porter's pairing state without its vote rounds, so a field whose evidence sits in a function
    only the vote rounds confirm is left to NXR_FULL_PORTER=1 (the whole porter, minutes: run it before committing
    such a field)."""
    def test_pins_and_fields_hold(self):
        import os
        import sys
        sys.path.insert(0, str(em.ROOT / 'tools'))
        import version_port as vp
        porter = vp.Porter(*(vp.load_image(p) for p in _mains()))
        full = os.environ.get('NXR_FULL_PORTER') == '1'
        if full:
            porter.run()
        pins, fields, problems = em.shipped_maps('1.4.3')
        self.assertEqual(problems, [])
        pinned, refused = em.apply_pins(porter, pins, '1.4.3')
        self.assertEqual(refused, [])
        if not full:   # (only the fields every piece of which sits in a pinned function)
            fields = {st: {k: v for k, v in offs.items()
                           if all(porter.func_of(0, int(x, 16)) in pinned for x in v['for'])} for st, offs in fields.items()}
        fmap, refused = em.gated_fields(fields, porter, pinned)
        self.assertEqual(refused, [])
        self.assertEqual(sum(len(v) for v in fmap.values()), sum(len(v) for v in fields.values()))
        # the vk hook's flag-set table: 1.0.0 reads [set, #0x10] (x8 = base + 0x30), 1.4.3 the same word as
        # [base, #0x40] (x9): equal offsets would say 'moved to 0x40', which is false; another base register refuses
        self.assertIn('not 0x30 + 0x40', em.field_evidence(porter, 0x1ccf8c8, 0x10, 0x40, pinned))
        # the re-based form reads it right: 1.4.3 still builds base + 0x30, and folds 0x30 + 0x10 into [base, #0x40]
        self.assertIsNone(em.field_evidence(porter, 0x1ccf8c8, 0x10, 0x10, pinned))

    def test_narrow_forms_on_mains(self):
        """the three narrow forms on the real mains, each with a near miss refused: the constant idiom (Player+0x28:
        'ldr x, [b, #0x28]; mov #0x80000; movk #2, lsl 32; tst', unique in both key functions, all registers
        renamed), the re-based sum (FlagSet+0x10: base + 0x30 still built, [base, #0x40] = 0x30 + 0x10), the
        mov-constant offset (PouchMgr+0x1e64: 'mov w9, #0x1e64; ldrb w10, [x8, x9]' in the pouch flag setter), and
        a register-offset read with a computed index at +0 (FlagSet+0x0)"""
        import sys
        sys.path.insert(0, str(em.ROOT / 'tools'))
        import version_port as vp
        porter = vp.Porter(*(vp.load_image(p) for p in _mains()))
        pins, _, _ = em.shipped_maps('1.4.3')
        pinned, _ = em.apply_pins(porter, pins, '1.4.3')
        porter.fwd[0x1acf574] = 0x218213c                      # (a confirmed pair of the full porter's)
        porter.confirmed = lambda fo: fo == 0x1acf574
        for at, off, kind in ((0x1ccfde0, 0x28, 'idiom'), (0x1ccf8c8, 0x10, 'rebased'), (0x1ccf930, 0x0, 'reg'),
                              (0x1acf658, 0x1e64, 'mov')):
            kinds = []
            self.assertIsNone(em.field_evidence(porter, at, off, off, pinned, kinds), hex(at))
            self.assertEqual(kinds, [kind])
            self.assertIsNotNone(em.field_evidence(porter, at, off, off + 8, pinned), hex(at))   # the near miss


class ShippedTest(unittest.TestCase):
    """pins_v<NNN>.json / fields_v<NNN>.json ship: numbers and our own short notes only."""
    PIN = {'new': '0xf23d70', 'body': 'ab' * 32, 'why': 'same callers and the same two tables',
           'checked': 'totk_pc_vk.cpp read against 1.4.3'}

    def test_pins_schema(self):
        self.assertEqual(em.check_pins({'0x1ccf894': self.PIN}), [])
        bad = {'0x1ccf894': dict(self.PIN, bytes='d65f03c0')}                         # any other field
        self.assertIn('needs exactly', em.check_pins(bad)[0])
        draft = {'0x1ccf894': {k: v for k, v in self.PIN.items() if k != 'body'}}
        self.assertIn('needs exactly', em.check_pins(draft)[0])                   # shipped: the body is required
        self.assertEqual(em.check_pins(draft, draft=True), [])                     # a draft --pins may leave it out
        for note, why in (('x' * 201, 'longer'), ('see 0123456789abcdef01', 'hex run'), ('a::b', 'game name'),
                          ('_ZN4gsys', 'game name'), ('', 'empty'), ('caf\u00e9', 'ASCII')):
            got = em.check_pins({'0x1': dict(self.PIN, why=note)})
            self.assertTrue(got and why in got[0] or why == 'ASCII' and got, (note, got))
        self.assertIn('64 hex', em.check_pins({'0x1': dict(self.PIN, body='ab' * 12)})[0])
        self.assertIn('key is not', em.check_pins({'1ccf894': self.PIN})[0])
        self.assertIn('new is not', em.check_pins({'0x1': dict(self.PIN, new='f23d70')})[0])

    def test_fields_schema_and_gate(self):
        f = {'Player': {'0x10d8': {'new': '0x10e0', 'for': ['0x1ccf894']}}, 'Actor': {'0x220': {'new': '0x228', 'for': ['0xf6092c']}}}
        self.assertEqual(em.check_fields(f), [])
        self.assertIn('not a struct our sources name', em.check_fields({'Newname': f['Player']})[0])
        self.assertTrue(em.check_fields({'Player': {'0x10d8': '0x10e0'}}))           # the flat private form does not ship
        self.assertTrue(em.check_fields({'Player': {'0x10d8': {'new': '0x10e0', 'for': []}}}))

    def test_committed_files_hold(self):
        """the committed tables pass this schema and the export gate's (tools/release/export_public.py)"""
        import sys
        pins, fields, problems = em.shipped_maps('1.4.3')
        self.assertEqual(problems, [])
        sys.path.insert(0, str(em.ROOT / 'tools' / 'release'))
        import export_public as ep
        files = {f'games/totk/patches/{n}': (None, (em.ROOT / 'games' / 'totk' / 'patches' / n).read_bytes(), None)
                 for n in ('pins_v143.json', 'fields_v143.json')}
        files['LEGAL.md'] = (None, b'tables for later game versions', None)
        # the engine sources the export ships with them: a fields entry's struct must be one of their names
        for p in (em.ROOT / 'engine' / 'runtime').rglob('*'):
            if (p.suffix in ('.cpp', '.h', '.c', '.inc') and p.is_file()
                    and not p.name.startswith(('totk_main_v', 'totk_fields_v'))):   # (private generated tables)
                files[p.relative_to(em.ROOT).as_posix()] = (None, p.read_bytes(), None)
        self.assertEqual(ep.version_pin_tables(files), [])
        import json
        files['games/totk/patches/pins_v143.json'] = (None, json.dumps({'0x1ccf894': self.PIN}).encode(), None)
        files['games/totk/patches/fields_v143.json'] = (None, json.dumps(
            {'Player': {'0x10d8': {'new': '0x10e0', 'for': ['0x1ccf894']}}}).encode(), None)
        files['engine/runtime/x.cpp'] = (None, b'ld32(p + totk_field(Player, 0x10d8));', None)   # (the gate's name source)
        self.assertEqual(em.check_pins({'0x1ccf894': self.PIN}), [])
        self.assertEqual(ep.version_pin_tables(files), [])             # filled tables pass the gate too

    def test_eight_digit_word(self):
        self.assertTrue(em.check_pins({'0x1': dict(self.PIN, why='word d65f03c0 here')}))


class FieldTest(unittest.TestCase):
    def test_pattern(self):
        src = 'ld64(a + totk_field(Actor, 0x220)); if (totk_field_known( Player ,0x10d8 )) x = 1; totk_field(B, 0X1F);'
        self.assertEqual([m.groups() for m in em.FIELD.finditer(src)], [('Actor', '0x220'), ('Player', '0x10d8')])

    def test_fields_inc(self):
        fields = [('Actor', '0x220'), ('Player', '0x10d8')]
        text, unplaced = em.fields_inc(fields, {'Actor': {'0x220': '0x228'}}, '1.4.2')
        self.assertIn('#define TF_Actor_0x220 0x228\n', text)
        self.assertNotIn('TF_Player', text)                 # unplaced: undefined, the build stops at its use
        self.assertEqual(unplaced, [('Player', '0x10d8')])
        text, unplaced = em.fields_inc(fields, {'Actor': {'0x220': '0x228'}}, '1.4.2', diagnostic=True)
        self.assertIn('#define TF_Player_0x10d8 0x7ffffff0u\n', text)
        self.assertIn('#define TF_Actor_0x220 0x228\n', text)
        self.assertEqual(unplaced, [('Player', '0x10d8')])


class SafeModeTest(unittest.TestCase):
    def test_export_env(self):
        """cap30 reaches the export's runtime_env (what boot.py and the package read), 1.0.0 is unchanged."""
        import json
        import tempfile
        from pathlib import Path
        from games.totk.patches import engine, read_table, TABLE_V100
        v100 = read_table(TABLE_V100)
        self.assertEqual(engine.safe_mode(TABLE_V100, v100), [])
        self.assertEqual(engine.runtime_env(['realtime']), {'TOTK_SPEED_FIX': '0'})
        with tempfile.TemporaryDirectory() as d:
            table = Path(d) / 'v143.toml'
            other = dict(v100, game=dict(v100['game'], version='1.4.3'))
            with self.assertRaises(engine.EngineError):                       # no engine map next to it
                engine.safe_mode(table, other)
            (Path(d) / 'v143.engine.json').write_text(json.dumps(dict(safe_mode=['cap30'])))
            safe = engine.safe_mode(table, other)
        self.assertEqual(safe, ['cap30'])
        self.assertEqual(engine.runtime_env(['realtime'], safe),
                         {'TOTK_SPEED_FIX': '0', 'TOTK_LOAD_FPS': '0', 'TOTK_TITLE_FPS': '0', 'TOTK_FPS': '30',
                          'TOTK_UNLOCK_FPS': '0'})


if __name__ == '__main__':
    unittest.main()
