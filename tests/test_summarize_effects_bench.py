import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('summarize', Path(__file__).resolve().parents[1] / 'scripts/summarize_effects_bench.py')
summarize = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summarize)


def window(label, pairs, gpu_ms, mode='stereo'):
    return {'label': label, 'mode': mode, 'metrics': {'fresh_pairs_per_s': pairs, 'frame_ms': {'median': 26.0},
            'gpu': {'gpu_busy_ms_median': gpu_ms, 'gpu_busy_fraction': .5}, 'threads': {'render_cpu_fraction': .7}}}


class SummaryTests(unittest.TestCase):
    def test_deltas_are_per_pack_against_mean_baseline(self):
        rows = [summarize.row('bloom', window(l, p, g)) for l, p, g in
                (('baseline', 34, 18), ('all', 37, 15), ('baseline#2', 36, 16))]
        rows += [summarize.row('all', window('baseline', 40, 10)), summarize.row('all', window('flares', 41, 9.5))]
        out = summarize.with_deltas(rows)
        self.assertEqual([(r['pack'], r['step'], r['d_pairs'], r['d_gpu_ms']) for r in out],
                         [('bloom', 'baseline', -1, 1), ('bloom', 'all', 2, -2), ('bloom', 'baseline#2', 1, -1),
                          ('all', 'baseline', 0, 0), ('all', 'flares', 1, -0.5)])

    def test_table_marks_mono_and_missing_values(self):
        rows = summarize.with_deltas([summarize.row('default', window('mono', 47, None, 'mono'))])
        text = summarize.table(rows)
        self.assertIn('| default | mono (mono) | 47.0 | - | 26.0 | - | - | 50% | 70% |', text)


if __name__ == '__main__':
    unittest.main()
