"""The live check must tell a left-handed run from a right-handed one (scripts/check_left_handed.py)."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from check_left_handed import reading, samples, summarise, verdicts

LEFT, RIGHT = [-.3, 1, -.5], [.3, 1, -.5]


def frame(loadout, gun, sword, marked=True):
    character = {'left_solved': LEFT, 'right_solved': RIGHT, 'units_per_metre': 1,
                 'weapons': [{'name': 'Gun_pose', 'final': gun}, {'name': 'GUN_R', 'final': sword}]}
    return reading(character, {'loadout': loadout, 'left_handed': marked})


def run(*frames):
    return verdicts(summarise(frames), any(f and f['marked'] for f in frames))


class CheckTests(unittest.TestCase):
    def test_weapons_on_the_expected_wrists_pass(self):
        checks = run(*[frame('primary', LEFT, LEFT)] * 40, *[frame('secondary', RIGHT, LEFT)] * 40)
        self.assertTrue(all(passed for passed, _ in checks), checks)

    def test_a_right_handed_run_fails(self):
        checks = run(*[frame('primary', RIGHT, RIGHT, False)] * 40, *[frame('secondary', LEFT, RIGHT, False)] * 40)
        self.assertEqual([passed for passed, _ in checks], [False] * 5, checks)

    def test_a_sword_left_on_the_right_arm_fails(self):
        # The one thing the headless suites can't see: the game ignoring a rewritten GUN_R.
        checks = run(*[frame('primary', LEFT, RIGHT)] * 40, *[frame('secondary', RIGHT, RIGHT)] * 40)
        self.assertFalse(checks[2][0], checks)
        self.assertFalse(checks[4][0], checks)

    def test_a_loadout_never_reached_fails(self):
        checks = run(*[frame('primary', LEFT, LEFT)] * 40)
        self.assertFalse(checks[-1][0], checks)
        self.assertIn('too few frames', checks[-1][1])

    def test_a_last_line_cut_short_is_skipped(self):
        whole = json.dumps({'t': 1.0, 'type': 'sample', 'input': {'loadout': 'primary'}, 'character': {'driven': True}}, separators=(',', ':'))
        with tempfile.TemporaryDirectory() as work:
            trace = Path(work) / 'trace.jsonl'
            trace.write_text(whole + '\n' + whole[:40], encoding='utf-8')
            self.assertEqual([t for t, _, _ in samples(trace)], [1.0])

    def test_a_frame_without_a_final_pose_is_no_reading(self):
        self.assertIsNone(reading({'left_solved': LEFT, 'right_solved': RIGHT, 'weapons': []}, {}))


if __name__ == '__main__':
    unittest.main()
