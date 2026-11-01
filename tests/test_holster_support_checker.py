import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import check_holster_support as checker
import vr_test_driver


def fixture():
    records = [{"t": 50, "type": "event", "event": "test_driver", "detail": "probe running"},
               {"t": 66, "type": "event", "event": "test_driver", "detail": "probe completed"}]
    for tick in range(481):
        elapsed = tick / 30
        support = 1 <= elapsed < 2.5
        buttons = 2 if 4.5 <= elapsed < 4.6 or 10.5 <= elapsed < 10.6 else 0
        records.append({"t": 50+elapsed, "type": "sample",
                        "input": {"age_ms": 5, "armed": True, "support_aim": support,
                                  "raw_controls": {"right_grip": 1}, "holster_zone": 0,
                                  "controls": {"chainsword_rev": .8 if 7 <= elapsed < 8.5 else 0},
                                  "pad": {"buttons": buttons, "lt": 255 if support else 0, "rt": 0}},
                        "character": {"age_ms": 5, "actor": "abc", "root": [0, 0, 0],
                                      "root_z_axis": [0, 0, 1], "units_per_metre": 1}})
    records.extend({"t": 50+t, "type": "event", "event": "holster_select", "slot": slot}
                   for t, slot in ((4.5, "secondary"), (10.5, "primary")))
    return sorted(records, key=lambda r: r["t"])


class HolsterSupportCheckerTests(unittest.TestCase):
    def test_live_tail_is_explicit_and_counted(self):
        text = '\n'.join(json.dumps(r) for r in fixture())+'\n{"input":{"cut":"incom'
        with self.assertRaises(ValueError):
            checker.read_trace(text)
        records, receipt = checker.read_trace(text, allow_live_tail=True)
        self.assertEqual(receipt["incomplete_final_records"], 1)
        self.assertEqual(receipt["complete_records"], len(fixture()))
        self.assertTrue(checker.check(records, "probe")["routing_pass"])
        records = [r for r in records if r.get("detail") != "probe completed"]
        self.assertFalse(checker.check(records, "probe")["routing_pass"])
        complete, receipt = checker.read_trace('{"type":"event"}', allow_live_tail=True)
        self.assertEqual(len(complete), 1)
        self.assertEqual(receipt["incomplete_final_records"], 0)

    def test_live_tail_rejects_middle_newline_terminated_and_corrupt_records(self):
        for text in ('{"x":"cut\n{}', '{"x":"cut\n', '{"x":garbage', '{"x":1}garbage',
                     '{"x":truX', '{"x":01', '{"x":"\\q', '[1,2', 'null'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                checker.read_trace(text, allow_live_tail=True)

    def test_live_tail_handles_value_delimiter_string_and_number_cuts(self):
        for tail in ('{"x":', '{"x":true', '{"x":"cut', '{"x":tr', '{"x":-',
                     '{"x":1.', '{"x":1e+', '{"x":1.2e', '{"x":1.2e-3'):
            with self.subTest(tail=tail):
                records, receipt = checker.read_trace('{}\n'+tail, allow_live_tail=True)
                self.assertEqual(records, [{}])
                self.assertEqual(receipt["incomplete_final_records"], 1)

    def test_scenario_compiles_through_real_driver(self):
        script = json.loads((ROOT / "config/test_driver/holster_support_ownership.json").read_text())
        self.assertTrue(vr_test_driver.compile_script(script, "probe").startswith("SM2VR_TEST 1 probe scripted_head 16000"))

    def test_routes_can_pass_without_claiming_native_switch(self):
        result = checker.check(fixture(), "probe")
        self.assertTrue(result["routing_pass"], result)
        self.assertFalse(result["native_switch_verified"])

    def test_missing_support_extra_switch_attack_staleness_and_gap_fail(self):
        for mutation in ("support", "switch", "attack", "stale", "gap", "cancelled", "missing_pulse"):
            with self.subTest(mutation=mutation):
                records = copy.deepcopy(fixture())
                if mutation == "switch":
                    records.append({"t": 51.5, "type": "event", "event": "holster_select", "slot": "secondary"})
                if mutation == "gap":
                    records = [r for r in records if not 51.3 < r["t"] < 51.7]
                for r in records:
                    if mutation == "cancelled" and r.get("detail") == "probe completed":
                        r["detail"] = "probe focus_lost"
                    if r.get("type") == "sample" and 51.2 < r["t"] < 51.8:
                        if mutation == "support": r["input"]["support_aim"] = False
                        if mutation == "attack": r["input"]["pad"]["buttons"] = 512
                        if mutation == "stale": r["input"]["age_ms"] = 150
                    if mutation == "missing_pulse" and r.get("type") == "sample":
                        r["input"]["pad"]["buttons"] = 0
                self.assertFalse(checker.check(records, "probe")["routing_pass"])


if __name__ == "__main__":
    unittest.main()
