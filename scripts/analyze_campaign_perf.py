"""What limits the campaign frame rate? One report per window of a -PerfCampaign run.

    python -I scripts/analyze_campaign_perf.py ABS_DUMP_DIR [--window HH:MM:SS-HH:MM:SS[=label]]...
        [--min-seconds 20] [--out ABS.json]

ABS_DUMP_DIR is the run's ordinary dump folder (build/ordinary/<date>/<tag>): cpu_trace.csv, and
from the companion presentmon.csv, gpu.csv, sampler_render.csv, sampler_simulation.csv. Only
cpu_trace.csv is required; anything missing makes its metrics null, never zero.

Windows. Without --window, the run is cut into segments of continuous foreground gameplay (Present
flags exactly 1) in one mode: `stereo` (stereo hold released and pairs submitted) or `mono` (stereo
hold set: the game renders one native view, scripts/stereo-hold.ps1). The first 3 s after each
change settle and are dropped; segments shorter than --min-seconds are dropped. --window picks a
fight by local wall-clock time (the trace's wall_clock rows map it to QPC) and is split by mode the
same way.

Per window: fresh pairs/s, game frame time, GPU busy (PresentMon; nvidia-smi as a cross-check),
simulation/render/Present thread CPU against wall time, the native dispatch's wall and own-thread
CPU per frame, time in native fences (depth-ready, depth/shadow/third and other waits), per-eye
scene time, the mod's own hook time, the profiler's own cost, thread-stack states, and a one-line
verdict (verdict()). All CPU spans are elapsed time unless named thread CPU. Profiled numbers:
compare windows of the same run, not with unprofiled play.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path

# Events kept from cpu_trace.csv (a long run has millions of rows).
SPANS = {
    "present_hook", "pair_submitted", "stereo_hold", "wall_clock", "trace_writer_cpu", "trace_rows",
    "thread_cpu_simulation", "thread_cpu_render", "thread_cpu_present", "process_cpu", "system_cpu_busy",
    "ordinary_dispatch_total_cpu", "ordinary_dispatch_native_cpu", "ordinary_dispatch_thread_cpu",
    "ordinary_scene_left_cpu", "ordinary_scene_right_cpu", "ordinary_scene_left_thread_cpu", "ordinary_scene_right_thread_cpu",
    "ordinary_temporal_total_cpu", "ordinary_temporal_native_cpu", "ordinary_sdk_total_cpu", "ordinary_sdk_native_cpu",
    "ordinary_colour_total_cpu", "ordinary_colour_native_cpu", "ordinary_bloom_selector_cpu",
    "inline_copy_finish_cpu", "ordinary_tracking_and_lock_cpu",
    "scene_left_depth_ready_cpu", "scene_right_depth_ready_cpu",
    "pcl_simulation_start", "pcl_simulation_end", "pcl_render_submit_start", "pcl_render_submit_end",
    "openxr_wait_frame_cpu", "openxr_begin_frame_cpu", "openxr_end_frame_cpu", "openxr_pair_cpu",
}
WAIT_PREFIXES = ("scene_left_", "scene_right_")
WAIT_SUFFIXES = ("_depth_wait_cpu", "_shadow_wait_cpu", "_third_wait_cpu", "_other_wait_cpu")
SETTLE_MS = 3000
# Verdict thresholds (fractions of the median frame interval / of one core).
GPU_BOUND = 0.85
THREAD_BOUND = 0.85
HOOK_BOUND = 0.20
DISPATCH_SHARE = 0.50
DISPATCH_WAITING = 0.60
FENCE_SHARE = 0.25
MACHINE_BUSY = 0.90
# Known native call sites in sampled stacks: reference-build RVA, and the 2026-10-21 port
# (scripts/game_build.py would give the same values from mod/src/GameBuildTable.inc).
KNOWN_FRAMES = {
    "depth-ready fence (ZMIPS_READY_0)": (0x180C195, 0x180C675),
    "scene depth wait": (0x1806490, 0x1806970),
    "scene shadow wait": (0x1806BD3, 0x18070B3),
    "scene third wait": (0x1806FC6, 0x18074A6),
}
WAIT_SYMBOLS = ("NtWait", "ZwWait", "NtDelayExecution", "ZwDelayExecution", "NtRemoveIoCompletion", "ZwRemoveIoCompletion",
                "NtSignalAndWait", "ZwSignalAndWait", "NtAlertThreadByThreadId", "ZwAlertThreadByThreadId",
                "NtWaitForAlertByThreadId", "ZwWaitForAlertByThreadId", "SleepConditionVariable", "NtYieldExecution")
SYSTEM_MODULES = {"ntdll.dll", "kernelbase.dll", "kernel32.dll", "ucrtbase.dll", "vcruntime140.dll", "msvcp140.dll", "win32u.dll"}
GAME_EXE = "warhammer 40000 space marine 2 - retail.exe"


def number(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def median(values):
    return statistics.median(values) if values else None


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def ratio(a, b):
    return a / b if a is not None and b else None


class Trace:
    """Rows of the kept events, each list sorted by start: (start, duration, value, thread)."""

    def __init__(self, rows):
        self.events = defaultdict(list)
        self.waits = defaultdict(list)
        self.threads = defaultdict(Counter)
        self.thread_column = False
        for row in rows:
            event = row.get("event")
            if not event:
                continue
            is_wait = event.startswith(WAIT_PREFIXES) and event.endswith(WAIT_SUFFIXES)
            if event not in SPANS and not is_wait:
                continue
            start, duration, value = number(row.get("qpc_start_ms")), number(row.get("duration_ms")), number(row.get("value"))
            if start is None or duration is None or duration < 0:
                continue
            thread = row.get("thread")
            tid = int(thread) if thread and thread.isdigit() else None
            self.thread_column |= tid is not None
            item = (start, duration, value if value is not None else 0.0, tid)
            (self.waits[event] if is_wait else self.events[event]).append(item)
            if tid is not None and event in ("present_hook", "ordinary_dispatch_native_cpu", "pcl_simulation_start", "pcl_simulation_end"):
                self.threads[event][tid] += 1
        for table in (self.events, self.waits):
            for values in table.values():
                values.sort()
        self.starts = {name: [item[0] for item in values] for name, values in self.events.items()}

    @staticmethod
    def load(path: Path):
        with path.open(encoding="utf-8-sig", newline="") as handle:
            return Trace(csv.DictReader(handle))

    def between(self, name, start, end, table=None):
        values = (table or self.events).get(name, [])
        starts = self.starts.get(name) if table is None else [item[0] for item in values]
        if starts is None:
            return []
        return values[bisect_left(starts, start):bisect_right(starts, end)]

    def role(self, event):
        counts = self.threads.get(event)
        return counts.most_common(1)[0][0] if counts else None

    def extent(self):
        present = self.events.get("present_hook", [])
        return (present[0][0], present[-1][0]) if present else (None, None)

    def unix_offset(self):
        """Unix seconds minus QPC seconds, from the once-a-second wall_clock rows."""
        rows = self.events.get("wall_clock", [])
        return median([value - start / 1000 for start, _, value, _ in rows]) if rows else None


def load_presentmon(path: Path):
    if not path.is_file():
        return None
    frames = []
    with path.open(encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            start = number(row.get("CPUStartQPCTime"))
            if start is None:
                continue
            frames.append((start, {key: number(row.get(column)) for key, column in (
                ("frame", "FrameTime"), ("cpu_busy", "CPUBusy"), ("cpu_wait", "CPUWait"),
                ("gpu_busy", "GPUBusy"), ("gpu_wait", "GPUWait"), ("gpu_latency", "GPULatency"))}))
    frames.sort(key=lambda item: item[0])
    return frames


def load_gpu_log(path: Path):
    """nvidia-smi --format=csv,nounits rows: (unix seconds, utilization percent)."""
    if not path.is_file():
        return None
    rows = []
    with path.open(encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        next(reader, None)
        for fields in reader:
            if len(fields) < 2:
                continue
            try:
                stamp = datetime.strptime(fields[0].strip(), "%Y/%m/%d %H:%M:%S.%f").timestamp()
            except ValueError:
                continue
            value = number(fields[1].strip())
            if value is not None:
                rows.append((stamp, value))
    return rows


def load_sampler(path: Path):
    """Samples: (qpc_ms, suspended_ms, frames=[(module, rva, symbol, path)])."""
    if not path.is_file():
        return None
    samples = {}
    with path.open(encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            key = row.get("sample")
            qpc, suspended = number(row.get("qpc_ms")), number(row.get("suspended_ms"))
            if key is None or qpc is None:
                continue
            entry = samples.setdefault(key, [qpc, suspended or 0.0, [], row.get("error") not in (None, "", "0")])
            if row.get("frame") not in (None, ""):
                rva = row.get("rva") or "0x0"
                entry[2].append(((row.get("module") or "").lower(), int(rva, 16), row.get("symbol") or "", (row.get("path") or "").lower()))
    return sorted((tuple(value) for value in samples.values()), key=lambda item: item[0])


def classify_sample(frames, game_dir=None):
    """('blocked', on) or ('running', where) for one stack, innermost frame first."""
    if not frames:
        return ("unknown", "no frames")
    top = frames[0]
    if top[0] == "ntdll.dll" and top[2].startswith(WAIT_SYMBOLS) or any(f[2].startswith(("SleepConditionVariable", "WaitForSingleObject", "WaitForMultipleObjects")) for f in frames[:3]):
        for name, rvas in KNOWN_FRAMES.items():
            if any(f[0] == GAME_EXE and f[1] in rvas for f in frames):
                return ("blocked", name)
        if any("present" in f[2].lower() for f in frames[:12]):
            return ("blocked", "Present")
        for module, _, _, path in frames:
            if module not in SYSTEM_MODULES:
                return ("blocked", "mod (dxgi.dll proxy)" if is_mod(module, path, game_dir) else module)
        return ("blocked", "system")
    for module, _, _, path in frames:
        if module in SYSTEM_MODULES:
            continue
        if is_mod(module, path, game_dir):
            return ("running", "mod (dxgi.dll proxy)")
        if module == GAME_EXE:
            return ("running", "game code")
        if module.startswith(("nvwgf2um", "nvldumd", "d3d12", "nvgpucomp", "amdxc")):
            return ("running", "GPU driver / D3D12")
        if module.startswith(("sl.", "nvngx")):
            return ("running", "DLSS / Streamline")
        return ("running", module)
    return ("running", "system")


def is_mod(module, path, game_dir):
    # The mod is dxgi.dll in the game's bin folder; the system DXGI lives in System32.
    return module == "dxgi.dll" and bool(path) and "system32" not in path and (game_dir is None or path.startswith(game_dir))


def sampler_summary(samples, start, end):
    if samples is None:
        return None
    chosen = [s for s in samples if start <= s[0] <= end and not s[3]]
    if not chosen:
        return {"samples": 0}
    states = Counter(classify_sample(s[2]) for s in chosen)
    blocked = sum(count for (state, _), count in states.items() if state == "blocked")
    return {"samples": len(chosen), "blocked_fraction": blocked / len(chosen),
            "states": {f"{state}: {where}": round(count / len(chosen), 4) for (state, where), count in states.most_common(8)},
            "suspended_ms_per_s": 1000 * sum(s[1] for s in chosen) / (end - start)}


def per_frame(trace, names, start, end, frames, table=None):
    if not frames:
        return None
    return sum(item[1] for name in names for item in trace.between(name, start, end, table)) / frames


def thread_fraction(trace, name, start, end):
    """CPU used by one role's thread over the window, as a fraction of one core."""
    rows = [r for r in trace.between(name, start, end) if r[0] + r[1] <= end]
    wall = sum(r[1] for r in rows)
    return sum(r[2] for r in rows) / wall if wall else None


def simulation_ms(trace, start, end):
    """PCL simulation start (raw token N) to simulation end (token N+1), per frame."""
    ends = {}
    for stamp, _, token, _ in trace.between("pcl_simulation_end", start, end + 1000):
        ends.setdefault(int(token), stamp)
    durations = []
    for stamp, _, token, _ in trace.between("pcl_simulation_start", start, end):
        finish = ends.get((int(token) + 1) & 0xFFFFFFFF)
        if finish is not None and 0 <= finish - stamp < 1000:
            durations.append(finish - stamp)
    return median(durations)


def render_submit_ms(trace, start, end):
    begins = {}
    for stamp, _, token, _ in trace.between("pcl_render_submit_start", start, end):
        begins.setdefault(int(token), stamp)
    durations = [stamp - begins[int(token)] for stamp, _, token, _ in trace.between("pcl_render_submit_end", start, end + 1000)
                 if int(token) in begins and 0 <= stamp - begins[int(token)] < 1000]
    return median(durations)


def window_metrics(trace, start, end, presentmon=None, gpu_log=None, samplers=None, unix_offset=None):
    seconds = (end - start) / 1000
    presents = trace.between("present_hook", start, end)
    intervals = [b[0] - a[0] for a, b in zip(presents, presents[1:])]
    frame_ms = median(intervals)
    dispatches = trace.between("ordinary_dispatch_native_cpu", start, end)
    frames = len(dispatches)
    pairs = trace.between("pair_submitted", start, end)
    metrics = {
        "seconds": round(seconds, 3),
        "game_fps": len(presents) / seconds if seconds else None,
        "frame_ms": {"median": frame_ms, "p90": percentile(intervals, .9)},
        "fresh_pairs_per_s": len(pairs) / seconds if seconds else None,
        "pose_age_ms_median": median([p[2] for p in pairs]),
        "dispatches_per_s": frames / seconds if seconds else None,
    }
    # GPU: PresentMon per frame, nvidia-smi once a second.
    gpu = None
    if presentmon is not None:
        chosen = [m for stamp, m in presentmon if start <= stamp <= end]
        def column(key):
            return [m[key] for m in chosen if m.get(key) is not None and m[key] >= 0]
        gpu = {"frames": len(chosen), "frame_ms_median": median(column("frame")), "gpu_busy_ms_median": median(column("gpu_busy")),
               "gpu_wait_ms_median": median(column("gpu_wait")), "cpu_busy_ms_median": median(column("cpu_busy")),
               "cpu_wait_ms_median": median(column("cpu_wait"))}
        gpu["gpu_busy_fraction"] = ratio(gpu["gpu_busy_ms_median"], gpu["frame_ms_median"])
    utilization = None
    if gpu_log is not None and unix_offset is not None:
        values = [v for stamp, v in gpu_log if start / 1000 + unix_offset <= stamp <= end / 1000 + unix_offset]
        utilization = statistics.fmean(values) / 100 if values else None
    metrics["gpu"] = gpu
    metrics["gpu_utilization_nvidia_smi"] = utilization
    # Threads.
    metrics["threads"] = {
        "present_tid": trace.role("present_hook"), "render_tid": trace.role("ordinary_dispatch_native_cpu"),
        # Simulation starts in jobs on rotating workers and ends on one game thread.
        "simulation_tid": trace.role("pcl_simulation_end"),
        "simulation_start_threads": len(trace.threads.get("pcl_simulation_start", {})),
        "simulation_cpu_fraction": thread_fraction(trace, "thread_cpu_simulation", start, end),
        "render_cpu_fraction": thread_fraction(trace, "thread_cpu_render", start, end),
        "present_cpu_fraction": thread_fraction(trace, "thread_cpu_present", start, end),
        "process_cores": thread_fraction(trace, "process_cpu", start, end),
        "machine_busy_fraction": median([r[2] for r in trace.between("system_cpu_busy", start, end)]),
        "simulation_ms_median": simulation_ms(trace, start, end),
        "render_submit_ms_median": render_submit_ms(trace, start, end),
    }
    t = metrics["threads"]
    named = [t["render_cpu_fraction"], t["simulation_cpu_fraction"]] + ([t["present_cpu_fraction"]] if t["present_tid"] != t["render_tid"] else [])
    t["other_threads_cores"] = t["process_cores"] - sum(named) if t["process_cores"] is not None and None not in named else None
    # The native two-view dispatch on the render thread, and inside it.
    dispatch_cpu = trace.between("ordinary_dispatch_thread_cpu", start, end)
    dispatch_wall = median([d[1] for d in dispatches])
    cpu_sum, wall_sum = sum(d[1] for d in dispatch_cpu), sum(d[2] for d in dispatch_cpu)
    waits = sorted(trace.waits)
    metrics["dispatch"] = {
        "wall_ms_median": dispatch_wall, "share_of_frame": ratio(dispatch_wall, frame_ms),
        "thread_cpu_ms_per_frame": cpu_sum / len(dispatch_cpu) if dispatch_cpu else None,
        "thread_cpu_of_wall": cpu_sum / wall_sum if wall_sum else None,
        "left_scene_ms": per_frame(trace, ["ordinary_scene_left_cpu"], start, end, frames),
        "right_scene_ms": per_frame(trace, ["ordinary_scene_right_cpu"], start, end, frames),
        "left_scene_thread_cpu_ms": per_frame(trace, ["ordinary_scene_left_thread_cpu"], start, end, frames),
        "right_scene_thread_cpu_ms": per_frame(trace, ["ordinary_scene_right_thread_cpu"], start, end, frames),
        "depth_ready_fence_ms": per_frame(trace, ["scene_left_depth_ready_cpu", "scene_right_depth_ready_cpu"], start, end, frames),
        "scene_waits_ms": {name: per_frame(trace, [name], start, end, frames, trace.waits) for name in waits},
    }
    fence = (metrics["dispatch"]["depth_ready_fence_ms"] or 0) + sum(v or 0 for v in metrics["dispatch"]["scene_waits_ms"].values())
    metrics["dispatch"]["native_fences_ms"] = fence if frames else None
    # The mod's own time per frame: the Present hook plus each wrapper's time outside its native call.
    overheads = {"present_hook": per_frame(trace, ["present_hook"], start, end, frames)}
    for total, native in (("ordinary_dispatch_total_cpu", "ordinary_dispatch_native_cpu"), ("ordinary_temporal_total_cpu", "ordinary_temporal_native_cpu"),
                          ("ordinary_sdk_total_cpu", "ordinary_sdk_native_cpu"), ("ordinary_colour_total_cpu", "ordinary_colour_native_cpu")):
        a, b = per_frame(trace, [total], start, end, frames), per_frame(trace, [native], start, end, frames)
        overheads[total.replace("_total_cpu", "_wrapper")] = a - b if a is not None and b is not None else None
    for name in ("ordinary_bloom_selector_cpu", "inline_copy_finish_cpu", "ordinary_tracking_and_lock_cpu"):
        overheads[name] = per_frame(trace, [name], start, end, frames)
    hook = sum(v for v in overheads.values() if v) if frames else None
    # The wrappers below run inside the native dispatch's wall time; take them out for the game's own share.
    nested = sum(overheads[name] or 0 for name in ("ordinary_temporal_wrapper", "ordinary_sdk_wrapper", "ordinary_colour_wrapper",
                                                    "ordinary_bloom_selector_cpu", "inline_copy_finish_cpu"))
    metrics["dispatch"]["native_excluding_mod_ms"] = dispatch_wall - nested if dispatch_wall is not None else None
    metrics["mod_hooks"] = {"ms_per_frame": hook, "share_of_frame": ratio(hook, frame_ms), "parts_ms": overheads,
                            "openxr_ms": {name: per_frame(trace, [name], start, end, frames) for name in
                                          ("openxr_wait_frame_cpu", "openxr_begin_frame_cpu", "openxr_end_frame_cpu", "openxr_pair_cpu")}}
    # The profiler's own cost.
    writer = trace.between("trace_writer_cpu", start, end)
    writer_wall = sum(r[1] for r in writer)
    metrics["profiler"] = {"trace_writer_ms_per_s": 1000 * sum(r[2] for r in writer) / writer_wall if writer_wall else None,
                           "trace_rows_per_s": 1000 * sum(r[2] for r in trace.between("trace_rows", start, end)) / writer_wall if writer_wall else None}
    for role, samples in (samplers or {}).items():
        metrics.setdefault("stacks", {})[role] = sampler_summary(samples, start, end)
    return metrics


def verdict(m):
    """(code, sentence) from one window's metrics. Rules, first match wins:
    hook share >= 20% of the frame -> mod; PresentMon GPU busy >= 85% of the frame (or nvidia-smi
    utilization when PresentMon is missing) -> gpu; render thread >= 85% of a core -> render_cpu;
    simulation thread >= 85% -> simulation_cpu; dispatch >= 50% of the frame with its thread
    running < 60% of it -> waiting (fences >= 25% of the frame -> fence; machine >= 90% busy ->
    workers; else wait_unknown); otherwise unclear."""
    frame = m["frame_ms"]["median"]
    if not frame:
        return ("no_data", "no gameplay frames in this window")
    hook = m["mod_hooks"]["share_of_frame"]
    gpu = (m["gpu"] or {}).get("gpu_busy_fraction")
    gpu_source = "PresentMon GPU busy"
    if gpu is None and m["gpu_utilization_nvidia_smi"] is not None:
        gpu, gpu_source = m["gpu_utilization_nvidia_smi"], "nvidia-smi utilization"
    threads, dispatch = m["threads"], m["dispatch"]
    render, simulation = threads["render_cpu_fraction"], threads["simulation_cpu_fraction"]
    share, running = dispatch["share_of_frame"], dispatch["thread_cpu_of_wall"]
    fences = ratio(dispatch["native_fences_ms"], frame)

    def pct(value):
        return f"{100 * value:.0f}%"
    if hook is not None and hook >= HOOK_BOUND:
        return ("mod", f"mod hook overhead: our hooks take {pct(hook)} of the {frame:.1f} ms frame")
    if gpu is not None and gpu >= GPU_BOUND:
        return ("gpu", f"GPU-bound: {gpu_source} {pct(gpu)} of the {frame:.1f} ms frame")
    if render is not None and render >= THREAD_BOUND:
        return ("render_cpu", f"CPU-bound render thread: {pct(render)} of a core (two native views' culling and draw submission)")
    if simulation is not None and simulation >= THREAD_BOUND:
        return ("simulation_cpu", f"CPU-bound game thread: simulation thread {pct(simulation)} of a core")
    if share is not None and running is not None and share >= DISPATCH_SHARE and running < DISPATCH_WAITING:
        if fences is not None and fences >= FENCE_SHARE:
            return ("fence", f"waiting on native GPU fences: {dispatch['native_fences_ms']:.1f} ms of each {frame:.1f} ms frame (depth-ready and scene waits) inside the dispatch")
        if threads["machine_busy_fraction"] is not None and threads["machine_busy_fraction"] >= MACHINE_BUSY:
            return ("workers", f"CPU-bound worker threads: the dispatch waits ({pct(1 - running)} of it off-CPU) while the machine is {pct(threads['machine_busy_fraction'])} busy")
        return ("wait_unknown", f"waiting inside the dispatch on something other than the known fences: its thread runs {pct(running)} of {dispatch['wall_ms_median']:.1f} ms")
    missing = [name for name, value in (("GPU busy", gpu), ("render thread CPU", render), ("simulation thread CPU", simulation)) if value is None]
    return ("unclear", "no single limit" + (f" (missing: {', '.join(missing)})" if missing else ""))


def second_states(trace):
    """Per trace second: (start_ms, mode) with mode stereo, mono or None (not comparable gameplay)."""
    begin, finish = trace.extent()
    if begin is None:
        return []
    count = int((finish - begin) // 1000) + 1
    gameplay = [True] * count
    seen = [False] * count
    for start, _, value, _ in trace.events.get("present_hook", []):
        index = int((start - begin) // 1000)
        seen[index] = True
        if value != 1:
            gameplay[index] = False
    pairs = [0] * count
    for start, _, _, _ in trace.events.get("pair_submitted", []):
        index = int((start - begin) // 1000)
        if 0 <= index < count:
            pairs[index] += 1
    hold_rows = trace.events.get("stereo_hold", [])
    hold_starts = [r[0] for r in hold_rows]
    states = []
    def held_at(stamp):
        position = bisect_right(hold_starts, stamp) - 1
        return hold_rows[position][2] == 1 if position >= 0 else False
    for index in range(count):
        at = begin + index * 1000
        held = held_at(at)
        mode = None
        # A second in which the hold changed belongs to neither mode.
        if seen[index] and gameplay[index] and held == held_at(at + 999):
            mode = "mono" if held else "stereo" if pairs[index] else None
        states.append((at, mode))
    return states


def segments(trace, min_seconds=20, start=None, end=None):
    """Settled runs of one mode: [(start_ms, end_ms, mode)]."""
    result, current = [], None
    for at, mode in second_states(trace):
        if (start is not None and at < start) or (end is not None and at + 1000 > end):
            mode = None
        if current and mode == current[2] and at == current[1]:
            current[1] = at + 1000
            continue
        if current:
            result.append(tuple(current))
        current = [at, at + 1000, mode] if mode else None
    if current:
        result.append(tuple(current))
    settled = [(a + SETTLE_MS, b, mode) for a, b, mode in result]
    return [(a, b, mode) for a, b, mode in settled if b - a >= min_seconds * 1000]


def parse_window(text, trace):
    span, _, label = text.partition("=")
    first, _, last = span.partition("-")
    offset = trace.unix_offset()
    begin, _ = trace.extent()
    if offset is None or begin is None:
        raise ValueError("--window needs wall_clock rows (a current -Profile trace)")
    day = datetime.fromtimestamp(begin / 1000 + offset)

    def qpc(clock):
        moment = datetime.strptime(clock, "%H:%M:%S").replace(year=day.year, month=day.month, day=day.day)
        value = (moment.timestamp() - offset) * 1000
        return value + 86400000 if value < begin - 3600000 else value
    a, b = qpc(first), qpc(last)
    if b <= a:
        raise ValueError(f"window {text} ends before it starts")
    return a, b, label or span


def local_clock(qpc, offset):
    return datetime.fromtimestamp(qpc / 1000 + offset).strftime("%H:%M:%S") if offset is not None else None


def analyze(directory: Path, windows=(), min_seconds=20):
    trace = Trace.load(directory / "cpu_trace.csv")
    presentmon = load_presentmon(directory / "presentmon.csv")
    gpu_log = load_gpu_log(directory / "gpu.csv")
    samplers = {role: load_sampler(directory / f"sampler_{role}.csv") for role in ("render", "simulation")}
    samplers = {role: value for role, value in samplers.items() if value is not None}
    offset = trace.unix_offset()
    chosen = []
    if windows:
        for text in windows:
            a, b, label = parse_window(text, trace)
            chosen += [(x, y, mode, label) for x, y, mode in segments(trace, min_seconds, a, b)]
    else:
        chosen = [(a, b, mode, None) for a, b, mode in segments(trace, min_seconds)]
    report = {"directory": str(directory), "thread_column": trace.thread_column,
              "inputs": {"presentmon": presentmon is not None, "gpu_log": gpu_log is not None, "samplers": sorted(samplers)},
              "windows": []}
    for a, b, mode, label in chosen:
        metrics = window_metrics(trace, a, b, presentmon, gpu_log, samplers, offset)
        code, sentence = verdict(metrics)
        report["windows"].append({"label": label, "mode": mode, "qpc_start_ms": a, "qpc_end_ms": b,
                                  "local_start": local_clock(a, offset), "local_end": local_clock(b, offset),
                                  "verdict": code, "summary": sentence, "metrics": metrics})
    stereo = [w for w in report["windows"] if w["mode"] == "stereo"]
    mono = [w for w in report["windows"] if w["mode"] == "mono"]
    if stereo and mono:
        def weighted(items, key):
            total = sum(w["metrics"]["seconds"] for w in items)
            return sum(w["metrics"][key] * w["metrics"]["seconds"] for w in items if w["metrics"][key] is not None) / total
        flat, vr = weighted(mono, "game_fps"), weighted(stereo, "fresh_pairs_per_s")
        report["flat_vs_vr"] = {"flat_fps": flat, "vr_pairs_per_s": vr, "vr_over_flat": vr / flat if flat else None,
                                "note": "time-weighted over all mono and stereo windows; compare adjacent windows for the same spot"}
    return report


def fmt(value, digits=1, scale=1.0, suffix=""):
    return "-" if value is None else f"{value * scale:.{digits}f}{suffix}"


def table(report):
    lines = ["| window | mode | s | pairs/s | game fps | frame ms | GPU busy | sim CPU | render CPU | dispatch ms (CPU) | fences ms | L/R eye ms | hooks ms | verdict |",
             "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for w in report["windows"]:
        m = w["metrics"]
        gpu = (m["gpu"] or {}).get("gpu_busy_fraction")
        if gpu is None and m["gpu_utilization_nvidia_smi"] is not None:
            gpu = m["gpu_utilization_nvidia_smi"]
        d, t = m["dispatch"], m["threads"]
        lines.append(f"| {w['label'] or ''} {w['local_start'] or ''}-{w['local_end'] or ''} | {w['mode']} | {m['seconds']:.0f} | {fmt(m['fresh_pairs_per_s'])} | {fmt(m['game_fps'])} | "
                     f"{fmt(m['frame_ms']['median'])} | {fmt(gpu, 0, 100, '%')} | {fmt(t['simulation_cpu_fraction'], 0, 100, '%')} | {fmt(t['render_cpu_fraction'], 0, 100, '%')} | "
                     f"{fmt(d['wall_ms_median'])} ({fmt(d['thread_cpu_ms_per_frame'])}) | {fmt(d['native_fences_ms'])} | {fmt(d['left_scene_ms'])}/{fmt(d['right_scene_ms'])} | "
                     f"{fmt(m['mod_hooks']['ms_per_frame'])} | {w['summary']} |")
    if "flat_vs_vr" in report:
        f = report["flat_vs_vr"]
        lines.append(f"\nflat {f['flat_fps']:.1f} fps vs VR {f['vr_pairs_per_s']:.1f} pairs/s (VR/flat {f['vr_over_flat']:.2f})")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", type=Path)
    parser.add_argument("--window", action="append", default=[], help="HH:MM:SS-HH:MM:SS[=label], local wall-clock time")
    parser.add_argument("--min-seconds", type=float, default=20)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    if not args.directory.is_absolute() or not (args.directory / "cpu_trace.csv").is_file():
        parser.error("directory must be an absolute path holding cpu_trace.csv")
    report = analyze(args.directory, args.window, args.min_seconds)
    if args.out:
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(table(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
