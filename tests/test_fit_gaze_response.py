import importlib.util
import json
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("fit_gaze_response", Path(__file__).resolve().parents[1] / "scripts" / "fit_gaze_response.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def line(t, heading, pitch, rx, ry, armed=True, mode="body"):
    h, p = math.radians(heading), math.radians(pitch)
    forward = [-math.sin(h) * math.cos(p), math.sin(p), -math.cos(h) * math.cos(p)]
    return json.dumps({"t": t, "type": "sample", "input": {"armed": armed, "pad": {"rx": round(rx * 32767), "ry": round(ry * 32767)}},
                       "camera": {"mode": mode, "game_forward": forward}}, separators=(",", ":"))


def synthetic(rate_yaw, rate_pitch, stick_x, stick_y, frames=40, dt=1 / 30, latency=1, **kw):
    """A native camera turning right at rate_yaw while the stick is held (one poll late)."""
    lines, heading, pitch = [], 10.0, 0.0
    for i in range(frames):
        lines.append(line(i * dt, heading, pitch, stick_x, stick_y, **kw))
        if i >= latency - 1:
            heading -= rate_yaw * dt  # Turning right lowers the heading.
            pitch += rate_pitch * dt
    return lines


class FitTests(unittest.TestCase):
    def test_recovers_held_stick_rates(self):
        result = module.fit(module.rows_from(synthetic(24.4, 13.3, 0.7, 0.7) + synthetic(7.2, 4.0, -0.5, -0.5)))
        yaw = {p["stick"]: p["degrees_per_second"] for p in result["yaw"]}
        pitch = {p["stick"]: p["degrees_per_second"] for p in result["pitch"]}
        self.assertAlmostEqual(yaw[0.7], 24.4, delta=0.2)
        self.assertAlmostEqual(yaw[0.5], 7.2, delta=0.2)
        self.assertAlmostEqual(pitch[0.7], 13.3, delta=0.2)
        self.assertAlmostEqual(pitch[0.5], 4.0, delta=0.2)

    def test_disarmed_menu_and_cutscene_samples_are_excluded(self):
        self.assertEqual(module.fit(module.rows_from(synthetic(30, 0, 0.8, 0, armed=False)))["samples"], 0,
                         "a disarmed pad never reached the game, so it says nothing about the response")
        self.assertEqual(module.fit(module.rows_from(synthetic(30, 0, 0.8, 0, mode="scripted")))["samples"], 0)

    def test_truncated_final_line_is_skipped(self):
        lines = synthetic(24.4, 0, 0.7, 0)
        lines.append(lines[-1][:len(lines[-1]) // 2])  # The game was killed mid-write.
        result = module.fit(module.rows_from(lines))
        self.assertEqual(result["samples"], 40)
        self.assertAlmostEqual({p["stick"]: p["degrees_per_second"] for p in result["yaw"]}[0.7], 24.4, delta=0.2)

    def test_changing_stick_is_not_a_held_measurement(self):
        lines = [line(i / 30, 10 - i, 0, (i % 2) * 0.8, 0) for i in range(30)]
        self.assertEqual(module.fit(module.rows_from(lines))["yaw"], [])


if __name__ == "__main__":
    unittest.main()
