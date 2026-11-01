import copy
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "qualify_embodiment_trace.py"
SPEC = importlib.util.spec_from_file_location("embodiment_gate", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def sample():
    body = []
    c = dict(age_ms=0, driven=True, units_per_metre=1,
             lean_degrees=dict(native=55.87, final=0))
    for hand, side, x in (("left", "L", -.2), ("right", "R", .2)):
        body.extend([dict(name="ARM1"+side, native=[x, 1.5, 0], final=[x, 1.5, 0]),
                     dict(name="ARM2"+side, native=[x, 1.5, -.4], final=[x, 1.5, -.4]),
                     dict(name="forearm_"+side, native=[x, 1.5, -.8], final=[x, 1.5, -1.1])])
        c[hand+"_native"] = [x, 1.5, -.8]
        c[hand+"_solved"] = c[hand+"_target"] = [x, 1.5, -1.1]
    c["body"] = body
    return dict(type="sample", t=1, input=dict(armed=True, age_ms=0), character=c, game=dict(paused=False))


class EmbodimentGate(unittest.TestCase):
    def test_published_correction_passes(self):
        r = MODULE.qualify([sample()], minimum=1)
        self.assertTrue(r["accepted"], r)
        self.assertEqual(r["extended_hands"], 2)

    def test_old_cap_and_detached_armour_fail(self):
        s = sample()
        s["character"]["lean_degrees"]["final"] = 25.87
        for b in s["character"]["body"]:
            if b["name"].startswith("forearm_"):
                b["final"] = b["native"]
        r = MODULE.qualify([s], minimum=1)
        self.assertFalse(r["accepted"])
        self.assertIn("published lean exceeds 2 degrees", r["errors"])
        self.assertIn("armour endpoint offset changes by more than 10 mm", r["errors"])

    def test_missing_stale_or_short_capture_cannot_pass(self):
        for change in (lambda s: s["character"].update(body=[]),
                       lambda s: s["input"].update(age_ms=101),
                       lambda s: s["game"].update(paused=True),
                       lambda s: s["character"]["body"][2].update(final=[float("nan"), 0, 0]),
                       lambda s: s.update(t=10)):
            s = copy.deepcopy(sample())
            change(s)
            self.assertFalse(MODULE.qualify([s], start=0, end=2, minimum=1)["accepted"])
        self.assertFalse(MODULE.qualify([sample()], minimum=2)["accepted"])


if __name__ == "__main__":
    unittest.main()
