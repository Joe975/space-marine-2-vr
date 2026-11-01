import importlib.util
import json
import math
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("driver", ROOT / "scripts/vr_test_driver.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class DriverTests(unittest.TestCase):
    def test_grenade_head_looks_keep_physical_belt_fixed_and_cancel_slowly(self):
        script = json.loads((ROOT / 'config/test_driver/grenade_belt_head_looks.json').read_text())
        self.assertEqual(script['mode'], 'scripted_head')
        self.assertEqual(script['duration_ms'], 24000)
        for phase, yaw in enumerate([0, 90, 180, -90]):
            rows = script['frames'][phase * 12:(phase + 1) * 12]
            self.assertEqual(rows[1]['left_grip'], 1)
            self.assertNotIn('left_grip', rows[5])
            self.assertEqual(rows[7]['left_grip'], 1)
            self.assertNotIn('left_grip', rows[10])
            for row in rows:
                self.assertEqual(row['head'][3], yaw)
                self.assertFalse(set(row) - {'ms', 'head', 'left', 'right', 'left_grip'})
            # Driver hands are relative to the yaw-only head: compensate them so
            # only the gaze changes while each acquisition point stays by the waist.
            x, y, z = rows[1]['left'][:3]
            r = math.radians(yaw)
            self.assertAlmostEqual(x * math.cos(r) + z * math.sin(r), -.1, places=5)
            self.assertAlmostEqual(-x * math.sin(r) + z * math.cos(r), .025, places=5)
            self.assertEqual(y, -.65)
            self.assertGreater(rows[3]['ms'] - rows[2]['ms'], 800)

    def test_hand_visibility_sweep_keeps_pose_and_rotates_without_locomotion(self):
        script=json.loads((ROOT/'config/test_driver/hands_50cm_rotation.json').read_text())
        self.assertEqual(script['mode'],'scripted_head')
        self.assertEqual(script['duration_ms'],120000)
        self.assertEqual([f['head'][3] for f in script['frames'][::2]],[0,45,90,135,180,-135,-90,-45,0])
        for f in script['frames']:
            self.assertEqual(f['left'],[-.18,-.1,-.5,0,0,0])
            self.assertEqual(f['right'],[.18,-.1,-.5,0,0,0])
            self.assertEqual(f['head'][:3],[0,0,0])
            self.assertFalse(set(f)-{'ms','head','left','right'})

    def test_view_displacement_uses_camera_handedness_not_rolling_actor(self):
        a = {"character": {"root": [0, 0, 0], "units_per_metre": .5},
             "camera": {"age_ms": 0, "anchored": True, "body_yaw": 180}}
        b = {"character": {"root": [2, 0, 0]}}
        self.assertAlmostEqual(driver.view_displacement(a, b)["right_metres"], 4)
        self.assertAlmostEqual(driver.view_displacement(a, b)["forward_metres"], 0)
        a["camera"]["body_yaw"] = 90
        self.assertAlmostEqual(driver.view_displacement(a, b)["forward_metres"], 4)
        a["camera"]["anchored"] = False
        self.assertIsNone(driver.view_displacement(a, b))

    def test_production_scenarios_compile_and_release_omitted_actions(self):
        for path in (ROOT / "config/test_driver").glob("*.json"):
            text = driver.compile_script(json.loads(path.read_text()), "scenario")
            self.assertTrue(text.startswith("SM2VR_TEST 1 scenario "))
            for row in text.splitlines()[1:]:
                self.assertEqual(len(row.split()), 30)
        text = driver.compile_script({"duration_ms": 500, "frames": [{"ms": 0, "buttons": ["dodge"]}, {"ms": 200}]}, "release")
        self.assertEqual(text.splitlines()[1].split()[1], "1")
        self.assertEqual(text.splitlines()[2].split()[1], "0")

    def test_invalid_inputs_never_replace_mailbox(self):
        cases = [{"duration_ms": 120001, "frames": [{"ms": 0}]},
                 {"duration_ms": 100, "frames": [{"ms": 0, "head": [1, 0, 0, 0, 0, 0]}]},
                 {"duration_ms": 100, "frames": [{"ms": 0, "move": [float("nan"), 0]}]},
                 {"duration_ms": 100, "frames": [{"ms": 0, "buttons": ["typo"]}]},
                 {"duration_ms": 100, "frames": [{"ms": 0}, {"ms": 0}]},
                 {"duration_ms": 100, "frames": [{"ms": 0, "left": [3, 0, 0, 0, 0, 0]}]}]
        for script in cases:
            with self.subTest(script=script), self.assertRaises(ValueError):
                driver.compile_script(script, "bad")

    def test_atomic_request_replaces_previous_request(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            driver.write_request(directory, "STOP one\n")
            driver.write_request(directory, "STOP two\n")
            self.assertEqual((directory / "request.txt").read_text(), "STOP two\n")
            self.assertEqual([p.name for p in directory.iterdir()], ["request.txt"])

    def test_report_measures_native_outcome_instead_of_requested_direction(self):
        records = [{"type": "event", "event": "test_driver", "t": 1, "detail": "abc running "}]
        for t, root in ((1, [0, 0, 0]), (3, [0, 0, 2])):
            records.append({"type": "sample", "t": t, "character": {"age_ms": 0, "actor": "one", "root": root,
                            "root_z_axis": [0, 0, 1], "units_per_metre": 1}, "metrics": {"head_offset_metres": [0, 0, -.3]}})
        records.append({"type": "event", "event": "button", "t": 1.1, "name": "A_dodge_confirm", "down": True})
        records.append({"type": "event", "event": "test_driver", "t": 4, "detail": "abc completed "})
        result = driver.report_trace([json.dumps(r) for r in records], "abc")
        self.assertEqual(result['terminal'], 'completed')
        self.assertEqual(result['root_displacement_initial_body_axes']['forward_metres'], 2)
        self.assertEqual(result['root_displacement_initial_body_axes']['right_metres'], 0)
        self.assertEqual(len(result['dodge_windows']), 1)
        self.assertEqual(result['dodge_windows'][0]['actual']['forward_metres'], 2)


if __name__ == "__main__":
    unittest.main()
