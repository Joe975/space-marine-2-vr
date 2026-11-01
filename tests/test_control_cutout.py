import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("control_cutout", ROOT / "scripts" / "control_cutout.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
FIXTURE = ROOT / "tests" / "fixtures" / "control_cutout_2026-10-26.txt"


def header():
    return json.dumps({"t": 0.0, "type": "header", "format": "sm2vr-trace", "version": 1, "hz": 30.0,
                       "wall_clock_local": "2026-10-26T18:00:00.000"})


def sample(t, armed=True, move=(0.0, 0.0), fire=0.0, parry=0.0, age=10.0, controls=True, loadout="primary"):
    i = {"age_ms": age, "epoch": 1, "armed": armed, "loadout": loadout, "guard": {"held": False}, "grenade": {"holding": False},
         "raw_controls": {"right_trigger": fire, "left_trigger": 0.0, "right_grip": 0.0, "left_grip": parry}}
    if controls:
        i["controls"] = {"move": [move[0], move[1], 0], "turn": 0.0, "fire": fire, "aim": 0.0, "right_grip": 0.9,
                         "left_grip": parry, "trackpad": 0.0, "fire_touch": False, "chainsword_rev": 0.0}
    return json.dumps({"t": round(t, 3), "type": "sample", "input": i})


def event(t, name, **fields):
    return json.dumps({"t": t, "type": "event", "event": name, **fields})


def armed(t, value, reason="awaiting neutral controls"):
    return event(t, "input_armed", armed=value, reason="active" if value else reason)


def run(dt=0.033):
    """10 s: a recenter at 1 s, a stereo pause at 3 s, a frame gap at 6 s, focus loss at 8 s."""
    lines = [header()]
    t = 0.0
    while t < 10:
        if 1.0 <= t < 1.4:
            lines.append(sample(t, armed=False, parry=0.8 if t < 1.2 else 0.0))
        elif 3.0 <= t < 5.0:
            lines.append(sample(t, armed=False, move=(0, 1), fire=0.9 if t > 4 else 0.0))
        elif 6.0 <= t < 6.5:
            lines.append(sample(t, armed=False, move=(0.5, 0.5)))
        elif 8.0 <= t < 8.5:
            lines.append(sample(t, armed=False, controls=False))
        elif 5.8 < t < 6.0:
            pass  # a 200 ms frame gap just before the 6 s window
        else:
            lines.append(sample(t, move=(0, 1)))
        t += dt
    lines += [event(1.0, "tracking_reset", epoch=2), event(1.0, "body_scale", reading="accepted"), armed(1.0, False), armed(1.4, True),
              armed(3.0, False), armed(5.0, True), armed(6.0, False), armed(6.5, True),
              armed(8.0, False, "game not foreground"), armed(8.5, True), '{"t": 9.9, "type": "sam']
    log = ["[18:00:02.850] [I] [tid:1] OrdinaryHeadsetRetention: ready=true stopped=false active=false pairs=0 refusal=0",
           "[18:00:02.990] [I] [tid:1] OrdinaryHeadsetResumed: dispatch=10 previous=6 accepted=true",
           "[18:00:00.100] [I] [tid:1] VrCombat: fire=0 aim=0 buttons=0x0 melee_enabled=false"]
    return lines, log


class ControlCutoutTests(unittest.TestCase):
    def test_windows_triggers_and_blockers(self):
        lines, log = run()
        report = module.analyse(lines, log)
        rows = report["windows"]
        self.assertEqual([r["trigger"] for r in rows], ["recenter", "stall", "stall", "controls unavailable"])
        self.assertEqual(rows[0]["start_wall"], "18:00:01.000")
        self.assertAlmostEqual(rows[1]["seconds"], 2.0)
        self.assertIn("stereo pairs paused 140 ms", rows[1]["detail"])
        self.assertIn("frame gap", rows[2]["detail"])
        self.assertEqual(rows[3]["detail"], "game not foreground")
        self.assertIn("parry", rows[0]["held"])
        self.assertGreater(rows[0]["untraced"], 0, "a sample with nothing traced held waits on a face button (Left A)")
        self.assertEqual(set(rows[1]["held"]), {"move", "fire"}, "the right grip only blocks with grip melee on")
        self.assertEqual(rows[1]["released"], ["move", "fire"])
        self.assertIn("stall 2", module.text(report))
        self.assertTrue(module.markdown(report).startswith("| Start | End |"))

    def test_named_cause_wins(self):
        lines = [header(), sample(0.0), event(0.5, "input_armed", armed=False, reason="active", cause="recenter"), sample(0.6, armed=False), armed(0.7, True)]
        self.assertEqual(module.analyse(lines)["windows"][0]["trigger"], "recenter")

    def test_fixture_columns(self):
        lines, log = run()
        report = module.analyse(lines, log)
        _, samples, _ = module.parse(lines)
        text = module.fixture(report, samples, "run/x/sm2vr_trace.jsonl", report["melee"])
        w = [l.split() for l in text.splitlines() if l.startswith("w ")]
        s = [l.split() for l in text.splitlines() if l.startswith("s ")]
        self.assertEqual([x[2] for x in w], ["recenter", "stall", "stall", "controls_unavailable"])
        self.assertTrue(all(len(x) == 17 for x in s), "s, index, time, 10 controls, untraced, two raw triggers, loadout")
        self.assertTrue(any(x[1] == "0" and x[13] == "1" for x in s), "untraced is marked inside the recenter window")
        self.assertEqual({x[16] for x in s}, {"p"})

    @unittest.skipUnless(FIXTURE.exists(), "replay fixture absent")
    def test_committed_fixture_shape(self):
        text = FIXTURE.read_text(encoding="utf-8").splitlines()
        windows = [l.split() for l in text if l.startswith("w ")]
        self.assertEqual(len(windows), 21)
        self.assertEqual(sum(1 for w in windows if w[2] == "stall"), 10)
        self.assertEqual(max(float(w[5]) for w in windows), 10.557, "the 18:48:07 window, 'How am I stuck?'")


if __name__ == "__main__":
    unittest.main()
