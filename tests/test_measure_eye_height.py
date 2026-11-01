import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import measure_eye_height as m  # noqa: E402


def line(height, armed=True, pitch=0.0):
    return json.dumps({"t": 1, "type": "sample", "input": {"armed": armed, "head": [0, height, 0],
                                                          "head_forward": [0, pitch, -1]}})


class MeasureEyeHeight(unittest.TestCase):
    def test_counts_only_armed_level_plausible_samples(self):
        lines = [line(1.62), line(1.64), line(1.63), line(1.63, armed=False), line(0.9),
                 line(1.63, pitch=-0.8), '{"t":1,"type":"sample","input":null}', "not json"]
        self.assertEqual(list(m.heights(lines)), [1.62, 1.64, 1.63])

    def test_median_ignores_a_short_crouch(self):
        values = [1.63] * 90 + [1.2] * 10
        self.assertEqual(m.summary(values)["median"], 1.63)

    def test_empty(self):
        self.assertEqual(m.summary([]), {"samples": 0})


if __name__ == "__main__":
    unittest.main()
