import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("combat_check", ROOT / "scripts/check_combat_driver.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class CombatDriverTests(unittest.TestCase):
    def records(self):
        result = [{"event":"test_driver", "detail":"case running", "t":0},
                  {"event":"test_driver", "detail":"case completed", "t":16}]
        for times,buttons,rt,rev in (([2.2,2.4,2.6],256,0,0),([4.7,4.9,5.1],0,255,0),
                                     ([7.02,7.07,7.12],512,0,0),([10.02,10.07,10.12],1,0,1),
                                     ([12.8,13.2,13.6],0,0,1)):
            for t in times:
                result.append({"type":"sample","t":t,"character":{"age_ms":5},"input":{
                    "age_ms":5,"armed":True,"pad":{"buttons":buttons,"rt":rt,"lt":0},
                    "controls":{"chainsword_rev":rev}}})
        return result

    def test_pad_gate_pass_is_explicitly_weaker_than_native_action_acceptance(self):
        result = checker.check(self.records(), "case")
        self.assertTrue(result["passed"], result)
        self.assertFalse(result["native_actions_verified"])
        for corrupt in ("pistol_melee", "missing_light", "power_parry", "stale", "incomplete"):
            rows = self.records()
            for row in rows:
                t = row["t"]
                if corrupt == "pistol_melee" and 4.6<t<5.4: row["input"]["pad"]["buttons"] = 512
                if corrupt == "missing_light" and 7<t<7.3: row["input"]["pad"]["buttons"] = 0
                if corrupt == "power_parry" and 10<t<10.3: row["input"]["pad"]["buttons"] = 257
                if corrupt == "stale" and row.get("input"): row["input"]["age_ms"] = 101
            if corrupt == "incomplete": rows = [r for r in rows if r.get("detail") != "case completed"]
            self.assertFalse(checker.check(rows,"case")["passed"], corrupt)

    def test_scenario_compiles_using_real_driver_schema(self):
        spec = importlib.util.spec_from_file_location("driver", ROOT / "scripts/vr_test_driver.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        script = json.loads((ROOT / "config/test_driver/combat_ownership.json").read_text())
        self.assertTrue(driver.compile_script(script,"case").startswith("SM2VR_TEST 1 case scripted_head 16000"))


if __name__ == "__main__": unittest.main()
