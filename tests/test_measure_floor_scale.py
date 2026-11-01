import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import measure_floor_scale as m  # noqa: E402


def bone(name, position):
    return {"name": name, "native": position, "final": position}


def line(head=1.65, hand=0.75, armed=True, takeover=False, mode="body", units=0.9):
    root = [10, -100, 20]
    marine = [bone("ARM1L", [9.7, -97.9, 20]), bone("ARM2L", [9.7, -98.3, 20]), bone("HANDL", [9.7, -98.7, 20]),
              bone("ARM1R", [10.3, -97.9, 20]), bone("ARM2R", [10.3, -98.3, 20]), bone("HANDR", [10.3, -98.7, 20])]
    target_y = root[1] + 2.4 + units * (hand - head)
    character = {"root": root, "animation_takeover": takeover, "body": marine,
                 "left_target": [9.7, target_y, 20], "right_target": [10.3, target_y, 20]}
    tracking = {"armed": armed, "menu": False, "head": [0, head, 0], "head_forward": [0, 0, -1],
                "left": [-0.7, hand, 0], "right": [0.7, hand, 0]}
    camera = {"mode": mode, "eye_centre": [10, root[1] + 2.4, 20], "units_per_metre": units}
    record = {"t": 1, "type": "sample", "input": tracking, "character": character, "camera": camera}
    return json.dumps(record, separators=(",", ":"))  # As the trace writes it.


class MeasureFloorScale(unittest.TestCase):
    def test_a_hand_on_the_real_floor_at_the_marines_knee(self):
        result = m.measure([line()] * 3 + [line(head=0.62, hand=0.0)] * 2)
        self.assertEqual(result["samples"], 3)
        self.assertEqual(result["eye_above_root"], 2.4)
        self.assertEqual(result["player_eye_height_metres"], 1.65)
        self.assertEqual(result["floor_above_root_predicted"], 0.915)
        self.assertEqual(result["floor_samples"], 4)  # Both hands, twice.
        self.assertAlmostEqual(result["floor_above_root"], 2.4 - 0.9 * 0.62, places=3)
        self.assertEqual(result["first_person_units_per_metre_for_floor"], 1.455)

    def test_the_floor_lands_on_the_ground_at_the_measured_scale(self):
        result = m.measure([line(units=1.455)])
        self.assertAlmostEqual(result["floor_above_root_predicted"], 0, places=2)

    def test_the_arms(self):
        result = m.measure([line()])
        self.assertEqual(result["marine_arm"], 0.8)
        self.assertEqual(result["marine_span"], 2.2)
        self.assertEqual(result["player_span_metres"], 1.4)
        self.assertEqual(result["player_span_game"], 1.26)
        self.assertGreater(result["reach"], 0)

    def test_skips_what_is_not_standing_first_person(self):
        lines = [line(armed=False), line(takeover=True), line(mode="takeover"), line(hand=0.0, armed=False), "not json",
                 '{"t":1,"type":"sample","input":null,"character":{},"head_forward":[0,0,-1]}']
        self.assertEqual(m.measure(lines), {"samples": 0})


if __name__ == "__main__":
    unittest.main()
