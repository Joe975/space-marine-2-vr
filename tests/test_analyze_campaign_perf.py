"""Fixtures for scripts/analyze_campaign_perf.py: one synthetic -PerfCampaign run per verdict."""
import csv
import io
import shutil
import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import analyze_campaign_perf as perf  # noqa: E402

BASE = 1_000_000.0       # QPC ms of the first frame
UNIX = 1_792_000_000.0   # Unix seconds at BASE
PRESENT, RENDER, SIM = 100, 200, 300


class Run:
    """Writes cpu_trace.csv (and optionally presentmon.csv / samplers) for a steady scene."""

    def __init__(self, seconds=40, frame=40.0, dispatch=0.5, dispatch_cpu=0.9, gpu=0.5, render_cpu=0.5, sim_cpu=0.5,
                 present_cpu=0.2, machine=0.4, depth_ready=0.0, present_hook=0.02, mono=(), presentmon=True, gameplay=True):
        self.rows, self.pm = [], []
        holds = {int(s) for a, b in mono for s in range(a, b)}
        t, frame_index = BASE, 0
        end = BASE + seconds * 1000
        while t < end:
            second = int((t - BASE) // 1000)
            held = second in holds
            f = frame * (0.5 if held else 1.0)  # Flat renders one view: twice the frames.
            self.add("pcl_simulation_start", t, 0, frame_index, SIM)
            self.add("pcl_simulation_end", t + 0.4 * f * sim_cpu, 0, frame_index + 1, SIM)
            d = dispatch * f
            self.add("ordinary_dispatch_total_cpu", t + 1, d + 0.1, 0, RENDER)
            self.add("ordinary_dispatch_native_cpu", t + 1, d, 0, RENDER)
            self.add("ordinary_dispatch_thread_cpu", t + 1, d * dispatch_cpu, d, RENDER)
            self.add("ordinary_scene_left_cpu", t + 1, d * (0.55 if not held else 1.0), 0, RENDER)
            if not held:
                self.add("ordinary_scene_right_cpu", t + 1 + d * 0.55, d * 0.45, 0, RENDER)
            if depth_ready:
                self.add("scene_left_depth_ready_cpu", t + 2, depth_ready * f, frame_index, RENDER)
            self.add("scene_left_other_wait_cpu", t + 2, 0.01, 0, RENDER)
            self.add("present_hook", t + d + 2, present_hook * f, 1 if gameplay else 3, PRESENT)
            if not held:
                self.add("pair_submitted", t + d + 3, 0, 120.0, PRESENT)
            self.pm.append((t + d + 2, f, f * 0.9, f * 0.1, gpu * f, f * (1 - gpu)))
            t += f
            frame_index += 1
        for second in range(seconds):
            at = BASE + second * 1000
            self.add("wall_clock", at, 0, UNIX + second, PRESENT)
            self.add("stereo_hold", at + 5, 0, 1 if second in holds else 0, PRESENT)
            if second:
                prev = at - 1000
                self.add("trace_writer_cpu", prev, 1000, 2.0, PRESENT)
                self.add("trace_rows", prev, 1000, 900, PRESENT)
                self.add("thread_cpu_simulation", prev, 1000, 1000 * sim_cpu, PRESENT)
                self.add("thread_cpu_render", prev, 1000, 1000 * render_cpu, PRESENT)
                self.add("thread_cpu_present", prev, 1000, 1000 * present_cpu, PRESENT)
                self.add("process_cpu", prev, 1000, 4000, PRESENT)
                self.add("system_cpu_busy", prev, 1000, machine, PRESENT)
        self.presentmon = presentmon

    def add(self, event, start, duration, value, thread):
        self.rows.append((start, event, duration, value, "full", thread))

    def write(self, directory: Path):
        with (directory / "cpu_trace.csv").open("w", newline="") as handle:
            out = csv.writer(handle)
            out.writerow(["qpc_start_ms", "event", "duration_ms", "value", "mode", "thread"])
            for start, event, duration, value, mode, thread in sorted(self.rows, key=lambda r: r[0]):
                out.writerow([f"{start:.6f}", event, f"{duration:.6f}", f"{value:.6f}", mode, thread])
        if self.presentmon:
            with (directory / "presentmon.csv").open("w", newline="", encoding="utf-8-sig") as handle:
                out = csv.writer(handle)
                out.writerow(["Application", "ProcessID", "SwapChainAddress", "CPUStartQPCTime", "FrameTime", "CPUBusy", "CPUWait", "GPUBusy", "GPUWait"])
                for start, frame, busy, wait, gpu, idle in self.pm:
                    out.writerow(["game.exe", 1, "0x1", f"{start:.4f}", f"{frame:.4f}", f"{busy:.4f}", f"{wait:.4f}", f"{gpu:.4f}", f"{idle:.4f}"])


class CampaignPerfTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.directory)

    def run_fixture(self, run, **options):
        run.write(self.directory)
        return perf.analyze(self.directory, **options)

    def only(self, report, mode="stereo"):
        windows = [w for w in report["windows"] if w["mode"] == mode]
        self.assertEqual(len(windows), 1, report["windows"])
        return windows[0]

    def verdict(self, **settings):
        return self.only(self.run_fixture(Run(**settings)))["verdict"]

    def test_each_verdict(self):
        self.assertEqual(self.verdict(gpu=0.95), "gpu")
        self.assertEqual(self.verdict(render_cpu=0.95), "render_cpu")
        self.assertEqual(self.verdict(sim_cpu=0.95), "simulation_cpu")
        self.assertEqual(self.verdict(dispatch=0.8, dispatch_cpu=0.3, depth_ready=0.4), "fence")
        self.assertEqual(self.verdict(dispatch=0.8, dispatch_cpu=0.3, machine=0.95), "workers")
        self.assertEqual(self.verdict(dispatch=0.8, dispatch_cpu=0.3), "wait_unknown")
        self.assertEqual(self.verdict(present_hook=0.25), "mod")
        self.assertEqual(self.verdict(), "unclear")

    def test_rule_order(self):
        # Our own hooks are named first even when the GPU is also near its limit.
        self.assertEqual(self.verdict(present_hook=0.25, gpu=0.95), "mod")
        # A busy GPU outranks a busy render thread (the thread may be feeding it).
        self.assertEqual(self.verdict(gpu=0.95, render_cpu=0.95), "gpu")
        # A dispatch that runs on its thread is not a wait.
        self.assertEqual(self.verdict(dispatch=0.8, dispatch_cpu=0.9, depth_ready=0.4), "unclear")

    def test_metrics_of_one_window(self):
        window = self.only(self.run_fixture(Run(gpu=0.95)))
        m = window["metrics"]
        self.assertAlmostEqual(m["frame_ms"]["median"], 40.0, places=3)
        self.assertAlmostEqual(m["fresh_pairs_per_s"], 25.0, delta=0.5)
        self.assertAlmostEqual(m["gpu"]["gpu_busy_fraction"], 0.95, places=3)
        self.assertAlmostEqual(m["threads"]["render_cpu_fraction"], 0.5, places=6)
        self.assertEqual((m["threads"]["present_tid"], m["threads"]["render_tid"], m["threads"]["simulation_tid"]), (PRESENT, RENDER, SIM))
        self.assertAlmostEqual(m["dispatch"]["wall_ms_median"], 20.0, places=3)
        self.assertAlmostEqual(m["dispatch"]["thread_cpu_of_wall"], 0.9, places=6)
        self.assertAlmostEqual(m["dispatch"]["left_scene_ms"], 11.0, places=3)
        self.assertAlmostEqual(m["dispatch"]["right_scene_ms"], 9.0, places=3)
        self.assertAlmostEqual(m["threads"]["simulation_ms_median"], 0.4 * 40 * 0.5, places=3)
        self.assertAlmostEqual(m["profiler"]["trace_writer_ms_per_s"], 2.0, places=6)
        self.assertEqual(window["local_start"], datetime.fromtimestamp(UNIX + 3).strftime("%H:%M:%S"))

    def test_missing_inputs_are_null_not_zero(self):
        m = self.only(self.run_fixture(Run(presentmon=False)))["metrics"]
        self.assertIsNone(m["gpu"])
        self.assertIsNone(m["gpu_utilization_nvidia_smi"])
        self.assertNotIn("stacks", m)

    def test_flat_and_stereo_windows(self):
        # 30 s stereo, 30 s mono hold, 30 s stereo: seconds holding a change and the 3 s after it are dropped.
        report = self.run_fixture(Run(seconds=90, mono=[(30, 60)]))
        modes = [(w["mode"], round(w["metrics"]["seconds"])) for w in report["windows"]]
        self.assertEqual(modes, [("stereo", 26), ("mono", 26), ("stereo", 27)])
        mono = [w for w in report["windows"] if w["mode"] == "mono"][0]["metrics"]
        self.assertAlmostEqual(mono["game_fps"], 50.0, delta=1)
        self.assertEqual(mono["fresh_pairs_per_s"], 0)
        flat = report["flat_vs_vr"]
        self.assertAlmostEqual(flat["flat_fps"], 50.0, delta=1)
        self.assertAlmostEqual(flat["vr_pairs_per_s"], 25.0, delta=0.5)
        self.assertAlmostEqual(flat["vr_over_flat"], 0.5, delta=0.02)

    def test_paused_or_menu_seconds_are_not_windows(self):
        self.assertEqual(self.run_fixture(Run(gameplay=False))["windows"], [])

    def test_wall_clock_window(self):
        run = Run(seconds=90, mono=[(30, 60)])
        run.write(self.directory)
        first = datetime.fromtimestamp(UNIX + 10).strftime("%H:%M:%S")
        last = datetime.fromtimestamp(UNIX + 45).strftime("%H:%M:%S")
        report = perf.analyze(self.directory, [f"{first}-{last}=fight"], min_seconds=5)
        self.assertEqual([(w["label"], w["mode"]) for w in report["windows"]], [("fight", "stereo"), ("fight", "mono")])
        self.assertAlmostEqual(report["windows"][0]["metrics"]["seconds"], 17, delta=1.01)

    def test_old_trace_without_thread_column(self):
        # The 21 Oct traces predate the thread column and the once-a-second rows.
        Run().write(self.directory)
        lines = (self.directory / "cpu_trace.csv").read_text().splitlines()
        keep = [lines[0].rsplit(",", 1)[0]] + [line.rsplit(",", 1)[0] for line in lines[1:]
                                                 if ",wall_clock," not in line and ",thread_cpu" not in line and ",stereo_hold," not in line]
        (self.directory / "cpu_trace.csv").write_text("\n".join(keep) + "\n")
        (self.directory / "presentmon.csv").unlink()
        report = perf.analyze(self.directory)
        window = self.only(report)
        self.assertFalse(report["thread_column"])
        self.assertIsNone(window["metrics"]["threads"]["render_cpu_fraction"])
        self.assertEqual(window["verdict"], "unclear")
        self.assertIn("missing", window["summary"])

    def test_stack_classification(self):
        exe = perf.GAME_EXE
        game_dir = "f:\\steam\\steamapps\\common\\space marine 2\\client_pc\\root\\bin\\pc"
        wait = [("ntdll.dll", 0x1, "NtWaitForSingleObject", ""), ("kernelbase.dll", 0x2, "WaitForSingleObjectEx", ""),
                (exe, 0x180C675, "", ""), (exe, 0x1806000, "", "")]
        self.assertEqual(perf.classify_sample(wait), ("blocked", "depth-ready fence (ZMIPS_READY_0)"))
        reference = [wait[0], (exe, 0x1806490, "", "")]
        self.assertEqual(perf.classify_sample(reference), ("blocked", "scene depth wait"))
        driver = [("nvwgf2umx.dll", 0x10, "", ""), (exe, 0x5000, "", "")]
        self.assertEqual(perf.classify_sample(driver), ("running", "GPU driver / D3D12"))
        mod = [("ntdll.dll", 0x1, "NtReadVirtualMemory", ""), ("kernelbase.dll", 0x2, "ReadProcessMemory", ""),
               ("dxgi.dll", 0x3, "", game_dir + "\\dxgi.dll")]
        self.assertEqual(perf.classify_sample(mod, game_dir), ("running", "mod (dxgi.dll proxy)"))
        system_dxgi = [("dxgi.dll", 0x3, "", "c:\\windows\\system32\\dxgi.dll"), (exe, 0x5000, "", "")]
        self.assertEqual(perf.classify_sample(system_dxgi), ("running", "dxgi.dll"))
        self.assertEqual(perf.classify_sample([(exe, 0x5000, "", "")]), ("running", "game code"))

    def test_sampler_summary_in_window(self):
        run = Run()
        run.write(self.directory)
        exe = perf.GAME_EXE
        with (self.directory / "sampler_render.csv").open("w", newline="") as handle:
            out = csv.writer(handle)
            out.writerow(["sample", "qpc_ms", "suspended_ms", "error", "frame", "pc", "module", "rva", "symbol", "path"])
            for i in range(200):
                at = BASE + 5000 + i * 50
                if i % 4 == 0:
                    out.writerow([i, at, 0.05, 0, 0, "0x1", "ntdll.dll", "0x1", "NtWaitForSingleObject", ""])
                    out.writerow([i, at, 0.05, 0, 1, "0x2", exe, "0x180c675", "", ""])
                else:
                    out.writerow([i, at, 0.05, 0, 0, "0x3", exe, "0x5000", "", ""])
        stacks = self.only(perf.analyze(self.directory))["metrics"]["stacks"]["render"]
        self.assertEqual(stacks["samples"], 200)
        self.assertAlmostEqual(stacks["blocked_fraction"], 0.25)
        self.assertAlmostEqual(stacks["states"]["running: game code"], 0.75)
        self.assertAlmostEqual(stacks["suspended_ms_per_s"], 1000 * 200 * 0.05 / 37000, places=6)

    def test_table_names_the_flat_comparison(self):
        report = self.run_fixture(Run(seconds=90, mono=[(30, 60)]))
        text = perf.table(report)
        self.assertIn("flat 50.0 fps vs VR 25.0 pairs/s", text.replace("49.9", "50.0").replace("50.1", "50.0"))


if __name__ == "__main__":
    unittest.main()
