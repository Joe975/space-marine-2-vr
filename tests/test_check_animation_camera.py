import importlib.util
import json
import math
import tempfile
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("check_animation_camera", Path(__file__).resolve().parents[1] / "scripts" / "check_animation_camera.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

UPM = 0.9


def sample(t, mode, eye, root=None, game=None, upm=UPM, separation=None):
    camera = {"mode": mode, "eye_centre": list(eye), "units_per_metre": upm}
    if game is not None:
        camera["game"] = list(game)
    if separation is not None:
        camera["eye_separation"] = separation
    row = {"t": t, "type": "sample", "camera": camera}
    if root is not None:
        row["character"] = {"root": list(root), "units_per_metre": UPM}
    return row


def write(rows):
    handle = tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False, encoding="utf-8")
    with handle:
        handle.write(json.dumps({"t": 0, "type": "header"}) + "\n")
        for row in rows:
            handle.write(json.dumps(row) + "\n")
    return Path(handle.name)


class TakeoverTest(unittest.TestCase):
    def takeover(self, bob_eye=False, follow_root=False):
        rows = [sample(0.0, "body", (10, 2.3, 19.7), (10, 0, 20), (10, 2.3, 19.7))]
        for i in range(30):
            t = 0.1 + i / 30
            # The execution carries the character 1.5 units forward and 0.3 up.
            root = (10, 0.01 * i, 20 - i * 0.05)
            bob = 0.25 * UPM * math.sin(i)
            eye_y = 2.3 + (bob if bob_eye else 0)
            # Held on the first-person spot, as VrBodyFollowing places it (or dragged by the root).
            eye = (10, eye_y, root[2] - 0.3 if follow_root else 19.7)
            rows.append(sample(t, "takeover", eye, root, (root[0], 3.0 + bob, root[2] + 2.7)))
        return module.check(write(rows))

    def test_held_view_passes(self):
        report = self.takeover()
        self.assertEqual(report["failures"], [])
        result = report["takeovers"][0]
        self.assertAlmostEqual(result["drift_max"], 0, places=6)
        self.assertAlmostEqual(result["height_range"], 0, places=6)
        self.assertAlmostEqual(result["distance_max"], 1.15 / UPM, places=3)
        self.assertGreater(result["native_height_range"], 0.4)

    def test_bobbing_eye_fails(self):
        report = self.takeover(bob_eye=True)
        self.assertTrue(any("eye height moved" in failure for failure in report["failures"]))

    def test_eye_dragged_by_root_fails(self):
        report = self.takeover(follow_root=True)
        self.assertTrue(any("eye drifted" in failure for failure in report["failures"]))


class ControlBackTest(unittest.TestCase):
    """docs/TAKEOVER_RETURN_2026-10-26.md: when the game gave control back during a takeover."""

    def takeover(self, stick, walk):
        # The game's camera faces -Z, 3 m back; the Marine stands 1 s, then moves 6 m/s along walk.
        rows = [sample(0.0, "body", (0, 2.3, -0.3), (0, 0, 0), (0, 2.3, -0.3))]
        root = [0.0, 0.0, 0.0]
        for i in range(60):
            t = 0.05 + i / 30
            if t > 1.0:
                root = [root[0] + walk[0] * 6 * UPM / 30, 0.0, root[2] + walk[1] * 6 * UPM / 30]
            row = sample(t, "takeover", (0, 2.3, -0.3), root, (root[0], 3.0, root[2] + 2.7))
            row["camera"]["game_forward"] = [0, -0.2, -0.98]
            row["input"] = {"pad": {"lx": int(stick[0] * 32767), "ly": int(stick[1] * 32767)}}
            rows.append(row)
        rows.append(sample(2.1, "body", (root[0], 2.3, root[2] - 0.3), root, (root[0], 2.3, root[2] - 0.3)))
        return module.check(write(rows))["takeovers"][0]

    def test_walking_along_the_stick_is_control(self):
        result = self.takeover((0, 1), (0, -1))  # stick forward, the Marine walks along the camera's forward
        self.assertIsNotNone(result["control"])
        self.assertTrue(1.0 < result["control"] < 1.2, result["control"])
        self.assertAlmostEqual(result["held_after_control"], result["end"] - result["control"], places=6)

    def test_strafe_maps_through_the_game_camera(self):
        result = self.takeover((1, 0), (1, 0))  # stick right: +X for a camera facing -Z
        self.assertIsNotNone(result["control"])

    def test_motion_off_the_stick_is_not_control(self):
        self.assertIsNone(self.takeover((0, 1), (1, 0))["control"])  # a lunge across the stick
        self.assertIsNone(self.takeover((0, 0), (0, -1))["control"])  # stick neutral


class RideTest(unittest.TestCase):
    """Jump pack rides (camera.ride): the eye stays on the Marine while it flies."""

    def ride(self, left_behind=False):
        rows = [sample(0.0, "body", (0, 2.3, -0.3), (0, 0, 0))]
        for i in range(30):
            t = 0.1 + i / 30
            # 16 m/s across, the root rising 1 unit; the eye 0.3 units ahead of it (or left at the start).
            root = (0, i / 30, -i * 16 * UPM / 30)
            eye = (0, 2.3, -0.3) if left_behind else (0, root[1] + 2.3, root[2] - 0.3)
            row = sample(t, "body", eye, root)
            row["camera"].update({"ride": True, "travel_speed_metres_per_second": 16.0})
            rows.append(row)
        rows.append(sample(1.2, "body", (0, 2.3, -15), (0, 0, -14.7)))
        return module.check(write(rows))

    def test_eye_on_the_marine_passes(self):
        report = self.ride()
        self.assertEqual(report["failures"], [])
        self.assertEqual(len(report["rides"]), 1)
        result = report["rides"][0]
        self.assertAlmostEqual(result["speed_max"], 16.0)
        self.assertAlmostEqual(result["distance_max"], 0.3 / UPM, places=4)
        self.assertAlmostEqual(result["climb"], (29 / 30) / UPM, places=4)
        self.assertEqual(report["takeovers"], [])

    def test_eye_left_behind_fails(self):
        report = self.ride(left_behind=True)
        self.assertTrue(any("from the root" in failure for failure in report["failures"]))


class ViewerTest(unittest.TestCase):
    def test_grounded_scales_report_real_eye_height_and_ipd(self):
        floor, eye = 2.1, 1.7
        rows = [sample(0.0, "body", (0, floor + 2.3, 0), (0, floor, 0))]
        for upm in (0.9, 10.8, 0.225):
            for i in range(3):
                rows.append(sample(1 + upm + i * 0.01, "viewer", (5, floor + eye * upm, 5), separation=0.064 * upm, upm=upm))
        report = module.check(write(rows), eye_height=eye)
        self.assertEqual(report["failures"], [])
        scales = {s["units_per_metre"]: s for s in report["viewers"][0]["scales"]}
        for upm in (0.9, 10.8, 0.225):
            self.assertAlmostEqual(scales[upm]["baseline_metres"], 0.064, places=5)
            self.assertAlmostEqual(scales[upm]["eye_height_metres"], eye, places=4)

    def test_floating_viewer_fails(self):
        # 21 Oct: at 0.2 units per metre Joe's eye was ~2 units above the floor: about 10 m.
        floor = -10.0
        rows = [sample(0.0, "body", (0, floor + 2.3, 0), (0, floor, 0)),
                sample(1.0, "viewer", (0, floor + 2.13, 0), separation=0.064 * 0.2, upm=0.2)]
        report = module.check(write(rows), eye_height=1.65)
        self.assertTrue(any("above the floor" in failure for failure in report["failures"]))
        self.assertAlmostEqual(report["viewers"][0]["scales"][0]["eye_height_metres"], 10.65, places=2)


class CapturePlanTest(unittest.TestCase):
    def test_items_sort_and_validate(self):
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
        capture_spec = importlib.util.spec_from_file_location("capture_animcam_live", Path(__file__).resolve().parents[1] / "scripts" / "capture_animcam_live.py")
        capture = importlib.util.module_from_spec(capture_spec)
        capture_spec.loader.exec_module(capture)
        self.assertEqual(capture.parse(["after@16500", "control=takeover@3500", "fp@2500"]),
                         [(2500, "snapshot", "fp"), (3500, "control", "takeover"), (16500, "snapshot", "after")])
        with self.assertRaises(SystemExit):
            capture.parse(["control=sideways@100"])
        with self.assertRaises(SystemExit):
            capture.parse(["no_time"])


if __name__ == "__main__":
    unittest.main()
