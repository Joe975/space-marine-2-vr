import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("check_marker_log", Path(__file__).resolve().parents[1] / "scripts" / "check_marker_log.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def sample(caller, frame, eye, ui="0.6,0.4", scene="0.6,0.4", error="0,0", depth="2"):
    return f"WorldMarkerProjection: caller={caller} render_id={frame} eye={eye} ui_uv=[{ui}] scene_uv=[{scene}] error=[{error}] depth={depth}\n"


class MarkerLogTests(unittest.TestCase):
    def setUp(self):
        self.good = "".join(sample(caller, frame, eye) for caller in ("0xbf3d8c", "0xbf497a") for frame, eye in ((8, 0), (9, 1)))

    def test_real_projection_contract(self):
        result = module.check(self.good)
        self.assertTrue(result["passed"])
        self.assertEqual(result["samples"], 4)
        self.assertEqual(result["eyes"], [0, 1])

    def test_coordinate_error_cannot_hide_behind_zero_reported_error(self):
        bad = self.good + sample("0xbf3d8c", 10, 0, ui="0.7,0.4")
        self.assertFalse(module.check(bad)["passed"])

    def test_missing_eye_path_or_duplicate_samples_fail(self):
        for bad in ("", self.good.replace("eye=1", "eye=0"), self.good.replace("0xbf497a", "0xbf3d8c"), sample("0xbf3d8c", 8, 0) * 10):
            self.assertFalse(module.check(bad)["passed"])

    def test_invalid_numerics_depth_and_frame_fail(self):
        for field in ({"ui": "nan,0.4"}, {"depth": "0"}, {"error": "-0.1,0"}):
            self.assertFalse(module.check(self.good + sample("0xbf3d8c", 10, 0, **field))["passed"])
        self.assertFalse(module.check(self.good + sample("0xbf3d8c", 0, 2))["passed"])


if __name__ == "__main__":
    unittest.main()
