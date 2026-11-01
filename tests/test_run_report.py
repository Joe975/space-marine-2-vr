import contextlib
import importlib.util
import io
import json
import sys
import tempfile
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module  # dataclasses resolve their annotations through sys.modules
    spec.loader.exec_module(module)
    return module


tool = load("run_report", "scripts/run_report.py")

# Trimmed from build/run/20261024_195508_playtest_merged_fixes2 (the stop the 24 Oct rule fixed),
# with a mask no run had shown and a new readiness reason added.
STOPPED = """\
[20:00:08.000] [I] [tid:1] CinematicState: available=true scripted=true paused=false published=0x4c10 requested=0x4c10 samples=1 failures=0 transitions=1
[20:00:09.000] [I] [tid:1] OrdinaryOwnedReadiness: reason=actor_missing consecutive=0 checks=1 blocked=1 ready_jobs=0 owner_changes=0 job=7639
[20:02:10.298] [E] [tid:1] OrdinaryHeadsetStopped: dispatch=17798 packet_refusal=0 bloom_refusal=5 scene_calls=2 scene_refused=0 scene_dropped=0
[20:02:11.000] [I] [tid:1] CinematicState: available=true scripted=false paused=false published=0x20000 requested=0x0 samples=2 failures=0 transitions=2
[20:02:12.000] [I] [tid:1] OrdinaryOwnedReadiness: reason=brand_new consecutive=0 checks=2 blocked=2 ready_jobs=0 owner_changes=0 job=17800
[20:02:13.000] [I] [tid:1] VrFlatFallback: active=true reason=stopped submitted=1214 frames=0
"""

# A level load that suspends and recovers, a hang, and the crash handler's register dump.
RECOVERED = """\
[20:53:34.384] [E] [tid:1] OrdinaryHeadsetSuspended: dispatch=43109 bloom_refusal=12 packet_refusal=4 contract_failure=none last_accepted=43108 episode=1 transition_hold=true
[20:53:35.000] [I] [tid:1] VrMenuPanel: active=true path=ordinary source=pause image=true native_flags=0x908/0x0 coverage=0.98 width_degrees=90 distance_metres=2
[20:54:13.688] [E] [tid:1] HangWatchdog: no frame presented for 10920 ms; thread dump written=true path=hang.dmp
[20:54:23.899] [I] [tid:1] OrdinaryHeadsetRecovered: dispatch=43638 previous=0 episode=1 transition_episodes=1 held_ms=49517
[20:55:00.000] [E] [tid:2] Exception occurred: c0000005
[20:55:00.000] [E] [tid:2] RIP: 2ae5189c19c
[20:55:00.000] [E] [tid:2] Module: 7ff7a9ed0000 game.exe
"""

STOP_THEN_REARM = """\
[10:00:00.000] [E] [tid:1] OrdinaryHeadsetStopped: dispatch=10 packet_refusal=0 bloom_refusal=6 scene_calls=2
[10:00:02.000] [I] [tid:1] OrdinaryHeadsetRearm: armed=true presents=12
[10:00:03.000] [I] [tid:1] OrdinaryHeadsetProducer: job=20 accepted=true refusal=0 packet_refusal=0 submitted=5
[10:00:04.000] [I] [tid:1] SceneMode: mode=hub mask=0x800 previous=gameplay unknown_bits=0x0 first_mask=true changes=2 masks=2
[10:00:05.000] [W] [tid:1] SceneMode: mode=unknown mask=0x80000 previous=hub unknown_bits=0x80000 first_mask=true changes=3 masks=3
[10:00:06.000] [W] [tid:1] CallCensus: mono=200 stereo=300 ready=true rates=[scene:1.00/2.00,grading_index0:1.00/2.00,temporal:1.00/1.00] doubled=[grading_index0] missing=[temporal] (a per-frame hook runs per eye)
"""


class RunReport(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)
        self.table = self.dir / "table.json"
        self.table.write_text(json.dumps(json.loads((ROOT / "config" / "known_run_states.json").read_text(encoding="utf-8"))), encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def write(self, text, name="run"):
        run = self.dir / name
        run.mkdir()
        (run / "sm2vr.log").write_text(text, encoding="utf-8")
        return run

    def run_tool(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = tool.main([*map(str, args), "--table", str(self.table)])
        return code, out.getvalue()

    def test_new_mask_and_reason_are_reported_and_fail_strict(self):
        report = tool.read(self.write(STOPPED) / "sm2vr.log")
        tool.compare(report, tool.load_table(self.table))
        self.assertEqual(report.new["masks"], ["0x20000"])
        self.assertEqual(report.new["readiness_reasons"], ["brand_new"])
        self.assertNotIn("bloom_refusals", report.new)  # 5 is known
        code, text = self.run_tool(self.dir / "run", "--strict")
        self.assertEqual(code, 1)
        self.assertIn("NEW masks 0x20000: first 20:02:11.000, 1 times", text)

    def test_stop_without_rearm_is_a_verdict(self):
        report = tool.read(self.write(STOPPED) / "sm2vr.log")
        self.assertTrue(any("stopped for good at 20:02:10.298" in v for v in report.verdicts))

    def test_rearm_clears_the_stop(self):
        report = tool.read(self.write(STOP_THEN_REARM) / "sm2vr.log")
        self.assertFalse(any("stopped for good" in v for v in report.verdicts))
        self.assertTrue(report.stereo_pairs)

    def test_cold_restart_clears_the_stop_but_an_exhausted_budget_does_not(self):
        restarted = STOPPED + "[20:02:14.000] [W] [tid:1] OrdinaryHeadsetColdRestart: cold_restarts=1 in_window=1 budget=3 bloom_stopped=true\n"
        report = tool.read(self.write(restarted) / "sm2vr.log")
        self.assertFalse(any("stopped for good" in v for v in report.verdicts))
        exhausted = restarted + "[20:30:00.000] [E] [tid:1] OrdinaryHeadsetStopped: budget=exhausted cold_restarts=3 in_window=3 attempts=1\n"
        report = tool.read(self.write(exhausted, "exhausted") / "sm2vr.log")
        self.assertTrue(any("stopped for good at 20:30:00.000" in v for v in report.verdicts))

    def test_dlss_off_in_the_game_is_a_verdict(self):
        held = STOP_THEN_REARM + "[10:00:07.000] [W] [tid:1] OrdinaryDlssHold: held=true jobs=120 submitted=5 (the game is running neither DLSS nor TAA)\n"
        report = tool.read(self.write(held) / "sm2vr.log")
        self.assertTrue(any("neither DLSS nor TAA from 10:00:07.000" in v for v in report.verdicts))
        back = STOP_THEN_REARM + "[10:00:08.000] [I] [tid:1] OrdinaryDlssHold: held=false submitted=5 (DLSS or TAA is back)\n"
        report = tool.read(self.write(back, "back") / "sm2vr.log")
        self.assertFalse(any("neither DLSS nor TAA" in v for v in report.verdicts))

    def test_pair_cap_and_slow_recovery_are_verdicts(self):
        # Trimmed from build/run/20261026_114008_playtest_classes: 78 s flat after a class change.
        log = (
            "[11:55:21.501] [E] [tid:1] OrdinaryHeadsetSuspended: dispatch=22567 bloom_refusal=12 packet_refusal=4 transition_hold=true\n"
            "[11:55:24.600] [E] [tid:1] OrdinaryBloomRenewed: refused generation=12 cap=12 (no fresh owned pair left; stereo waits for the pool's old layout)\n"
            "[11:56:39.992] [I] [tid:1] OrdinaryHeadsetRecovered: dispatch=24284 previous=0 episode=4 held_ms=78493 attempts=4\n"
            "[11:59:31.177] [I] [tid:1] OrdinaryHeadsetRecovered: dispatch=30949 previous=30476 episode=5 held_ms=9686 attempts=5\n")
        report = tool.read(self.write(log) / "sm2vr.log")
        self.assertTrue(any("owned pair cap reached at 11:55:24.600" in v for v in report.verdicts))
        self.assertTrue(any("came back slowly at 11:56:39.992: 78 s" in v for v in report.verdicts))
        self.assertFalse(any("11:59:31" in v for v in report.verdicts), "a 10 s hold is normal")
        self.assertFalse(any("never recovered" in v for v in report.verdicts))

    def test_renewals_count_reuses_and_pairs(self):
        # The 26 Oct 14:46 run's first renewals as they log since retired pairs are reused, plus an old-format line.
        log = (
            "[14:52:47.908] [W] [tid:1] OrdinaryBloomRenewed: reused=false renewals=1 generation=2 cap=12 retired=1 difference=0x1a0 episode=1 (native pool rebuilt to a new layout; owned pair retired, a fresh pair at the bootstrap)\n"
            "[14:54:35.567] [W] [tid:1] OrdinaryBloomRenewed: reused=true renewals=2 generation=2 cap=12 retired=2 difference=0x80 episode=2 (native pool rebuilt to a layout seen before; its retired pair is current again at the bootstrap)\n"
            "[14:59:03.887] [W] [tid:1] OrdinaryBloomRenewed: reused=true renewals=3 generation=2 cap=12 retired=2 difference=0x80 episode=1 (native pool rebuilt to a layout seen before; its retired pair is current again at the bootstrap)\n")
        report = tool.read(self.write(log) / "sm2vr.log")
        self.assertEqual((report.renewals, report.renewal_reuses, report.pairs), (3, 2, 2))
        self.assertEqual(sum(kind == "OrdinaryBloomRenewed" for _, kind, _ in report.events), 3)
        self.assertFalse(report.verdicts, "a renewal is no verdict")
        self.assertIn("Owned pair renewals: 3 (2 reused a retired pair, 1 new layouts; 2 pairs created)", tool.describe(report, {}))
        old = "[11:45:05.444] [W] [tid:1] OrdinaryBloomRenewed: renewals=1 generation=2 difference=0x120 episode=1 (native pool rebuilt; owned pair retired, fresh bootstrap next)\n"
        report = tool.read(self.write(old, "old") / "sm2vr.log")
        self.assertEqual((report.renewals, report.renewal_reuses, report.pairs), (1, 0, 2), "a log from before reuse always made a pair")

    def test_readiness_stall_give_up_is_a_verdict(self):
        stall = (
            "[11:19:58.000] [W] [tid:1] StereoStall: action=rediscover reason=actor_missing scene=gameplay stalled_ms=10000 tries=1 total=1 body_trackers=32 bodies=46 overflow=16105\n"
            "[11:20:38.000] [E] [tid:1] StereoStall: action=give_up reason=actor_missing scene=gameplay stalled_ms=40000 tries=3 total=3 body_trackers=32 bodies=46 overflow=17000\n")
        report = tool.read(self.write(stall) / "sm2vr.log")
        self.assertTrue(any("stalled in plain play at 11:20:38.000 (actor_missing)" in v for v in report.verdicts))
        hub = stall.replace("[E]", "[I]").replace("scene=gameplay", "scene=hub")
        report = tool.read(self.write(hub, "hub") / "sm2vr.log")
        self.assertFalse(any("stalled" in v for v in report.verdicts), "a hub give-up (main menu, no player) is logged at info and is no verdict")

    def test_pip_behind_the_pair_is_a_verdict(self):
        # 26 Oct 14:44:59 (docs/LOAD_CRASH_2026-10-26.md): the class selector's PIP landed in slot 2.
        log = (
            "[14:44:59.110] [E] [tid:1] StereoForeignView: slot=2 flags=0x10043 behind_pair=true sightings=1 holds=1 (a game PIP view landed behind the owned pair)\n"
            "[14:45:30.000] [I] [tid:1] ForeignViewHold: start slot=1 flags=0x10043 holds=2 settle_ms=5000 (a game PIP view; owned stereo holds off while it shows)\n")
        report = tool.read(self.write(log) / "sm2vr.log")
        self.assertTrue(any("game PIP view behind the owned pair at 14:44:59.110" in v for v in report.verdicts))
        self.assertTrue(any(kind == "ForeignViewHold" for _, kind, _ in report.events))
        held = tool.read(self.write(log.splitlines()[1] + "\n", "held") / "sm2vr.log")
        self.assertFalse(held.verdicts, "a hold that started in mono (PIP in slot 1) is normal")

    def test_recovery_clears_the_suspension_but_hang_and_crash_stay(self):
        report = tool.read(self.write(RECOVERED) / "sm2vr.log")
        self.assertFalse(any("never recovered" in v for v in report.verdicts))
        self.assertTrue(any(v.startswith("hang at 20:54:13.688") for v in report.verdicts))
        self.assertTrue(any("exception c0000005" in v for v in report.verdicts))
        # The register dump is part of the exception, not a warning kind of its own.
        self.assertNotIn("RIP", report.values["warning_kinds"])
        self.assertNotIn("Module", report.values["warning_kinds"])
        self.assertIn("0x908", report.values["masks"])

    def test_unrecovered_suspension_is_a_verdict(self):
        text = RECOVERED.splitlines()[0] + "\n"
        report = tool.read(self.write(text) / "sm2vr.log")
        self.assertTrue(any("never recovered" in v for v in report.verdicts))

    def test_scene_modes_new_masks_and_doubled_hooks(self):
        report = tool.read(self.write(STOP_THEN_REARM) / "sm2vr.log")
        self.assertEqual(set(report.values["scene_modes"]), {"hub", "unknown"})
        self.assertIn("0x80000", report.values["masks"])
        self.assertTrue(any(k == "SceneMode" and "no rule for mask 0x80000" in s for _, k, s in report.events))
        self.assertFalse(any(k == "SceneMode" and "0x800 " in s for _, k, s in report.events), "a known mode is not an event")
        self.assertEqual(set(report.values["doubled_hooks"]), {"grading_index0"})
        self.assertEqual(set(report.values["missing_hooks"]), {"temporal"})
        tool.compare(report, tool.load_table(self.table))
        self.assertEqual(report.new["doubled_hooks"], ["grading_index0"], "an untriaged doubled hook is NEW")
        self.assertEqual(report.new["missing_hooks"], ["temporal"])
        table = tool.load_table(self.table)
        table["doubled_hooks"]["grading_index0"] = "triaged: harmless"
        report = tool.read(self.dir / "run" / "sm2vr.log")
        tool.compare(report, table)
        self.assertNotIn("doubled_hooks", report.new, "a triaged one is not")

    def test_clean_run_passes_strict(self):
        clean = STOP_THEN_REARM.splitlines()[:4]
        code, text = self.run_tool(self.write("\n".join(clean) + "\n"), "--strict")
        self.assertEqual(code, 0, text)
        self.assertIn("Nothing new, no verdicts.", text)

    def test_learn_adds_new_values_to_the_table(self):
        self.run_tool(self.write(STOPPED), "--learn")
        table = json.loads(self.table.read_text(encoding="utf-8"))
        self.assertIn("0x20000", table["masks"])
        self.assertIn("brand_new", table["readiness_reasons"])
        report = tool.read(self.dir / "run" / "sm2vr.log")
        tool.compare(report, tool.load_table(self.table))
        self.assertEqual(report.new, {})

    def test_missing_log(self):
        code, _ = self.run_tool(self.dir / "absent")
        self.assertEqual(code, 2)

    def test_shipped_table_is_valid_and_complete(self):
        table = json.loads((ROOT / "config" / "known_run_states.json").read_text(encoding="utf-8"))
        for category in tool.CATEGORIES:
            self.assertIn(category, table)
        # Every readiness reason the mod can log is known (OwnedStereoReadiness::Reason).
        header = (ROOT / "mod" / "src" / "OwnedStereoReadiness.h").read_text(encoding="utf-8")
        enum = header.split("enum class Reason:uint32_t {", 1)[1].split("}", 1)[0]
        for reason in (r.strip() for r in enum.split(",")):
            self.assertIn(reason, table["readiness_reasons"], reason)

    def test_table_and_scene_classifier_agree(self):
        """Every mask in the table has a classification test, and every mode a label."""
        import re
        table = json.loads((ROOT / "config" / "known_run_states.json").read_text(encoding="utf-8"))
        header = (ROOT / "mod" / "src" / "SceneMode.h").read_text(encoding="utf-8")
        modes = header.split("enum class Mode:uint32_t {", 1)[1].split("}", 1)[0]
        self.assertEqual({m.strip() for m in modes.split(",")}, set(table["scene_modes"]))
        test = (ROOT / "tests" / "test_scene_mode.cpp").read_text(encoding="utf-8")
        seen = set(re.findall(r"\{(0x[0-9a-f]+),(?:true|false),Mode::", test))
        self.assertEqual(seen, set(table["masks"]), "a mask added to the table needs its row in tests/test_scene_mode.cpp SEEN")


class TourRoute(unittest.TestCase):
    """The tour only uses steps drive-game.ps1 has and keys game-keys.ps1 can send."""

    def test_route_steps_exist(self):
        import re
        tour = (ROOT / "scripts" / "tour.ps1").read_text(encoding="utf-8")
        route = re.search(r"^\$route = '([^']+)'", tour, re.M).group(1)
        drive = (ROOT / "scripts" / "drive-game.ps1").read_text(encoding="utf-8")
        steps = set(re.findall(r"^\s+'([a-z-]+)' \{", drive, re.M))
        keys = set(re.findall(r"(\w+) = 0x", (ROOT / "scripts" / "game-keys.ps1").read_text(encoding="utf-8")))
        self.assertTrue({"continue", "quit-title", "keys", "wait", "shot"} <= steps, steps)
        for step in route.split(","):
            name, _, arg = step.partition(":")
            self.assertIn(name, steps, step)
            if name == "keys":
                for key in arg.split("+"):
                    self.assertIn(key, keys, step)
        # The tour ends at the title (the level-exit freeze) and starts with the real menu flow.
        self.assertTrue(route.startswith("continue,"))
        self.assertIn("quit-title", route)
        self.assertNotIn("-DirectLaunch", tour)


if __name__ == "__main__":
    unittest.main()
