import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import capture_console_visibility as console
from capture_hand_visibility import capture_name, run
from vr_test_driver import compile_script


class ConsoleVisibilityTests(unittest.TestCase):
    def test_real_driver_request_stays_stationary_through_every_capture_window(self):
        script = json.loads(console.SCENARIO.read_text())
        lines = compile_script(script, 'console_regression').splitlines()
        self.assertIn('scripted_head 112000 16', lines[0])
        self.assertEqual(len(console.NAMES), len(set(console.NAMES)))
        for i, (yaw, pitch) in enumerate(console.POSES):
            a, b = script['frames'][i * 2:i * 2 + 2]
            self.assertEqual((a['ms'], b['ms']), (i * 14000, i * 14000 + 10000))
            for frame in (a, b):
                self.assertEqual(frame['head'], [0, 0, 0, yaw, pitch, 0])
                self.assertEqual(frame['left'], [-.25, -.6, 0, 0, 0, 0])
                self.assertEqual(frame['right'], [.25, -.6, 0, 0, 0, 0])
                self.assertEqual(set(frame), {'ms', 'head', 'left', 'right'})
            # No interpolation towards the next heading until after the whole
            # capture interval; no requested turn/attack/holster/locomotion.
            for elapsed in (i * 14000 + 3500, i * 14000 + 7999):
                status = dict(id='this', state='running', elapsed_ms=elapsed)
                self.assertEqual(capture_name(i, status, 'this', console.NAMES), console.NAMES[i])
            self.assertLess(i * 14000 + 8000, b['ms'])

    def test_wrong_request_state_and_transition_do_not_capture(self):
        status = dict(id='this', state='running', elapsed_ms=3500)
        for change in ({'id': 'other'}, {'state': 'focus_lost'}, {'state': 'completed'},
                       {'elapsed_ms': 3499}, {'elapsed_ms': 8000}):
            self.assertIsNone(capture_name(0, dict(status, **change), 'this', console.NAMES))
        for index in (-1, 8):
            self.assertIsNone(capture_name(index, status, 'this', console.NAMES))

    def test_reused_or_unsafe_capture_names_refused_before_queue_write(self):
        scratch = ROOT / 'build' / 'test_scratch'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as folder:
            output = Path(folder)
            for names in ((), console.NAMES + ('ninth',), ('same',) * 8,
                          ('../unsafe',) + console.NAMES[1:]):
                with self.assertRaises(ValueError):
                    run(output, output, console.SCENARIO, names)
            (output / console.NAMES[0]).mkdir()
            with self.assertRaises(ValueError):
                run(output, output, console.SCENARIO, console.NAMES)
            self.assertFalse((output / 'request.txt').exists())


if __name__ == '__main__':
    unittest.main()
