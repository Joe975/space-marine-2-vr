import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


checker = load("rev_check", "scripts/check_chainsword_rev.py")
SCENARIO = json.loads((ROOT / "config/test_driver/chainsword_rev_default.json").read_text())


def held(frames, field, ms):
    """Value a scripted analog field holds at ms (held until the next keyframe)."""
    return [f for f in frames if f["ms"] <= ms][-1].get(field, 0)


class ChainswordRevCheckerTests(unittest.TestCase):
    def records(self):
        rows = [{"event": "test_driver", "detail": "case running", "t": 100},
                {"event": "test_driver", "detail": "case completed", "t": 133}]
        starts = stops = 0
        t = 0.0
        while t <= 33:
            force = held(SCENARIO["frames"], "fire", t * 1000)
            aim = held(SCENARIO["frames"], "aim", t * 1000)
            loadout = "primary" if 24 <= t <= 30.5 else "secondary"
            revving = loadout == "secondary" and 7 <= t < 16
            if revving and t == 7:
                starts += 1
            if t == 16:
                stops += 1
            fire = aim if loadout == "secondary" else force
            rev = force if loadout == "secondary" else 0
            rows.append({"type": "sample", "t": 100 + t, "input": {
                "age_ms": 5, "armed": True, "loadout": loadout, "controls": {"chainsword_rev": rev},
                "pad": {"buttons": 0, "rt": round(fire * 255), "lt": 0}},
                "chainsword_rev": {"installed": True, "active": revving, "starts": starts, "stops": stops,
                                   "play_rate": checker.rev_rate(force) if revving else 1.0,
                                   "gain": checker.MIN_GAIN + (1 - checker.MIN_GAIN) * checker.rev_amount(force)}})
            t = round(t + .05, 2)
        return rows

    def test_phase_windows_match_the_scripted_triggers(self):
        frames = SCENARIO["frames"]
        for name, low, high, force, _ in checker.PHASES:
            for ms in (low * 1000, high * 1000):
                self.assertAlmostEqual(held(frames, "fire", ms), force, msg=name)
                self.assertEqual(held(frames, "aim", ms), .8 if name == "pistol" else 0, name)

    def test_scenario_compiles_using_real_driver_schema(self):
        driver = load("driver", "scripts/vr_test_driver.py")
        self.assertTrue(driver.compile_script(SCENARIO, "case").startswith("SM2VR_TEST 1 case scripted_head 33000"))

    def test_expected_run_passes(self):
        result = checker.check(self.records(), "case")
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["phases"]["native_starts"], 1)

    def test_each_failure_mode_is_caught(self):
        def corrupt(edit):
            rows = self.records()
            for row in rows:
                if row.get("type") == "sample":
                    edit(row["t"] - 100, row)
            return checker.check(rows, "case")

        def rest_revs(t, r):
            if 5 <= t <= 6:
                r["chainsword_rev"]["active"] = True

        def eased_stops(t, r):
            if 14 <= t <= 15:
                r["chainsword_rev"]["active"] = False

        def restarted(t, r):
            if t >= 11:
                r["chainsword_rev"]["starts"] += 1
                r["chainsword_rev"]["stops"] += 1

        def slow_teeth(t, r):
            if 11 <= t <= 12:
                r["chainsword_rev"]["play_rate"] = 1.0

        def rifle_revs(t, r):
            if 27 <= t <= 28:
                r["input"]["controls"]["chainsword_rev"] = .8

        def rev_fires(t, r):
            if 8 <= t <= 9:
                r["input"]["pad"]["rt"] = 255

        def loadout_flicker(t, r):
            if 8.5 <= t <= 8.6:
                r["input"]["loadout"] = None

        def not_installed(t, r):
            r["chainsword_rev"]["installed"] = False

        for edit in (rest_revs, eased_stops, restarted, slow_teeth, rifle_revs, rev_fires, loadout_flicker, not_installed):
            self.assertFalse(corrupt(edit)["passed"], edit.__name__)
        incomplete = [r for r in self.records() if r.get("detail") != "case completed"]
        self.assertFalse(checker.check(incomplete, "case")["passed"])


if __name__ == "__main__":
    unittest.main()
