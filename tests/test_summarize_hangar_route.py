import contextlib
import io
import json
import shutil
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import summarize_hangar_route as module  # noqa: E402

TEMP = ROOT / "build" / "test_temp" / "summarize_hangar_route"
EYE_W, H = 64, 48


def status(time, rate, age, queue, thread=False):
    return (f"[{time}.000] [I] [tid:1] VrSession: status=OpenXR tracking active; session state=5; queue={queue} epoch=1 "
            f"submitted_pairs=100 per_eye_hz={rate} failed_transfers=0 last_render_id=1 oldest_pose_age_ms={age} submit_cpu_ms=0.9 "
            f"worn_references=0 submit_thread={'true' if thread else 'false'} submit_replaced=0")


def write_stream(prefix: Path, right_blip_frame=None):
    """A smoothly brightening both-eye stream, one capture per game frame; one right-eye-only blip if asked."""
    x = np.arange(EYE_W, dtype=np.float32)
    frames = []
    for t in range(30):
        eye = np.repeat(np.tile(40 + 0.4 * x + 0.5 * t, (H, 1))[..., None], 3, axis=2)
        right = eye.copy()
        if t == right_blip_frame:
            right[0:6, 0:8] += 60
        frames.append(np.concatenate([eye, right], axis=1))
    np.clip(np.stack(frames), 0, 255).astype(np.uint8).tofile(prefix.with_suffix(".rgb"))
    prefix.with_suffix(".json").write_text(json.dumps({"width": 2 * EYE_W, "height": H, "eye_width": EYE_W}))
    prefix.with_suffix(".times").write_text("".join(f"{t:.4f} 10:00:{t // 30:02d}.{t * 33 % 1000:03d}\n" for t in range(30)))


class SummarizeHangarRouteTest(unittest.TestCase):
    def setUp(self):
        shutil.rmtree(TEMP, ignore_errors=True)
        (TEMP / "run1A").mkdir(parents=True)
        (TEMP / "run2B").mkdir(parents=True)
        (TEMP / "run1A" / "sm2vr.log").write_text("\n".join([
            status("10:00:05", 35.0, 100.0, "game"), status("10:00:10", 36.0, 98.0, "game"),
            status("10:00:40", 20.0, 150.0, "game")]) + "\n")
        (TEMP / "run2B" / "sm2vr.log").write_text("\n".join([
            status("10:00:05", 50.0, 60.0, "own", True), status("10:00:10", 52.0, 58.0, "own", True)]) + "\n")
        (TEMP / "runs.csv").write_text(f"1A,{TEMP / 'run1A'}\n2B,{TEMP / 'run2B'}\n")
        (TEMP / "segments.csv").write_text("name,step,start,end,state\n"
                                           "1A,1still,10:00:00,10:00:20,completed\n1A,2enter,10:00:21,10:00:25,clock_gap\n"
                                           "2B,1still,10:00:00,10:00:20,completed\n")
        write_stream(TEMP / "1A_1still", right_blip_frame=15)
        write_stream(TEMP / "2B_1still")

    def test_steps_take_their_window_queue_and_one_eye_blips(self):
        rows = {r["name"]: r for r in module.rows(TEMP)}
        self.assertEqual(set(rows), {"1A", "2B"})  # the unrecorded enter step is skipped
        self.assertEqual((rows["1A"]["queue"], rows["1A"]["pairs"], rows["1A"]["pose"]), ("game", 35.5, 99.0))
        self.assertEqual((rows["2B"]["queue"], rows["2B"]["pairs"]), ("own+thr", 51.0))
        self.assertGreater(rows["1A"]["right_only"], 0)
        self.assertEqual(rows["1A"]["left_only"], 0)
        self.assertEqual(rows["2B"]["right_only"], 0)

    def test_pooled_lines_group_by_arm_and_step(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            module.main([str(TEMP)])
        pooled = out.getvalue().split("(medians of the launches' steps)")[1]
        self.assertRegex(pooled, r"A\s+still\s+1\s+35\.5\s+99\.0")
        self.assertRegex(pooled, r"B\s+still\s+1\s+51\.0\s+59\.0")


if __name__ == "__main__":
    unittest.main()
