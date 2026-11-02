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


class BetaTest(unittest.TestCase):
    def test_beta_groups_cover_known_required_hooks_and_have_env(self):
        # a beta may drop only required hooks with a feature group; every group says how its feature is turned off
        self.assertTrue(set(em.BETA_GROUPS) <= set(em.HOOK_FAIL))
        self.assertTrue(set(em.BETA_GROUPS.values()) <= set(em.BETA_ENV))
        for g, env in em.BETA_ENV.items():
            self.assertTrue(env, g)


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
