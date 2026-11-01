import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("replay_melee_trace", ROOT / "scripts" / "replay_melee_trace.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
TOOL = ROOT / "build" / "mod" / "melee_replay.exe"
DT = 1 / 30


def sample(t, right, loadout="secondary", left=(-0.3, 0.9, 0.0), trigger=0.0, grip=0.0):
    return json.dumps({"t": round(t, 4), "type": "sample", "input": {
        "epoch": 3, "armed": True, "loadout": loadout, "head": [0, 1.7, 0], "head_forward": [0, 0, -1],
        "right": list(right), "right_forward": [0, 0, -1], "left": list(left), "left_forward": [0, 0, -1],
        "raw_controls": {"right_trigger": trigger, "right_grip": grip, "left_trigger": 0.0}}})


def resting_then_swing(start_at_hip):
    """Rest, then a 0.4 m swing across the chest (or forward and up out of the hip)."""
    lines = []
    for k in range(60):
        t = k * DT
        f = min(max((t - 1.0) / 0.2, 0), 1)
        if start_at_hip:
            right = (0.25, 1.0 + 0.2 * f, -0.45 * f)
        else:
            right = (-0.1 + 0.4 * f, 1.3, -0.4)
        lines.append(sample(t, right))
    return lines


class ReplayTests(unittest.TestCase):
    def test_export_columns_and_loadout(self):
        rows = module.export([sample(0, (0.2, 1.3, -0.4)), sample(DT, (0.2, 1.3, -0.4), loadout="primary"), '{"t":1,"type":"event"}'])
        self.assertEqual(len(rows), 2)
        self.assertEqual(len(rows[0]["text"].split()), 7 + 6 + 7 + 7 + 2, "then the support grip and the left grip (the push needs an open hand)")
        self.assertEqual(rows[0]["text"].split()[3], "1")
        self.assertEqual(rows[1]["text"].split()[3], "0", "the primary loadout is code 0 (bolter swings have their own gate)")
        legacy = json.loads(sample(0, (0.2, 1.3, -0.4)))
        del legacy["input"]["loadout"]
        self.assertEqual(module.export([json.dumps(legacy)], assume_secondary=True)[0]["text"].split()[3], "1")

    def test_motions_need_the_sword_out(self):
        rows = module.export(resting_then_swing(False))
        self.assertEqual(len(module.motions(rows)), 1)
        rows = module.export([l.replace('"secondary"', '"primary"') for l in resting_then_swing(False)])
        self.assertEqual(module.motions(rows), [])

    def test_compare_matches_events_to_motions(self):
        rows = module.export(resting_then_swing(False))
        old = module.parse("summary rules=old swings=0\n")
        new = module.parse("swing 1.100 light 3.0\nsummary rules=new swings=1\n")
        report = module.compare(rows, old, new)
        self.assertEqual([(m["old"], m["new"]) for m in report["motions"]], [("nothing", "swing light")])
        self.assertIn("| 20 Oct rules | new rules |", module.markdown(report))

    @unittest.skipUnless(TOOL.exists(), "melee_replay not built")
    def test_tool_replays_production_rules(self):
        for hip, old_swings, new_swings in ((False, 1, 1), (True, 0, 1)):
            rows = module.export(resting_then_swing(hip))
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "s.txt"
                path.write_text("\n".join(r["text"] for r in rows) + "\n", encoding="ascii")
                old = module.run(TOOL, path, True)[1]
                new = module.run(TOOL, path, False)[1]
            self.assertEqual((int(old["swings"]), int(new["swings"])), (old_swings, new_swings),
                             "a swing from the hip rest only attacks under the new rules" if hip else "a plain swing attacks under both")


    @unittest.skipUnless(TOOL.exists(), "melee_replay not built")
    def test_feel_scenario_expectations_offline(self):
        script = json.loads((ROOT / "config" / "test_driver" / "melee_feel.json").read_text(encoding="utf-8"))
        rows = module.export(module.scenario_lines(script))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "s.txt"
            path.write_text("\n".join(r["text"] for r in rows) + "\n", encoding="ascii")
            old = module.run(TOOL, path, True)[1]
            events, new = module.run(TOOL, path, False)
            _, entry_030 = module.run(TOOL, path, False, ("--set", "guard.max_entry_forward_metres=0.30"))
        self.assertEqual((int(new["swings"]), int(new["windups"]), int(new["vetoes"])), (6, 1, 0),
                         "hip-out, chop from the shoulder, three slashes and a two-handed swing attack; the lift is a wind-up")
        # Failing from 24 Oct to 26 Oct, when it expected 0 guards. The 23 Oct guard zone (Joe: starts
        # up to 0.38 m out, was 0.30) catches the scripted left hand on its way in to the two-handed
        # grip: it turns across while still 0.35 m out, before the hands are close enough to count as
        # two-handed. That is the zone's known cost, pinned here (docs/PARRY_TIMING_2026-10-26.md).
        self.assertEqual([round(e["t"], 1) for e in events if e["kind"] == "guard"], [10.1],
                         "one guard as the left hand passes 0.30-0.38 m out on its way to the two-handed grip")
        self.assertEqual(int(entry_030["guards"]), 0, "with the 0.30 m entry limit from before 23 Oct there is none")
        self.assertLessEqual(int(old["swings"]), 2, "the 20 Oct rules miss most of them (the two-handed swing is a coin toss against the veto)")

    @unittest.skipUnless(TOOL.exists(), "melee_replay not built")
    def test_guard_raises_out_of_the_grenade_belt_26_oct(self):
        """Joe's headset playtest of 26 Oct (run 20261026_164402_playtest_0186, trimmed): six fast
        left-hand raises from the relaxed hand at the grenade belt into the guard pose. Live, five
        of them never pressed LB: the belt latched the guard and the hand was already turned across
        on its first sample out of the belt (docs/PARRY_TIMING_2026-10-26.md)."""
        fixture = ROOT / "tests" / "fixtures" / "parry_belt_raises_2026-10-26.jsonl"
        rows = module.export(fixture.read_text(encoding="utf-8").splitlines())
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "s.txt"
            path.write_text("\n".join(r["text"] for r in rows) + "\n", encoding="ascii")
            events, _ = module.run(TOOL, path, False)
            latched, _ = module.run(TOOL, path, False, ("--guard-belt-latch",))
        guards = [e["t"] for e in events if e["kind"] == "guard"]
        self.assertEqual([round(t, 1) for t in guards], [612.9, 762.9, 1229.0, 1250.9, 1294.7, 1328.8],
                         "every raise guards")
        first_out = {612.9: 612.904, 762.9: 762.931, 1250.9: 1250.915, 1294.7: 1294.684, 1328.8: 1328.783}
        for t in guards:
            if round(t, 1) in first_out:
                self.assertAlmostEqual(t, first_out[round(t, 1)], places=3, msg="on the first sample out of the belt, no later")
        self.assertEqual([round(e["t"], 1) for e in latched if e["kind"] == "guard"], [1229.9],
                         "the old belt latch swallows five raises and delays the sixth (live it pressed at 1228.97, "
                         "where the 90 Hz poll caught one forward-pointing sample first)")


    @unittest.skipUnless(TOOL.exists(), "melee_replay not built")
    def test_bolter_and_emote_scenarios_offline(self):
        def replay(name, loadout):
            script = json.loads((ROOT / "config" / "test_driver" / name).read_text(encoding="utf-8"))
            rows = module.export(module.scenario_lines(script, loadout=loadout))
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "s.txt"
                path.write_text("\n".join(r["text"] for r in rows) + "\n", encoding="ascii")
                return module.run(TOOL, path, False)
        events, _ = replay("melee_bolter.json", "primary")
        self.assertEqual([round(e["t"], 1) for e in events if e["kind"] == "swing"], [14.0],
                         "only the hard stroke attacks: not aiming, shots, a stroke right after a shot, an arm dropped from overhead or a stroke while aiming down sights")
        events, summary = replay("melee_evening.json", "secondary")
        self.assertEqual([(round(e["t"], 1), e["detail"][0]) for e in events if e["kind"] == "swing"],
                         [(2.0, "light"), (2.4, "light"), (2.9, "light"), (5.8, "power"), (7.6, "light")],
                         "the flurry after the overhead reset attacks three times; power only with the trigger held before the stroke")
        self.assertEqual([round(e["t"], 1) for e in events if e["kind"] == "push"], [9.9], "the open left-hand thrust is one push")
        self.assertEqual([round(e["t"], 1) for e in events if e["kind"] == "guard"], [12.3], "a guard 60 degrees across by the face")
        events, _ = replay("melee_emote.json", "secondary")
        self.assertEqual([round(e["t"], 1) for e in events if e["kind"] == "swing"], [2.1], "the swing out of the hip with the grip held attacks; the fist raises do not")
        self.assertEqual([round(e["t"], 1) for e in events if e["kind"] == "emote"], [7.6], "the fist beside the head is no emote; raised high it is")


class BuildTests(unittest.TestCase):
    def test_build_script_rebuilds_the_replay_tool(self):
        # 26 Oct: scripts\build-mod.bat did not build melee_replay, so the replay tests above ran a
        # stale exe (old guard rules) until it was rebuilt by hand.
        import re
        build = (ROOT / "scripts" / "build-mod.bat").read_text(encoding="utf-8")
        targets = {t for line in re.findall(r"--target ([^|]+)\|\|", build) for t in line.split()}
        self.assertIn("melee_replay", targets)


if __name__ == "__main__":
    unittest.main()
