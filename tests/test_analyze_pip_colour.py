import importlib.util
from pathlib import Path
import unittest
import copy

spec = importlib.util.spec_from_file_location("colour", Path(__file__).resolve().parents[1] / "scripts/analyze_pip_colour.py")
colour = importlib.util.module_from_spec(spec)
spec.loader.exec_module(colour)


class ColourAnalysisTests(unittest.TestCase):
    def test_both_scene_repeats_and_all_channels_must_improve(self):
        def report(error):
            return {"channels": {c: {"mae_bytes": {"identity": error}} for c in ("red", "green", "blue")}}
        reports = [report(20), report(6), report(24), report(8)]
        self.assertTrue(colour.repeatable_improvement(reports)["bounded_colour_improvement_pass"])
        changed = copy.deepcopy(reports)
        changed[3]["channels"]["blue"]["mae_bytes"]["identity"] = 15
        self.assertFalse(colour.repeatable_improvement(changed)["bounded_colour_improvement_pass"])
        changed[3]["channels"]["blue"]["mae_bytes"]["identity"] = float("nan")
        self.assertFalse(colour.repeatable_improvement(changed)["bounded_colour_improvement_pass"])
        with self.assertRaises(ValueError):
            colour.repeatable_improvement(reports[:3])
    def test_transfer_hypotheses_are_distinct(self):
        pairs = [(i / 255, colour.encode(i / 255)) for i in range(1, 254)]
        result = colour.fit(pairs)
        self.assertLess(result["mae_bytes"]["linear_to_srgb"], 1e-9)
        self.assertGreater(result["mae_bytes"]["identity"], 40)
        self.assertGreater(result["mae_bytes"]["linear_fit"], 5)
        for value in (0, .0031308, .1, .5, 1):
            self.assertAlmostEqual(colour.decode(colour.encode(value)), value, places=6)

    def test_linear_exposure_and_offset(self):
        result = colour.fit([(x / 255, x / 255 * .7 + .05) for x in range(255)])
        self.assertAlmostEqual(result["linear_gain"], .7)
        self.assertAlmostEqual(result["linear_bias_bytes"], 12.75)
        self.assertLess(result["mae_bytes"]["linear_fit"], 1e-9)

    def test_area_downsample_and_invalid_geometry(self):
        pip = (2, 2, bytes([10, 20, 30] * 4))
        main = (4, 4, bytes([20, 30, 40] * 16))
        result = colour.analyze(main, pip, (0, 0, 2, 2), 1)
        self.assertEqual(result["main_downsample"], 2)
        self.assertAlmostEqual(result["channels"]["red"]["mae_bytes"]["identity"], 10)
        with self.assertRaises(ValueError):
            colour.analyze((3, 4, b""), pip)
        with self.assertRaises(ValueError):
            colour.analyze(main, pip, (0, 0, 5, 5))

    def test_equal_output_uses_same_normalised_architecture_region(self):
        image = (3840, 2160, bytes([10, 20, 30]) * (3840 * 2160))
        result = colour.analyze(image, image)
        self.assertEqual(result["region"], [720, 270, 3120, 720])
        self.assertEqual(result["main_downsample"], 1)
        self.assertEqual(result["channels"]["red"]["samples"], 7600)
        self.assertEqual(result["channels"]["red"]["mae_bytes"]["identity"], 0)
