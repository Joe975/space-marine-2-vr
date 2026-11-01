import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "qualify_stereo_overlap.py"
spec = importlib.util.spec_from_file_location("overlap", SCRIPT)
overlap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlap)


def eye(index, x, offset):
    return {"eye": index, "render_id": 19,
            "world": [1,0,0,0, 0,1,0,0, 0,0,1,0, x,0,0,1],
            "projection": [1,0,0,0, 0,1,0,0, offset,0,0,1, 0,0,.1,0]}


class StereoOverlapTests(unittest.TestCase):
    def test_nonzero_ipd_and_asymmetric_projection(self):
        left, right = eye(0, -.03, .2), eye(1, .03, -.2)
        result = overlap.reproject(left, right, (600, 500), (1000, 1000), 2)
        self.assertAlmostEqual(result["pixel"][0], 385)
        self.assertAlmostEqual(result["pixel"][1], 500)
        self.assertAlmostEqual(result["world"][0], -.03)

    def test_depth_bounds_and_actual_outside_overlap(self):
        pair = {"submitted": True, "dispatch": 20, "eyes": [eye(0, -.03, .2), eye(1, .03, -.2)]}
        self.assertEqual(overlap.qualify(pair, (1000,1000), (100,400,150,600), (1,100))["classification"], "outside")
        valid = overlap.qualify(pair, (1000,1000), (500,400,600,600), (1,100))
        self.assertEqual(valid["classification"], "inside")
        self.assertAlmostEqual(valid["target_pixel_bounds"][0], 270)
        self.assertAlmostEqual(valid["target_pixel_bounds"][2], 399.7)

    def test_rotated_target_uses_world_and_full_projection(self):
        left, right = eye(0, 0, 0), eye(1, 0, 0)
        right["world"] = [0,0,-1,0, 0,1,0,0, 1,0,0,0, 0,0,0,1]
        result = overlap.reproject(left, right, (1000,500), (1000,1000), 4)
        self.assertAlmostEqual(result["pixel"][0], 0)
        self.assertAlmostEqual(result["target_w"], 4)

    def test_invalid_or_unjoined_pair_refused(self):
        pair = {"submitted": True, "dispatch":20, "eyes":[eye(0,0,0),eye(1,0,0)]}
        pair["eyes"][1]["render_id"] = 20
        with self.assertRaises(ValueError):
            overlap.qualify(pair,(1000,1000),(0,0,100,100),(1,10))
        with self.assertRaises(ValueError):
            overlap.reproject(eye(0,0,0),eye(1,0,0),(100,100),(1000,1000),0)
        with self.assertRaises(ValueError):
            overlap.inverse([0]*16)


if __name__ == "__main__":
    unittest.main()
