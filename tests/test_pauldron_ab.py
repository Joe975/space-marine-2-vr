import importlib.util
import json
import math
import tempfile
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("pauldron_ab", Path(__file__).resolve().parents[1] / "scripts" / "pauldron_ab.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def sample(t, yaw_degrees, twist_degrees, upm=0.9):
    """Body facing yaw_degrees (root z axis), shoulders 0.4 m either side of the neck, turned by
    twist_degrees with the left shoulder leading; eye 0.23 m ahead of and 0.32 m above the neck."""
    yaw = math.radians(yaw_degrees)
    forward = [math.sin(yaw), 0.0, math.cos(yaw)]
    right = [forward[2], 0.0, -forward[0]]  # character's right for this synthetic rig
    neck = [10.0, 2.0, -4.0]
    def at(f, r, u):
        return [neck[i] + upm * (f * forward[i] + r * right[i]) + (upm * u if i == 1 else 0) for i in range(3)]
    tw = math.radians(twist_degrees)
    left = at(0.4 * math.sin(tw), -0.4 * math.cos(tw), -0.05)
    rightb = at(-0.4 * math.sin(tw), 0.4 * math.cos(tw), -0.05)
    body = [{"name": "clavicleL", "native": at(0, -0.1, 0)}, {"name": "clavicleR", "native": at(0, 0.1, 0)},
            {"name": "shoulder_L", "native": left, "final": at(0, -0.4, -0.05)},
            {"name": "shoulder_R", "native": rightb, "final": at(0, 0.4, -0.05)}]
    return {"t": t, "type": "sample",
            "character": {"driven": True, "units_per_metre": upm, "root_z_axis": forward, "body": body,
                          "twist_degrees": {"native": -twist_degrees, "final": 0.0}, "lean_degrees": {"native": 9.0, "final": 0.0}},
            "camera": {"eye_centre": at(0.23, 0, 0.32)}}


class PauldronMeasureTests(unittest.TestCase):
    def test_measures_shoulder_ahead_of_eye_and_twist(self):
        for yaw in (0.0, 72.3, -150.0):
            m = module.measure(sample(1.0, yaw, 48.6))
            native, final = m["native"], m["final"]
            self.assertAlmostEqual(native["twist_deg"], 48.6, places=1)
            self.assertAlmostEqual(native["shoulder_L_forward_m"], 0.4 * math.sin(math.radians(48.6)) - 0.23, places=3)
            self.assertAlmostEqual(native["shoulder_L_right_m"], -0.4 * math.cos(math.radians(48.6)), places=3)
            self.assertAlmostEqual(final["twist_deg"], 0.0, places=1)
            self.assertAlmostEqual(final["shoulder_L_forward_m"], -0.23, places=3)
            self.assertGreater(final["shoulder_L_azimuth_deg"], 90, "squared: the left shoulder is behind the eye's lateral line")
            self.assertLess(native["shoulder_L_azimuth_deg"], 90)
            self.assertAlmostEqual(final["shoulder_L_up_m"], -0.37, places=3)

    def test_missing_body_or_eye(self):
        s = sample(1.0, 0, 30)
        s["camera"] = {}
        self.assertIsNone(module.measure(s))
        s = sample(1.0, 0, 30)
        s["character"]["body"] = s["character"]["body"][2:]
        self.assertIsNone(module.measure(s), "without clavicles there is no handedness reference")

    def test_nearest_driven_sample_per_snapshot(self):
        with tempfile.TemporaryDirectory() as d:
            trace = Path(d) / "t.jsonl"
            lines = [json.dumps(sample(t / 10, 0, 40 if t < 50 else 0), separators=(",", ":")) for t in range(100)]
            lines.insert(30, json.dumps({"t": 3.02, "type": "event", "event": "eye_snapshot", "detail": "before"}, separators=(",", ":")))
            lines.append(json.dumps({"t": 8.0, "type": "event", "event": "eye_snapshot", "detail": "after"}, separators=(",", ":")))
            trace.write_text("\n".join(lines) + "\n", encoding="utf-8")
            picked = module.snapshot_samples(trace)
            self.assertAlmostEqual(picked["before"]["t"], 3.0)
            self.assertAlmostEqual(module.measure(picked["before"])["native"]["twist_deg"], 40, places=1)
            self.assertAlmostEqual(module.measure(picked["after"])["native"]["twist_deg"], 0, places=1)


if __name__ == "__main__":
    unittest.main()
