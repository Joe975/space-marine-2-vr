import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"scripts"))
from analyze_render_probes import analyze


def row(event, start, value, duration=0):
    return dict(event=event, qpc_start_ms=str(start), duration_ms=str(duration), value=str(value))


def fixture():
    return [row("inline_copy_appended", 10, 14), row("inline_copy_appended", 11, 15),
            row("inline_copy_submitted", 13, 14), row("inline_copy_submitted", 14, 15),
            row("inline_copy_completion_observed", 20, 14), row("inline_copy_completion_observed", 21, 15),
            row("ordinary_bridge_dispatch", 22, 7), row("ordinary_bridge_begin", 22, 99),
            row("ordinary_bridge_pending_cpu", 22, 7, 20),
            row("bridge_left_gpu", 25, 99, .2), row("bridge_right_gpu", 26, 99, .3),
            row("ordinary_submit_accepted_cpu", 42, 7, 1)]


class RenderProbeTests(unittest.TestCase):
    def test_exact_identity_join_and_gpu_clock(self):
        result = analyze(reversed(fixture()), 0, 100)
        self.assertEqual(result["joined_pairs"], 1)
        self.assertAlmostEqual(result["intervals_ms"]["bridge_gpu_end_to_submit_ms"]["mean"], 15.7)
        self.assertEqual(result["intervals_ms"]["native_observed_to_bridge_ms"]["mean"], 1)

    def test_missing_eye_cannot_claim_complete_pair(self):
        result = analyze([r for r in fixture() if r["event"] != "bridge_right_gpu"], 0, 100)
        self.assertEqual(result["joined_pairs"], 0)
        self.assertEqual(result["incomplete_accepted_pairs"], 1)

    def test_window_boundary_never_joins_other_window(self):
        result = analyze(fixture(), 12, 100)
        self.assertEqual(result["joined_pairs"], 0)
        self.assertEqual(result["accepted_pairs"], 1)

    def test_duplicates_and_reversed_timeline_fail(self):
        with self.assertRaises(ValueError):
            analyze(fixture()+[row("inline_copy_appended", 12, 14)], 0, 100)
        rows = fixture(); rows[-1] = row("ordinary_submit_accepted_cpu", 24, 7)
        with self.assertRaises(ValueError):
            analyze(rows, 0, 100)

    def test_invalid_values_fail(self):
        for bad in (row("bridge_left_gpu", 20, 99, -1), row("bridge_left_gpu", 20, float("nan"))):
            with self.assertRaises(ValueError):
                analyze([bad], 0, 100)

    def test_isolated_queue_and_depth_ready_are_actual_window_observations(self):
        result = analyze([row("ordinary_handoff_mode", 1, 3), row("ordinary_bridge_isolated", 2, 1),
                          row("scene_left_depth_ready_cpu", 3, 7, 9), row("ordinary_bridge_isolated", 101, 0)], 0, 100)
        self.assertEqual(result["handoff_modes"], [3])
        self.assertEqual(result["isolated_bridge_values"], [1])
        self.assertEqual(result["events"]["scene_left_depth_ready_cpu"]["mean"], 9)

    def test_error_counts_and_shadows_are_not_pair_ids(self):
        result = analyze([row("bridge_gpu_probe_read_failed", 30, 99), row("shadow_prepare_left_cpu", 30, 1, 2),
                          row("shadow_prepare_left_cpu", 40, 1, 3)], 0, 100)
        self.assertEqual(result["gpu_probe_errors"], 1)
        self.assertEqual(result["shadow_refresh_values"]["shadow_prepare_left_cpu"], [1])
        self.assertEqual(result["events"]["shadow_prepare_left_cpu"]["count"], 2)

    def test_sdk_landmark_requires_one_fully_contained_invocation(self):
        rows = [row("scene_left_after_third_cpu", 10, 7, 30), row("ordinary_sdk_total_cpu", 34, 7, .5)]
        result = analyze(rows, 0, 100)["pre_upscale"]["left"]
        self.assertEqual(result["third_wait_end_to_sdk_ms"]["mean"], 24)
        self.assertEqual(result["sdk_start_to_scene_end_ms"]["mean"], 6)
        for extra in (row("ordinary_sdk_total_cpu", 35, 7, 1), row("ordinary_sdk_total_cpu", 39, 7, 2)):
            result = analyze(rows+[extra], 0, 100)["pre_upscale"]["left"]
            self.assertIsNone(result["third_wait_end_to_sdk_ms"])
            self.assertEqual(result["unpartitioned_phases"], 1)

    def test_running_thread_time_joins_exact_wall_intervals_without_clamping(self):
        rows = [row("scene_left_pre_sdk_wall", 10, 7, 12), row("scene_left_pre_sdk_thread_cpu", 10, 7, 0),
                row("scene_left_pre_sdk_wall", 30, 8, 12), row("scene_left_pre_sdk_thread_cpu", 30, 8, 15.625),
                row("scene_left_pre_sdk_wall", 50, 9, 12), row("ordinary_handoff_mode", 5, 1)]
        result = analyze(rows, 0, 100)
        execution = result["pre_upscale_execution"]["left"]
        self.assertEqual(execution["matched_intervals"], 2)
        self.assertAlmostEqual(execution["aggregate_running_fraction"], 15.625/24)
        self.assertEqual(execution["thread_cpu_ms"]["mean"], 15.625/2)
        self.assertEqual(result["handoff_modes"], [1])
        with self.assertRaises(ValueError):
            analyze(rows+[row("scene_left_pre_sdk_thread_cpu", 51, 9, 2)], 0, 100)

    def test_queue_ordered_bridge_can_precede_native_completion_observation(self):
        rows = fixture()
        for r in rows:
            if r["event"] == "inline_copy_completion_observed":
                r["qpc_start_ms"] = "24"
        result = analyze(rows, 0, 100)
        self.assertEqual(result["joined_pairs"], 1)
        self.assertEqual(result["intervals_ms"]["native_observed_to_bridge_ms"]["mean"], -2)
        result = analyze(rows+[row("ordinary_pair_cpu_verified", 12, 7), row("ordinary_bridge_point", 23, 1)], 0, 100)
        self.assertEqual(result["intervals_ms"]["cpu_verified_to_bridge_ms"]["mean"], 10)
        self.assertEqual(result["intervals_ms"]["native_last_append_to_cpu_verified_ms"]["mean"], 1)
        self.assertEqual(result["bridge_points"], {"present": 0, "dispatch_complete": 1})


if __name__ == "__main__":
    unittest.main()
