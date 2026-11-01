import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("analyze_melee_trace", Path(__file__).resolve().parents[1] / "scripts" / "analyze_melee_trace.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

HEADER = {"t": 0, "type": "header", "format": "sm2vr-trace", "version": 1, "hz": 30, "wall_clock_local": "2026-10-20T18:02:56"}
DT = 1 / 30
HEAD = [0.0, 1.7, 0.0]
UPM = 0.9


def sample(t, right=(0.2, 1.3, -0.4), left=(-0.3, 0.9, 0.0), guard=False, zone=0, trigger=0.0, root=(0.0, 0.0, 0.0), feet=None):
    """One sample: standing-space hands pointing forward (-Z), character root facing +Z."""
    feet = feet or {"FOOT1L": (-0.15, 0.27, 0.0), "FOOT1R": (0.15, 0.27, 0.0)}
    world = lambda p: [root[0] + p[0] * UPM, root[1] + p[1] * UPM, root[2] + p[2] * UPM]
    return {"t": round(t, 4), "type": "sample",
            "input": {"head": HEAD, "left": list(left), "right": list(right), "left_forward": [0, 0, -1], "right_forward": [0, 0, -1],
                      "guard": {"held": guard, "in_zone": guard}, "raw_controls": {"right_trigger": trigger}, "holster_zone": zone},
            "character": {"root": list(root), "root_z_axis": [0, 0, 1], "units_per_metre": UPM,
                          "body": [{"name": n, "native": world(p)} for n, p in feet.items()]}}


def event(t, kind, **fields):
    return {"t": round(t, 4), "type": "event", "event": kind, **fields}


def press(t, name, ms):
    return [event(t, "button", name=name, down=True), event(t + ms / 1000, "button", name=name, down=False)]


def lines(records):
    return [json.dumps(r) for r in sorted(records, key=lambda r: r["t"])]


class MeleeTraceTests(unittest.TestCase):
    def build(self):
        records = [HEADER]
        t = 0.0
        for k in range(200):  # 6.7 s at 30 Hz
            t = k * DT
            right, left, guard, zone, trigger, root, feet = (0.2, 1.3, -0.4), (-0.3, 0.9, 0.0), False, 0, 0.0, (0.0, 0.0, 0.0), None
            if 0.9 <= t <= 1.0:  # right swing: 10 cm per sample across the chest, then the native lunge
                right = (0.2 + (t - 0.9) * 3.0, 1.3, -0.4)
            if 1.0 < t <= 2.0:
                root = (0.0, 0.0, min(t - 1.0, 0.6) * 10 * UPM)
            if 1.0 < t <= 1.2:
                zone = 2  # follow-through reaches the hip zone
            if 2.0 <= t <= 2.6:
                guard = True
            if 2.3 <= t <= 2.45:  # left punch from the guard, the right controller barely twists
                left = (-0.2, 1.4, -0.3 - (t - 2.3) * 6.0)
                right = (0.2 + (t - 2.3) * 0.6, 1.3, -0.4)
            if 4.0 <= t:
                root = (0.0, 0.0, 6.0)
            if 4.0 <= t <= 4.15:  # the power swing moves the hand too
                right = (0.2 + (t - 4.0) * 2.0, 1.3, -0.4)
            if 4.0 <= t <= 4.6:
                trigger = 1.0
            if 4.4 <= t <= 4.7:  # kick: left foot 1.7 m up, 1.3 m in front
                feet = {"FOOT1L": (-0.2, 1.7, 1.3), "FOOT1R": (0.15, 0.27, 0.0)}
            if 5.6 <= t <= 5.8:  # a dodge roll throws the feet over the head: not a kick
                feet = {"FOOT1L": (-0.2, 1.8, 0.8), "FOOT1R": (0.15, 1.9, 0.6)}
            records.append(sample(t, right, left, guard, zone, trigger, root, feet))
        records += [event(0.967, "swing", speed=3.0, travel=0.2)] + press(0.967, "RB_melee", 44)
        records += [event(2.0, "guard_start", hand=[-0.2, -0.1, 0.35], angle_degrees=80)] + [event(2.0, "button", name="LB_parry_block", down=True)]
        records += [event(2.4, "button", name="LB_parry_block", down=False), event(2.4, "swing", speed=2.2, travel=0.2)] + press(2.4, "RB_melee", 154)
        records += [event(2.554, "button", name="LB_parry_block", down=True), event(2.6, "button", name="LB_parry_block", down=False)]
        records += [event(4.1, "swing", speed=2.0, travel=0.2)] + press(4.1, "up_power_melee", 156)
        records += press(5.5, "A_dodge_confirm", 150)
        return module.analyse(lines(records))

    def test_each_swing_names_hand_kind_and_native_response(self):
        report = self.build()
        light, punch, power = report["swings"]
        self.assertEqual((light["lead"], light["native"], light["pulse_ms"]), ("right", "light", 44))
        self.assertTrue(light["short_pulse_in_holster_zone"], "a 44 ms pulse that ends in the hip zone is flagged")
        self.assertGreater(light["root_travel_metres"], 5, "the native lunge is measured in metres")
        self.assertEqual(punch["lead"], "left", "a fast left hand with a barely moving right one is left-led")
        self.assertTrue(punch["guard_held"] and punch["lb_released_for_swing"])
        self.assertEqual(punch["lb_repressed_after_ms"], 154, "LB pressed again right after the pulse without a new guard is a bounce")
        self.assertEqual((power["native"], power["right_trigger"]), ("power", 1.0))
        self.assertIsNotNone(power["kick_after_s"], "the kick follows the power press")
        self.assertIsNone(light["kick_after_s"])

    def test_kicks_exclude_dodge_rolls_and_list_preceding_inputs(self):
        kicks = self.build()["kicks"]
        self.assertEqual(len(kicks), 1)
        self.assertEqual(kicks[0]["foot"], "FOOT1L")
        self.assertAlmostEqual(kicks[0]["height"], 1.7, places=2)
        self.assertIn("up_power_melee", [p["name"] for p in kicks[0]["preceding"]])

    def test_left_fast_motion_reports_what_it_produced(self):
        motions = self.build()["left_fast_motions"]
        punches = [m for m in motions if m["peak"] > 4]
        self.assertEqual(len(punches), 1)
        self.assertEqual(punches[0]["swings"], [2.4])
        self.assertIn("RB_melee", punches[0]["presses"])

    def test_counts_and_markdown(self):
        report = self.build()
        self.assertEqual(report["counts"]["swings"], 3)
        self.assertEqual(report["counts"]["left_led"], 1)
        self.assertEqual(report["counts"]["lb_repressed"], 1)
        text = module.markdown(report)
        self.assertIn("| 2.400 | left |", text)
        self.assertIn("Kick", text)

    def test_new_rules_verdicts(self):
        verdicts = [v["now"] for v in self.build()["new_rules"]]
        self.assertEqual(verdicts, ["light", "nothing (left hand led)", "power"])

    def test_new_rules_flick_cooldown_and_resting_finger(self):
        rows = [{"t": 1.0, "native": "light", "right_trigger": 0.08, "speeds": {"left": {"wrist": 0.1, "tip": 0.1}, "right": {"wrist": 3, "tip": 4}}},
                {"t": 1.3, "native": "power", "right_trigger": 1.0, "speeds": {"left": {"wrist": 0.1, "tip": 0.1}, "right": {"wrist": 3, "tip": 4}}},
                {"t": 3.0, "native": "light", "right_trigger": 0.0, "speeds": {"left": {"wrist": 0.1, "tip": 0.1}, "right": {"wrist": 0.3, "tip": 2}}}]
        samples = []
        for k in range(150):
            t = k * DT
            x = 0.2 + min(max(t - 0.9, 0), 0.2) * 2 if t < 2 else 0.6  # the wrist moves for the first two, not the third
            samples.append(module.compact(sample(t, right=(x, 1.3, -0.4))))
        verdicts = [v["now"] for v in module.new_rule_verdicts(samples, rows)]
        self.assertEqual(verdicts, ["light", "nothing (cooldown)", "nothing (wrist flick)"], "a resting finger (0.08) is light; 300 ms later is inside the cooldown")

    def test_guard_replay_entry_angle_and_pistol_latch(self):
        def row(t, forward, angle, trigger):
            return {"t": t, "guard_hand": [-0.2, -0.3, forward], "guard_angle": angle, "left_trigger": trigger}
        rows = [row(0.0, 0.6, 55, 1.0), row(0.1, 0.6, 55, 0.0), row(0.2, 0.6, 55, 1.0), row(0.3, 0.6, 55, 0.0),  # pistol shots, arm out
                row(0.4, 0.9, 0, 0.0), row(0.5, 0.2, 80, 0.0), row(0.6, 0.5, 80, 0.0), row(0.7, 0.9, 0, 0.0)]  # a real guard drifting out
        events = [{"t": 0.1, "event": "button", "name": "RT_fire", "down": False}, {"t": 0.3, "event": "button", "name": "RT_fire", "down": False}]
        old = module.replay_guard(rows, events, module.OLD_GUARD, aim_latch=False)
        new = module.replay_guard(rows, events, module.NEW_GUARD)
        self.assertEqual([s["t"] for s in old], [0.1, 0.3, 0.5], "old rules parry between shots")
        self.assertEqual([s["t"] for s in new], [0.5], "new rules: only the guard raised near the face, held while it drifts")

    def test_arbitration_events_are_counted(self):
        records = [HEADER, sample(0), sample(DT), event(0.02, "swing_vetoed", speed=2.0, other_hand_speed=5.8), event(0.03, "guard_yield")]
        report = module.analyse(lines(records))
        self.assertEqual((report["counts"]["left_hand_vetoes"], report["counts"]["guard_yields"]), (1, 1))
        self.assertEqual(report["vetoes"][0]["other_hand_speed"], 5.8)


if __name__ == "__main__":
    unittest.main()
