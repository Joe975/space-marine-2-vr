"""Fixtures for scripts/analyze_depth_ready_wait.py: a GPU-wait stall and a plain backlog."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from analyze_depth_ready_wait import analyze, load, unpack  # noqa: E402

RENDER, WORKER = 10, 20
DIRECT, COMPUTE = 1, 2
ZMIPS, OTHER = 5, 6


def pack(queue, fence, value):
    return queue * 2 ** 48 + fence * 2 ** 32 + value


def row(event, at, value, duration=0.0, thread=RENDER):
    return {"qpc_start_ms": str(at), "event": event, "duration_ms": str(duration), "value": str(value), "mode": "full", "thread": str(thread)}


def frames(stall):
    rows = [row("queue_identity", 0, DIRECT * 16 + 0), row("queue_identity", 0, COMPUTE * 16 + 2)]
    for n in range(10):
        t = 100.0 + n * 22
        rows.append(row("queue_execute", t + 0.5, DIRECT * 1000 + 3))
        if stall:
            # The direct queue waits for compute work that a worker submits late, 8 ms into the CPU wait.
            rows.append(row("queue_wait", t + 1.0, pack(DIRECT, OTHER, n + 1)))
            rows.append(row("queue_signal", t + 10.0, pack(COMPUTE, OTHER, n + 1), thread=WORKER))
            rows.append(row("fence_completed", t + 10.4, pack(COMPUTE, OTHER, n + 1)))
        rows.append(row("queue_signal", t + 1.5, pack(DIRECT, ZMIPS, n + 1)))
        rows.append(row("scene_left_depth_ready_cpu", t + 2.0, n, duration=9.0))
        rows.append(row("fence_cpu_wait", t + 2.1, pack(0, ZMIPS, n + 1), duration=0.01))
        rows.append(row("fence_completed", t + 10.9, pack(DIRECT, ZMIPS, n + 1)))
    return rows


class DepthReadyWaitTests(unittest.TestCase):
    def test_unpack(self):
        self.assertEqual(unpack(pack(3, 17, 5)), (3, 17, 5))

    def test_gpu_wait_stall_names_the_late_fence(self):
        report = analyze(load(frames(stall=True)))
        summary = report["summary"]
        self.assertEqual(summary["waits"], 10)
        self.assertEqual(summary["causes"], {f"GPU wait on fence {OTHER} signalled on queue {COMPUTE} (compute)": 10})
        self.assertAlmostEqual(summary["signal_before_wait_ms_median"], 0.5)
        self.assertAlmostEqual(summary["completed_after_wait_end_ms_median"], -0.1)
        example = report["examples"][0]
        self.assertEqual((example["queue"], example["queue_type"], example["fence"]), (DIRECT, "direct", ZMIPS))
        wait = example["gpu_waits_ahead"][0]
        self.assertEqual(wait["signal_thread"], WORKER)
        self.assertAlmostEqual(wait["signal_after_wait_start_ms"], 8.0)

    def test_external_fence_is_named(self):
        # A GPU wait on a fence nothing in the process signals; the watcher still sees it arrive late.
        rows = [r for r in frames(stall=True) if not (r["event"] in ("queue_signal",) and r["thread"] == str(WORKER))]
        rows = [dict(r, value=str(pack(0, OTHER, unpack(float(r["value"]))[2]))) if r["event"] == "fence_completed" and unpack(float(r["value"]))[1] == OTHER else r for r in rows]
        summary = analyze(load(rows))["summary"]
        self.assertEqual(list(summary["causes"]), [f"GPU wait on fence {OTHER} signalled on nothing in this process (another process, e.g. the VR compositor)"])

    def test_invisible_stall_inside_end_frame(self):
        # 21 Oct test room: the runtime's last signal inside xrEndFrame is reached 13 ms later, the one
        # before it at once, and no GPU wait is traced: the queue stopped inside xrEndFrame.
        rows = [row("queue_identity", 0, DIRECT * 16 + 0)]
        for n in range(10):
            t = 100.0 + n * 22
            rows.append(row("openxr_end_frame_cpu", t - 5.0, 0, duration=0.7))
            rows.append(row("queue_signal", t - 4.8, pack(DIRECT, OTHER, 2 * n + 1)))
            rows.append(row("fence_completed", t - 4.5, pack(DIRECT, OTHER, 2 * n + 1)))
            rows.append(row("queue_signal", t - 4.35, pack(DIRECT, OTHER, 2 * n + 2)))
            rows.append(row("fence_completed", t + 8.6, pack(DIRECT, OTHER, 2 * n + 2)))
            rows.append(row("queue_signal", t + 1.5, pack(DIRECT, ZMIPS, n + 1)))
            rows.append(row("scene_left_depth_ready_cpu", t + 2.0, n, duration=9.0))
            rows.append(row("fence_cpu_wait", t + 2.1, pack(0, ZMIPS, n + 1), duration=0.01))
            rows.append(row("fence_completed", t + 10.9, pack(DIRECT, ZMIPS, n + 1)))
        report = analyze(load(rows))
        self.assertEqual(report["summary"]["causes"], {"queue stalled behind work queued inside openxr_end_frame_cpu (no traced GPU wait)": 10})
        stop = report["examples"][0]["queue_stopped_at"]
        self.assertAlmostEqual(stop["queued_before_wait_ms"], 6.35)
        self.assertAlmostEqual(stop["previous_reached_after_queue_ms"], 0.3)

    def test_without_a_gpu_wait_it_is_backlog(self):
        summary = analyze(load(frames(stall=False)))["summary"]
        self.assertEqual(summary["causes"], {"backlog: earlier work on the same queue": 10})

    def test_window_and_eye_filters(self):
        events = load(frames(stall=False))
        self.assertEqual(analyze(events, start=100, end=150)["summary"]["waits"], 3)
        self.assertEqual(analyze(events, eye="right")["summary"]["waits"], 0)

    def test_registration_on_another_thread_is_ignored(self):
        rows = [r for r in frames(stall=False) if r["event"] != "fence_cpu_wait"]
        rows += [row("fence_cpu_wait", 102.1, pack(0, ZMIPS, 1), thread=WORKER)]
        self.assertEqual(analyze(load(rows))["summary"]["waits"], 0)


if __name__ == "__main__":
    unittest.main()
