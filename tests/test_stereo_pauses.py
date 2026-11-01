"""scripts/stereo_pauses.py: pauses of the owned stereo pairs and their causes (docs/STEREO_PAUSES_2026-10-26.md)."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("stereo_pauses", ROOT / "scripts" / "stereo_pauses.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
FIXTURE = ROOT / "tests" / "fixtures" / "stereo_pauses_2026-10-26.txt"


def log_line(t, kind, rest):
    h, m, s = 18, int(t // 60), t % 60
    return f"[{h:02d}:{m:02d}:{s:06.3f}] [I] [tid:1] {kind}: {rest}"


def periodic(t, blocked, skips, suspended=0, held=0, reason="ready"):
    return [log_line(t, "OrdinaryOwnedReadiness", f"reason={reason} consecutive=3 checks=1 blocked={blocked} ready_jobs=1 owner_changes=0 job=1"),
            log_line(t, "ForeignViewHold", f"held=false sightings=0 holds=0 behind_pair=0 slot=1 held_jobs={held}"),
            log_line(t, "OrdinaryHeadsetProducer", f"job=1 accepted=true refusal=0 clock_skips={skips} suspended_jobs={suspended}")]


def pause(start, end, dispatch, previous):
    return [log_line(start, "OrdinaryHeadsetRetention", "ready=true stopped=false active=false pairs=0 refusal=0"),
            log_line(end, "OrdinaryHeadsetResumed", f"dispatch={dispatch} previous={previous} accepted=true bloom_refusal=0 history_reset=true")]


def compact(item):
    return json.dumps(item, separators=(",", ":"))


def trace(frames):
    """frames: (t, pose_age_ms, flags) from 18:00:00."""
    lines = [compact({"t": 0.0, "type": "header", "format": "sm2vr-trace", "version": 1, "hz": 30.0,
                      "wall_clock_local": "2026-10-26T18:00:00.000"})]
    for t, pose, flags in frames:
        lines.append(compact({"t": round(t, 3), "type": "sample", "input": {"age_ms": 20.0}, "character": {"age_ms": pose},
                              "game": {"flags": flags}}))
    return lines


class StereoPauses(unittest.TestCase):
    def setUp(self):
        # Four pauses: a long game frame (10 s), a clock skip (20 s), a freshness refusal with
        # no visible hitch (30 s) and one in photo mode (40 s). Then a suspension (50 s).
        log = periodic(5, 0, 0)
        log += pause(10.00, 10.12, 104, 100) + periodic(12, 3, 0)
        log += pause(20.00, 20.10, 206, 200) + periodic(22, 7, 2)
        log += pause(30.00, 30.09, 304, 300) + periodic(32, 10, 2)
        log += [log_line(39.9, "OrdinaryClockGap", "skipped=4 markers=1 errors=0")]
        log += pause(40.00, 40.12, 408, 400) + periodic(42, 10, 6)
        log += pause(50.00, 50.60, 530, 500) + periodic(52, 10, 6, suspended=28)
        frames = []
        t = 0.0
        while t < 60:
            flags = 0x108 if 39 <= t <= 41 else 0
            frames.append((t, 25.0, flags))
            t += 0.25 if 9.8 <= t < 10.0 else 0.04  # one 250 ms frame just before the first pause
        self.found = module.analyse(log, trace(frames))

    def test_pauses_and_mono_jobs(self):
        self.assertEqual([p["mono"] for p in self.found], [3, 5, 3, 7, 29])
        self.assertEqual([p["ms"] for p in self.found], [120, 100, 90, 120, 600])

    def test_causes(self):
        self.assertEqual([p["cause"] for p in self.found], ["hitch", "clock", "freshness", "viewer", "suspended"])
        self.assertGreater(self.found[0]["trace"]["gap_ms"], 200)
        self.assertEqual(self.found[3]["context"][0].split()[1], "OrdinaryClockGap")

    def test_trigger_jobs(self):
        # The jobs before readiness's two settling ones, only where the resume window applies.
        self.assertEqual([p["trigger"] for p in self.found], [1, 3, 1, None, None])

    def test_summary_and_fixture(self):
        rows = {r["cause"]: r for r in module.summary(self.found)}
        self.assertEqual(rows["hitch"]["settle_jobs"], 2)
        self.assertEqual(rows["viewer"]["settle_jobs"], 0)
        text = module.fixture(self.found, "run/test")
        self.assertIn("p 1 clock 64820.000 100 5 3", text)
        self.assertIn("p 3 viewer 64840.000 120 7 -1", text)
        self.assertIn("| hitch | 1 |", module.markdown(self.found))

    def test_without_trace(self):
        found = module.analyse(pause(10.0, 10.1, 104, 100))
        self.assertEqual(found[0]["mono"], 3)
        self.assertEqual(found[0]["cause"], "unknown")

    def test_committed_fixture(self):
        # The 18:30 run's replay fixture (tests/test_owned_stereo_readiness.cpp reads it).
        rows = [line.split() for line in FIXTURE.read_text(encoding="utf-8").splitlines() if line.startswith("p ")]
        self.assertEqual(len(rows), 59)
        gameplay = [r for r in rows if r[2] in module.RESUMABLE]
        self.assertEqual(len(gameplay), 40)
        self.assertTrue(all(int(r[6]) == int(r[5]) - 2 >= 1 for r in gameplay))
        self.assertTrue(all(int(r[6]) == -1 for r in rows if r[2] not in module.RESUMABLE))


if __name__ == "__main__":
    unittest.main()
