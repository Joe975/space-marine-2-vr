import importlib.util
import copy
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("analyze_pip_lens", Path(__file__).resolve().parents[1] / "scripts" / "analyze_pip_lens.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PixelShiftTests(unittest.TestCase):
    def test_native_cache_and_culling_evidence_with_mutations(self):
        near, far = .1, 100
        e, f = near / (near - far), near * far / (far - near)
        p = [1, 0, 0, 0, 0, 1, 0, 0, -.3, -.2, e, 1, 0, 0, f, 0]
        inv = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1 / f, .3, .2, 1, -e / f]
        combined = [1, 0, 0, 0, 0, 1, 0, 0, .3, .2, -e, -1, -4.5, -5, 5 * e + f, 5]
        world = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 3, 4, 5, 1]
        planes = [0, 0, -1, -4.9, 1, 0, -.7, -.5, -1, 0, -1.3, -9.5,
                  0, 1, -.8, 0, 0, -1, -1.2, -10, 0, 0, 1, -95]
        pip = {"pip": True, "projection_cache": p, "inverse_projection_cache": inv, "combined_cache": combined,
               "world": world, "rectangle": [-.07, -.08, .2, .2], "corners": [-.07, -.08, .13, -.08, .13, .12, -.07, .12],
               "planes": planes, "cull_radius": .3}
        directions = []
        for x, y in zip(pip["corners"][::2], pip["corners"][1::2]):
            length = math.sqrt(x * x + y * y + near * near)
            directions.extend((x / length, y / length, -near / length))
        sides = []
        for corner in range(4):
            current, following = directions[corner * 3:corner * 3 + 3], directions[((corner + 1) % 4) * 3:((corner + 1) % 4) * 3 + 3]
            sides.extend((current[1] * following[2] - current[2] * following[1], current[2] * following[0] - current[0] * following[2], current[0] * following[1] - current[1] * following[0]))
        pip.update(cull_directions=directions, cull_sides=sides, cull_distance=-near, cull_normal=[0, 0, 1], cull_origin=[3, 4, 5])
        main = {"pip": False, "projection_cache": [0] * 16}
        capture = {"native_dispatch": {"before": [main, pip], "after": [main, pip]}}
        self.assertEqual(module.geometry_errors(capture, .15, .1), [])
        for field, index, value, expected in (("inverse_projection_cache", 0, 2, "stale inverse projection"),
                                              ("combined_cache", 12, 9, "stale combined view/projection"),
                                              ("planes", 7, 20, "native culling disagrees with final lens")):
            changed = copy.deepcopy(capture)
            changed["native_dispatch"]["after"][1][field][index] = value
            self.assertIn(expected, module.geometry_errors(changed, .15, .1))
        changed = copy.deepcopy(capture)
        changed["native_dispatch"]["after"][1]["cull_radius"] = .1
        self.assertIn("near-corner bound is too small", module.geometry_errors(changed, .15, .1))
        changed = copy.deepcopy(capture)
        changed["native_dispatch"]["after"][1]["cull_distance"] = -.2
        self.assertIn("compact near cap differs from true near plane", module.geometry_errors(changed, .15, .1))
        changed["native_dispatch"]["after"][1]["cull_distance"] = float("nan")
        self.assertIn("invalid compact culling data", module.geometry_errors(changed, .15, .1))

    def test_real_pixel_translation_and_negative_control(self):
        width, height = 120, 90
        def pattern(x, y):
            return ((x * 7411 ^ y * 233 ^ (x * y * 17)) % 200) + 20
        original = bytes(pattern(x, y) for y in range(height) for x in range(width) for _ in range(3))
        for dx, dy in ((12, -8), (-12, 8)):
            shifted = bytes(pattern(x - dx, y - dy) for y in range(height) for x in range(width) for _ in range(3))
            found = module.pixel_shift((width, height, original), (width, height, shifted), (dx, dy), region=(25, 20, 95, 65), step=3)
            self.assertTrue(found["pass"])
            self.assertEqual(found["measured"], [dx, dy])
            wrong = module.pixel_shift((width, height, original), (width, height, original), (dx, dy), region=(25, 20, 95, 65), step=3)
            self.assertFalse(wrong["pass"])

    def test_mismatched_image_sizes_rejected(self):
        with self.assertRaises(ValueError):
            module.pixel_shift((10, 10, bytes(300)), (11, 10, bytes(330)), (1, 1))


if __name__ == "__main__":
    unittest.main()
