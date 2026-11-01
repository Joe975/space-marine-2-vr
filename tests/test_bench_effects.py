import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('bench_effects', ROOT / 'scripts/bench_effects.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class PlanTests(unittest.TestCase):
    def test_modes_keep_light_union_and_shadow_refresh(self):
        steps = bench.plan(['baseline', 'ssr', 'all', 'baseline', 'mono'], 60, 15)
        self.assertEqual([s['label'] for s in steps], ['baseline', 'ssr', 'all', 'baseline#2', 'mono'])
        self.assertTrue(all(s['shading'].startswith('light_union_shadow_refresh') for s in steps))
        self.assertEqual([s['stereo_hold'] for s in steps], ['stereo'] * 4 + ['mono'])
        self.assertEqual(steps[4]['shading'], 'light_union_shadow_refresh')
        self.assertEqual({s['flare_hide'] for s in steps}, {'native'})
        hidden = bench.plan(['flares', 'everything'], 60, 15)
        self.assertEqual([(s['shading'], s['flare_hide']) for s in hidden],
                         [('light_union_shadow_refresh', 'hide'), ('light_union_shadow_refresh_all', 'hide')])

    def test_hzb_modes_set_the_control_and_only_mono_holds(self):
        steps = bench.plan(['hzb_off', 'hzb_left', 'hzb_native', 'mono', 'mono_hzb', 'no_refresh', 'baseline'], 60, 15)
        self.assertEqual([s['hzb'] for s in steps], ['off', 'left_only', 'native', 'off', 'left_only', 'off', 'off'])
        self.assertEqual([s['stereo_hold'] for s in steps], ['stereo'] * 3 + ['mono'] * 2 + ['stereo'] * 2)
        self.assertEqual(steps[5]['shading'], 'light_union')

    def test_shared_modes_keep_stereo(self):
        steps = bench.plan(['hzb_shared', 'hzb_objects'], 60, 15)
        self.assertEqual([(s['hzb'], s['stereo_hold']) for s in steps], [('shared', 'stereo'), ('shared_objects', 'stereo')])

    def test_hzb_values_are_ones_the_mod_accepts(self):
        header = (ROOT / 'mod/src/NativeHzbCulling.h').read_text()
        for value in set(bench.HZB.values()):
            self.assertIn(f'"{value}"', header)

    def test_shading_values_are_ones_the_mod_accepts(self):
        # Mirrors NativeOrdinaryShadingControl::parse; a typo there would silently fall back.
        header = (ROOT / 'mod/src/NativeOrdinaryShadingControl.h').read_text()
        for value in set(bench.MODES.values()):
            self.assertIn(f'"{value}"', header)

    def test_rejects_bad_input(self):
        for modes, hold, settle in ((['fog'], 60, 15), ([], 60, 15), (['ssr'], 121, 15), (['ssr'], 30, 30)):
            with self.assertRaises(ValueError):
                bench.plan(modes, hold, settle)

    def test_snapshot_names_are_safe_and_short(self):
        self.assertEqual(bench.snapshot_name('bloom', 'baseline#2'), 'fx_bloom_baseline_2')
        self.assertLessEqual(len(bench.snapshot_name('x' * 50, 'everything')), 40)

    def test_hold_script_is_valid_for_the_driver(self):
        script = bench.hold_script(-75, 0, 60)
        text = bench.driver.compile_script(script, 'bench_test')
        self.assertTrue(text.startswith('SM2VR_TEST 1 bench_test scripted_head 60000 2'))


if __name__ == '__main__':
    unittest.main()
