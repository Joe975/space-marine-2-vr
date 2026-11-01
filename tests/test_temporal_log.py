import importlib.util
import json
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('temporal', Path(__file__).parents[1] / 'scripts/check_temporal_log.py')
temporal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(temporal)


def fixture(previous_center=False, moving=False, lag=False, turning=False, step=0.1, reset_jump=False):
    lines, previous = [], None
    for i in range(6):
        shift = 0.032 if i % 2 else -0.032
        center = [i * step if moving else 0, 2, 3]
        if reset_jump and i >= 3:
            center[0] += 10
        angle = i * 0.1 if turning else 0
        right = [math.cos(angle), 0, math.sin(angle)]
        c = dict(t=10000 + i * 10000, frame=i, seq=i, shift=shift, result=0, reset=int(reset_jump and i == 3),
                 pos=[p + r * shift for p, r in zip(center, right)], right=right, up=[0, 1, 0],
                 fwd=[math.sin(angle), 0, -math.cos(angle)],
                 projection=[[1.2, 0, 0, 0], [0, 2.1, 0, 0], [0, 0, -0.0000004, 1], [0, 0, 0.1, 0]])
        h = temporal.history(c, previous, False, previous_center) if previous else [[float(a == b) for b in range(4)] for a in range(4)]
        e = dict(t=c['t'] - 100, seq=i, shift=shift, center=center, pos=c['pos'], right=c['right'])
        def emit(prefix, data):
            return prefix + ' '.join(f'{k}={json.dumps(v, separators=(",", ":"))}' for k, v in data.items())
        lines.append(emit('TemporalEngine: ', e))
        logged = {k: v for k, v in c.items() if k not in ('seq', 'shift')}
        if lag:
            logged['t'] += 10000
        logged['projection'] = sum(c['projection'], [])
        logged['history'] = sum(h, [])
        lines.append(emit('TemporalStreamline: ', logged))
        previous = c
    return '\n'.join(lines)


class TemporalTests(unittest.TestCase):
    def test_correct_eye_history_with_movement(self):
        result = temporal.check_log(fixture(moving=True), require_motion=True)
        self.assertEqual(result['consecutive_history_pairs'], 5)
        self.assertEqual(result['moving_pairs'], 5)
        self.assertLess(result['median_max_matrix_error']['eye_to_eye'], 1e-8)
        self.assertGreater(result['median_max_matrix_error']['previous_center'], 0.02)
        self.assertEqual(result['moving_pairs_with_error_below_0_005']['eye_to_eye'], 5)
        self.assertAlmostEqual(result['max_center_position_change'], 0.5)

    def test_rotation_only_counts_as_motion(self):
        result = temporal.check_log(fixture(turning=True), require_motion=True)
        self.assertEqual(result['moving_pairs'], 5)
        self.assertLess(result['max_center_position_change'], 1e-8)
        self.assertGreater(result['max_forward_change'], 0.4)
        self.assertLess(result['moving_median_max_matrix_error']['eye_to_eye'], 1e-8)

    def test_stationary_and_tiny_drift_fail_motion_gate(self):
        for trace in (fixture(), fixture(moving=True, step=0.0002)):
            with self.assertRaisesRegex(ValueError, 'No substantial camera'):
                temporal.check_log(trace, require_motion=True)
        result = temporal.check_log(fixture())
        self.assertIsNone(result['moving_median_max_matrix_error']['eye_to_eye'])

    def test_reset_jump_cannot_turn_tiny_drift_into_movement_evidence(self):
        trace = fixture(moving=True, step=0.0002, reset_jump=True)
        with self.assertRaisesRegex(ValueError, 'No substantial camera'):
            temporal.check_log(trace, require_motion=True)
        result = temporal.check_log(trace)
        self.assertLess(result['max_center_position_change'], 0.001)

    def test_bad_history_during_motion_is_not_hidden_by_stationary_statistics(self):
        result = temporal.check_log(fixture(previous_center=True, turning=True), require_motion=True)
        self.assertEqual(result['moving_pairs_with_error_below_0_005']['eye_to_eye'], 0)
        self.assertEqual(result['moving_pairs_with_error_below_0_005']['previous_center'], 5)

    def test_unshifted_previous_history_is_distinguishable(self):
        result = temporal.check_log(fixture(previous_center=True))
        self.assertLess(result['median_max_matrix_error']['previous_center'], 1e-8)
        self.assertGreater(result['median_max_matrix_error']['eye_to_eye'], 0.02)

    def test_one_submission_lag_is_paired_by_pose(self):
        result = temporal.check_log(fixture(previous_center=True, moving=True, lag=True))
        self.assertEqual(result['consecutive_history_pairs'], 5)
        self.assertLess(result['median_max_matrix_error']['previous_center'], 1e-8)

    def test_missing_trace_and_broken_alternation_fail(self):
        for text in ('', fixture().replace('seq=3 ', 'seq=4 '), fixture().replace('shift=0.032', 'shift=-0.032')):
            with self.assertRaises(ValueError):
                temporal.check_log(text)


if __name__ == '__main__':
    unittest.main()
