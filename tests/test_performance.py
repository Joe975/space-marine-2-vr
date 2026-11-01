import io
import json
import sys
import subprocess
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"scripts"))
from analyze_performance import analyze


def present(step=10, gpu="6", other=False):
    lines = ["ProcessID,SwapChainAddress,CPUStartQPCTime,GPUBusy,GPUTime,CPUBusy,CPUWait,PresentMode,SyncInterval"]
    for n in range(101):
        lines.append(f"42,0x1,{1000+n*step},{gpu},8,7,3,Composed: Flip,0")
        if other:
            lines.append(f"42,0x2,{1000+n*step},1,1,1,1,Composed: Flip,0")
    return io.StringIO("\n".join(lines))


def spans(mode="full"):
    lines = ["qpc_start_ms,event,duration_ms,value,mode"]
    for n in range(100):
        lines.append(f"{1000+n*10},present_hook,2,0,{mode}")
        if n % 2 == 0:
            lines.append(f"{1000+n*10},pair_submitted,0,35,full")
    return io.StringIO("\n".join(lines))


class PerformanceEvidence(unittest.TestCase):
    def test_cli_cannot_silently_ignore_required_path_without_cpu_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'present.csv'
            output=Path(directory)/'summary.json'
            source.write_text(present().getvalue(),encoding='utf-8')
            result=subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'scripts'/'analyze_performance.py'),
                str(source),'--render-path','single_frame','--out',str(output)],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(output.exists())
            self.assertIn('CPU trace',result.stderr)

    def test_single_frame_path_is_verified_without_assuming_half_rate_pairs(self):
        trace=spans().getvalue().replace('pair_submitted','unused_pair')
        for n in range(100):
            trace+=f'\n{1000+n*10},ordinary_present_bridge_cpu,1,0,full'
            trace+=f'\n{1000+n*10},pair_submitted,0,35,full'
        report=analyze(present(),io.StringIO(trace),'full',False,'single_frame')
        self.assertEqual(report['render_path'],'single_frame')
        self.assertEqual(report['game_present_fps'],100)
        self.assertEqual(report['fresh_pairs_per_second'],100)
        self.assertIsNone(report['eye_copies_per_second'],'ordinary copies have no legacy AFR eye-copy events')
        with self.assertRaisesRegex(ValueError,'render path'):
            analyze(present(),spans(),'full',False,'single_frame')
        with self.assertRaisesRegex(ValueError,'render path'):
            analyze(present(),io.StringIO(trace+'\n1500,capture_eye_cpu,1,0,full'),'full',False,'single_frame')
        with self.assertRaises(ValueError):
            analyze(present(),None,None,False,'single_frame')

    def test_truncated_trace_cannot_qualify_a_complete_capture(self):
        trace = spans().getvalue().replace("present_hook,2,0,", "present_hook,2,1,")
        for keep in (lambda t: t < 1800, lambda t: t >= 1200):
            truncated = "\n".join(line for line in trace.splitlines()
                                  if line.startswith("qpc_") or keep(float(line.split(",")[0])))
            with self.assertRaisesRegex(ValueError, "complete PresentMon window"):
                analyze(present(), io.StringIO(truncated), "full", True)

    def test_openxr_validation_rejects_idle_and_mixed_visibility_even_with_pairs(self):
        trace=spans().getvalue().replace("present_hook,2,0,", "present_hook,2,1,")
        xr="\n1100,xr_session_state,0,5,full\n1100,xr_should_render,0,1,full\n"
        self.assertEqual(analyze(present(),io.StringIO(trace+xr),"full",True)["fresh_pairs_per_second"],50)
        for invalid in (xr.replace("state,0,5", "state,0,3"),
                        xr+"1200,xr_session_state,0,NA,full\n",
                        xr+"1200,xr_should_render,0,0,full\n",
                        xr.replace("render,0,1", "render,0,NA"),
                        "\n1100,openxr_pair_cpu,1,0,full\n"):
            with self.assertRaisesRegex(ValueError,"OpenXR capture"):
                analyze(present(),io.StringIO(trace+invalid),"full",True)
        # Non-submitting isolation phases are still meaningful while unworn.
        analyze(present(),io.StringIO((trace+xr).replace("full","copy").replace("render,0,1","render,0,0")),"copy",True)

    def test_openxr_state_is_preserved_without_inventing_hmd_presence(self):
        trace=spans().getvalue()+"\n1100,xr_session_state,0,5,full\n1100,xr_should_render,0,1,full\n1100,xr_predicted_refresh_hz,0,90,full\n"
        report=analyze(present(),io.StringIO(trace),"full")
        self.assertEqual(report["observations"]["xr_session_state"],{5.0:1})
        self.assertEqual(report["observations"]["xr_should_render"],{1.0:1})
        self.assertEqual(report["observations"]["xr_predicted_refresh_hz"],{90.0:1})
        self.assertEqual(report["observations"]["hmd_activity"],{})

    def test_present_fps_gpu_cost_and_pair_rate_are_separate(self):
        report = analyze(present(), spans(), "full")
        self.assertEqual(report["game_present_fps"], 100)
        self.assertEqual(report["fresh_pairs_per_second"], 50)
        self.assertEqual(report["present_interval_ms"]["p99"], 10)
        self.assertEqual(report["metrics_ms"]["gpu_busy"]["mean"], 6)
        self.assertEqual(report["cpu_span_duty_percent"]["present_hook"], 20)
        self.assertEqual(report["older_eye_pose_age_at_submit_end_ms"]["mean"], 35)

    def test_missing_metrics_are_unknown_not_zero(self):
        report = analyze(present(gpu="NA"))
        self.assertIsNone(report["metrics_ms"]["gpu_busy"])
        self.assertIsNone(report["metrics_ms"]["gpu_latency"])
        json.dumps(report, allow_nan=False)

    def test_other_swapchains_and_boundaries_do_not_inflate_rate(self):
        report = analyze(present(other=True), spans())
        self.assertEqual(report["frames"], 101)
        self.assertEqual(report["discarded_other_swapchain_rows"], 101)
        self.assertEqual(report["game_present_fps"], 100)
        self.assertEqual(report["fresh_pairs_per_second"], 50)

    def test_wrong_clock_mode_and_multiple_processes_fail_closed(self):
        for source in (present().getvalue().replace("CPUStartQPCTime", "CPUStartQPC"),
                       present().getvalue().replace("42,0x1,1010", "43,0x1,1010")):
            with self.assertRaises(ValueError):
                analyze(io.StringIO(source))
        with self.assertRaises(ValueError):
            analyze(present(), spans("tracking"), "full")
        with self.assertRaises(ValueError):
            analyze(present(), io.StringIO("qpc_start_ms,event,duration_ms,value,mode\n"), "full")

    def test_long_frame_is_included_and_not_silently_filtered(self):
        report = analyze(present(step=50))
        self.assertEqual(report["game_present_fps"], 20)
        self.assertEqual(report["over_22_22ms_percent"], 100)

    def test_live_file_later_partial_row_does_not_corrupt_completed_window(self):
        trace = spans().getvalue()+"\n2001,openvr_hando"
        report = analyze(present(), io.StringIO(trace), "full")
        self.assertEqual(report["fresh_pairs_per_second"], 50)
        with self.assertRaises(ValueError):
            analyze(present(), io.StringIO(spans().getvalue()+"\n1501,broken"), "full")

    def test_gameplay_validation_rejects_focus_loss_pause_and_missing_runtime(self):
        trace = spans().getvalue().replace("present_hook,2,0,", "present_hook,2,1,")
        self.assertEqual(analyze(present(), io.StringIO(trace), "full", True)["fresh_pairs_per_second"], 50)
        for flag in (0, 3, 5, 9, 17, 33):
            with self.assertRaises(ValueError):
                analyze(present(), io.StringIO(trace.replace("present_hook,2,1,", f"present_hook,2,{flag},", 1)), "full", True)
        with self.assertRaises(ValueError):
            analyze(present(), None, "full", True)
        with self.assertRaises(ValueError):
            analyze(present(), io.StringIO(trace.replace("present_hook,2,1,", "present_hook,2,NA,", 1)), "full", True)
        with self.assertRaises(ValueError):
            analyze(present(), io.StringIO("\n".join(line for line in trace.splitlines() if "pair_submitted" not in line)), "full", True)


if __name__ == "__main__":
    unittest.main()
