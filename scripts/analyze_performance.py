"""Summarize a single warmed-up PresentMon capture, with optional QPC-aligned CPU spans.

Present intervals measure desktop game delivery, not headset refresh or motion-to-photon.
GPU busy is ETW process attribution; CPU spans are elapsed wall time, including waits.
"""
import argparse
import csv
import json
import math
from collections import Counter, defaultdict
from pathlib import Path


def number(value):
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (ValueError, TypeError):
        return None


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    def percentile(p):
        index = (len(values)-1)*p
        lo = int(index)
        return values[lo]+(values[min(lo+1, len(values)-1)]-values[lo])*(index-lo)
    return {"count": len(values), "mean": sum(values)/len(values), "p50": percentile(.5),
            "p95": percentile(.95), "p99": percentile(.99), "max": values[-1]}


def analyze(present, cpu=None, mode=None, require_gameplay=False, require_render_path=None):
    if require_gameplay and cpu is None:
        raise ValueError("Gameplay validation requires the matching CPU trace")
    if require_render_path and cpu is None:
        raise ValueError("Render-path validation requires the matching CPU trace")
    rows = list(csv.DictReader(present))
    if not rows or "CPUStartQPCTime" not in rows[0]:
        raise ValueError("Expected PresentMon --qpc_time_ms (CPUStartQPCTime in milliseconds)")
    pids = {row["ProcessID"] for row in rows}
    if len(pids) != 1:
        raise ValueError("Capture must target exactly one process")
    chains = Counter(row["SwapChainAddress"] for row in rows)
    chain = chains.most_common(1)[0][0]
    selected = [row for row in rows if row["SwapChainAddress"] == chain]
    times = [number(row["CPUStartQPCTime"]) for row in selected]
    if len(times) < 30 or None in times or any(b <= a for a, b in zip(times, times[1:])):
        raise ValueError("Need at least 30 frames with finite, strictly increasing QPC times")
    intervals = [b-a for a, b in zip(times, times[1:])]
    elapsed = times[-1]-times[0]
    result = {"process_id": next(iter(pids)), "swapchain": chain,
              "discarded_other_swapchain_rows": len(rows)-len(selected), "frames": len(times),
              "qpc_start_ms": times[0], "qpc_end_ms": times[-1], "duration_s": elapsed/1000,
              "game_present_fps": 1000*len(intervals)/elapsed,
              "present_interval_ms": distribution(intervals),
              "p99_equivalent_fps": 1000/distribution(intervals)["p99"],
              "over_22_22ms_percent": 100*sum(v > 1000/45 for v in intervals)/len(intervals),
              "present_modes": dict(Counter(row.get("PresentMode", "unknown") for row in selected)),
              "sync_intervals": dict(Counter(row.get("SyncInterval", "unknown") for row in selected)),
              "frame_types": dict(Counter(row.get("FrameType", "unavailable") for row in selected)),
              "metrics_ms": {}}
    # PresentMon 2.x and 3.x names differ; preserve missing metrics rather than reporting zero.
    for metric, names in {"gpu_busy": ("GPUBusy", "MsGPUBusy"), "gpu_time": ("GPUTime", "MsGPUTime"),
                          "cpu_busy": ("CPUBusy", "MsCPUBusy"), "cpu_wait": ("CPUWait", "MsCPUWait"),
                          "gpu_latency": ("GPULatency", "MsGPULatency"),
                          "present_api": ("MsInPresentAPI",)}.items():
        column = next((name for name in names if name in selected[0]), None)
        values = [number(row[column]) for row in selected] if column else []
        result["metrics_ms"][metric] = distribution(v for v in values if v is not None and v >= 0)
    if cpu is not None:
        spans = defaultdict(list)
        values = defaultdict(list)
        present_modes = Counter()
        for row in csv.DictReader(cpu):
            start = number(row.get("qpc_start_ms"))
            # The app can still be appending a later row to this buffered file. Only
            # validate spans in the already completed PresentMon capture window.
            if start is not None and (start < times[0] or start > times[-1]):
                continue
            duration = number(row.get("duration_ms"))
            if start is None or duration is None or duration < 0:
                raise ValueError("Invalid CPU span")
            if start < times[0] or start+duration > times[-1]:
                continue
            event = row["event"]
            spans[event].append(duration)
            value = number(row.get("value"))
            if value is not None:
                values[event].append(value)
            if event == "present_hook":
                present_modes[row["mode"]] += 1
        if not present_modes or (mode and set(present_modes) != {mode}):
            raise ValueError(f"CPU trace absent or benchmark mode changed during capture: {dict(present_modes)}")
        # Both streams observe one Present per game frame. Allow boundary frames
        # crossing the window, but never qualify a trace stopped by its row cap.
        if require_gameplay and len(spans["present_hook"]) < len(times)-3:
            raise ValueError("CPU trace does not cover the complete PresentMon window")
        # Value bits: foreground=1, paused=2, cinematic=4, generated-present gate=8,
        # observed campaign screen menu=16, unavailable native state=32.
        if require_gameplay and (len(values["present_hook"]) != len(spans["present_hook"]) or
                                 any(value != 1 for value in values["present_hook"])):
            raise ValueError("Capture includes unfocused, paused, cinematic, screen-menu, unknown-state or frame-generation-blocked frames")
        result["benchmark_modes"] = dict(present_modes)
        ordinary = bool(spans.get("ordinary_present_bridge_cpu"))
        afr = bool(spans.get("capture_eye_cpu") or spans.get("eye_copied"))
        render_path = ("mixed" if ordinary and afr else "single_frame" if ordinary else
                       "afr" if afr else "unknown")
        if require_render_path and render_path != require_render_path:
            raise ValueError(f"Expected {require_render_path} render path, observed {render_path}")
        result["render_path"] = render_path
        result["cpu_spans_ms"] = {key: distribution(value) for key, value in spans.items()}
        result["cpu_span_duty_percent"] = {key: 100*sum(value)/elapsed for key, value in spans.items()}
        pairs = len(spans.get("pair_submitted", []))
        result["submitted_pair_count"] = pairs
        result["fresh_pairs_per_second"] = pairs*1000/elapsed
        result["eye_copies_per_second"] = (None if render_path == "single_frame" and not spans.get("eye_copied") else
                                          len(spans.get("eye_copied", []))*1000/elapsed)
        result["older_eye_pose_age_at_submit_end_ms"] = distribution(values.get("pair_submitted", []))
        result["observations"] = {key: dict(Counter(values[key])) for key in (
            "hmd_activity", "hmd_refresh_hz", "present_hook", "xr_session_state", "xr_should_render", "xr_predicted_refresh_hz", "xr_user_present")}
        if require_gameplay and mode == "full" and any(key.startswith(("openxr_", "xr_")) for key in spans):
            if (not values["xr_should_render"] or any(value != 1 for value in values["xr_should_render"]) or
                    len(values["xr_should_render"]) != len(spans["xr_should_render"]) or
                    not values["xr_session_state"] or len(values["xr_session_state"]) != len(spans["xr_session_state"]) or
                    any(value not in (4, 5) for value in values["xr_session_state"])):
                raise ValueError("OpenXR capture includes missing visibility or do-not-render frames")
        if require_gameplay and mode == "full" and pairs < 10:
            raise ValueError("Full runtime capture lacks successful stereo submissions")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--cpu", type=Path)
    parser.add_argument("--mode", choices=("tracking", "copy", "full"))
    parser.add_argument("--require-gameplay", action="store_true")
    parser.add_argument("--render-path", choices=("afr", "single_frame"))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    with args.csv.open(encoding="utf-8-sig", newline="") as present:
        if args.cpu:
            with args.cpu.open(encoding="utf-8-sig", newline="") as cpu:
                result = analyze(present, cpu, args.mode, args.require_gameplay, args.render_path)
        else:
            result = analyze(present, None, args.mode, args.require_gameplay, args.render_path)
    args.out.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
