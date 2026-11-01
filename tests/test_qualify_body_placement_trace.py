import contextlib
import copy
import importlib.util
import io
import json
import math
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/qualify_body_placement_trace.py"
SPEC = importlib.util.spec_from_file_location("body_placement_gate", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def sample(time=1, yaw=0, units=.9):
    forward = [math.sin(yaw), 0, math.cos(yaw)]
    shift = [-.2*units*x for x in forward]
    body = []
    for n, name in enumerate(MODULE.SHIFTED + MODULE.UNCHANGED):
        native = [4+n*.1, 6+n*.1, 12-n*.1]
        final = [p+d for p, d in zip(native, shift)] if name in MODULE.SHIFTED else native.copy()
        body.append(dict(name=name, native=native, final=final))
    character = dict(age_ms=0, actor="abc", driven=True, units_per_metre=units,
                     root_z_axis=forward, body=body,
                     left_solved=[1, 2, 3], left_target=[1, 2, 3],
                     right_solved=[4, 5, 6], right_target=[4, 5, 6])
    return dict(type="sample", t=time, character=character,
                input=dict(armed=True, age_ms=0, epoch=7),
                game=dict(available=True, paused=False, cinematic=False))


class BodyPlacementGate(unittest.TestCase):
    def test_yaw_units_and_pitched_actor_reference_pass(self):
        for yaw in (0, math.pi/2, math.pi):
            for units in (.9, 1.7):
                row = sample(yaw=yaw, units=units)
                # Production projects/normalizes the actor axis, never head pitch.
                row["character"]["root_z_axis"] = [math.sin(yaw)*.8, .6, math.cos(yaw)*.8]
                result = MODULE.qualify([row], minimum=1)
                self.assertTrue(result["accepted"], result)
                self.assertTrue(all(g["status"] == "pass" for g in result["gates"].values()))
                self.assertEqual(result["placement_telemetry"]["status"], "unavailable")

    def test_old_disabled_body_and_world_axis_offset_fail(self):
        row = sample(yaw=math.pi/2)
        for bone in row["character"]["body"]:
            if bone["name"] in MODULE.SHIFTED:
                bone["final"] = bone["native"].copy()
        result = MODULE.qualify([row], minimum=1)
        self.assertFalse(result["accepted"])
        self.assertEqual(result["gates"]["CENTRE"]["status"], "fail")
        for bone in row["character"]["body"]:
            if bone["name"] in MODULE.SHIFTED:
                bone["final"][2] -= .18  # Fixed world Z instead of yawed body back.
        self.assertFalse(MODULE.qualify([row], minimum=1)["accepted"])
        # A physical-metre displacement must be scaled exactly once in world space.
        row = sample(units=.9)
        for bone in row["character"]["body"]:
            if bone["name"] in MODULE.SHIFTED:
                bone["final"][2] = bone["native"][2]-.2
        self.assertFalse(MODULE.qualify([row], minimum=1)["accepted"])

    def test_each_bone_and_wrist_gate_detects_a_bad_publication(self):
        for name in MODULE.SHIFTED + MODULE.UNCHANGED:
            row = sample()
            next(b for b in row["character"]["body"] if b["name"] == name)["final"][0] += .03
            result = MODULE.qualify([row], minimum=1)
            self.assertFalse(result["accepted"], name)
            self.assertEqual(result["gates"][name]["status"], "fail", name)
        for side in ("left", "right"):
            row = sample();row["character"][side+"_solved"][2] += .01
            self.assertEqual(MODULE.qualify([row], minimum=1)["gates"][side+"_wrist"]["status"], "fail")

    def test_missing_fields_are_unavailable_even_with_other_good_samples(self):
        for name in MODULE.SHIFTED + MODULE.UNCHANGED:
            row = sample()
            row["character"]["body"] = [b for b in row["character"]["body"] if b["name"] != name]
            result = MODULE.qualify([row, sample(1.1)], minimum=1)
            self.assertFalse(result["accepted"])
            self.assertEqual(result["gates"][name]["status"], "unavailable")
            self.assertIn("bone not recorded", result["gates"][name]["reasons"])
        for change in (lambda c: c.update(left_target=None),
                       lambda c: c["body"][0].update(final=[float("nan"), 0, 0]),
                       lambda c: c["body"].append(copy.deepcopy(c["body"][0]))):
            row = sample();change(row["character"])
            self.assertFalse(MODULE.qualify([row], minimum=1)["accepted"])

    def test_uncommitted_stale_future_or_inactive_cannot_hide_in_good_samples(self):
        changes = [lambda r: r["character"].update(driven=False),
                   lambda r: r["input"].update(armed=False),
                   lambda r: r["character"].update(age_ms=101),
                   lambda r: r["input"].update(age_ms=-.1),
                   lambda r: r["game"].update(paused=True),
                   lambda r: r["game"].update(cinematic=True),
                   lambda r: r["game"].update(available=False),
                   lambda r: r.update(character=None),
                   lambda r: r["character"].update(actor="0"),
                   lambda r: r["input"].update(epoch=None),
                   lambda r: r["character"].update(units_per_metre=0),
                   lambda r: r["character"].update(root_z_axis=[0, 1, 0])]
        for change in changes:
            row = sample();change(row)
            result = MODULE.qualify([row, sample(1.1)], minimum=1)
            self.assertFalse(result["accepted"], result)

    def test_window_same_actor_epoch_coverage_and_count(self):
        self.assertTrue(MODULE.qualify([sample(.1), sample(1), sample(1.1), sample(9)], start=.9, end=2, minimum=2)["accepted"])
        self.assertFalse(MODULE.qualify([sample()], minimum=2)["accepted"])
        for key, value, producer in (("actor", "def", "character"), ("epoch", 8, "input")):
            row = sample(1.1);row[producer][key] = value
            self.assertFalse(MODULE.qualify([sample(), row], minimum=2)["accepted"])
        self.assertFalse(MODULE.qualify([sample(), sample(2)], minimum=2)["accepted"])
        self.assertFalse(MODULE.qualify([sample(), sample()], minimum=2)["accepted"])

    def test_optional_commit_telemetry_must_not_contradict_positions(self):
        row = sample();row["character"]["body_placement"] = dict(back_metres=.2, updates=1)
        self.assertTrue(MODULE.qualify([row], minimum=1)["accepted"])
        for telemetry in (dict(back_metres=.2, updates=0), dict(back_metres=0, updates=100), {}, "invalid",
                          dict(back_metres=.2, updates=10, stage="body_placement", refusal="output"),
                          dict(back_metres=.2, updates=10, stage="committed", refusal="mapping"),
                          dict(back_metres=.2, updates=10, applied_back_metres=0),
                          dict(back_metres=.2, updates=10, applied_back_metres=.2, control_status="invalid")):
            row["character"]["body_placement"] = telemetry
            self.assertFalse(MODULE.qualify([row], minimum=1)["accepted"])

    def test_bad_arguments_fail_closed(self):
        for args in (dict(minimum=0), dict(end=0), dict(start=float("nan")), dict(back_metres=0), dict(back_metres=.5)):
            with self.assertRaises(ValueError):
                MODULE.qualify([sample()], **args)


class StrictTraceReader(unittest.TestCase):
    def test_tail_is_explicit_and_middle_corruption_is_never_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"trace.jsonl"
            complete = json.dumps(sample())+"\n"
            path.write_text(complete+'{"type":"sample","t":', encoding="utf-8")
            with self.assertRaises(ValueError):
                MODULE.read_trace(path)
            records, receipt = MODULE.read_trace(path, allow_live_tail=True)
            self.assertEqual(len(records), 1)
            self.assertEqual(receipt["incomplete_live_tail_ignored"], 1)
            for malformed in (complete+'{bad}\n', complete+'{bad}\n'+complete,
                              complete+'{"type":"sample",}',
                              complete+'{"type":"sample",}   ',
                              complete+'{bad}',
                              complete+'{"type":"sample","t":NaN}',
                              complete+'{"type":"sample","t":1,"t":2}',
                              complete+'[]\n'):
                path.write_text(malformed, encoding="utf-8")
                with self.assertRaises(ValueError):
                    MODULE.read_trace(path, allow_live_tail=True)
            path.write_text(json.dumps(sample()), encoding="utf-8")
            self.assertEqual(MODULE.read_trace(path)[1]["incomplete_live_tail_ignored"], 0)

    def test_cli_returns_json_and_failure_for_missing_or_old_body_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"trace.jsonl"
            row = sample();row["character"]["body"] = []
            path.write_text(json.dumps(row)+"\n", encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = MODULE.main([str(path), "--start", "0", "--end", "2", "--minimum", "1"])
            self.assertEqual(code, 1)
            result = json.loads(output.getvalue())
            self.assertFalse(result["accepted"])
            self.assertEqual(result["gates"]["CENTRE"]["status"], "unavailable")


if __name__ == "__main__":
    unittest.main()
