"""Analyze passive native-phase and completed GPU-copy probes in a qualified window.

Durations overlap: CPU spans include waits; GPU completion-to-poll measures when
the CPU observed completion, not when the GPU finished. No motion-to-photon claim.
"""
import argparse
import csv
import json
import math
from collections import defaultdict
from bisect import bisect_left
from pathlib import Path
from analyze_performance import distribution


def analyze(rows, start, end):
    if not all(math.isfinite(v) for v in (start, end)) or end <= start:
        raise ValueError("finite increasing QPC window required")
    events = defaultdict(list)
    for row in rows:
        stamp = float(row["qpc_start_ms"])
        if not math.isfinite(stamp):
            raise ValueError("nonfinite event time")
        if not start <= stamp <= end:
            continue
        duration, value = float(row["duration_ms"]), float(row["value"])
        if not all(math.isfinite(v) for v in (duration, value)) or duration < 0:
            raise ValueError("invalid probe duration/value")
        if stamp+duration <= end:
            events[row["event"]].append((stamp, duration, value))

    def index(name):
        result = {}
        for stamp, duration, value in events[name]:
            if value != int(value) or int(value) in result:
                raise ValueError(f"non-integral/duplicate identity: {name}")
            result[int(value)] = (stamp, duration)
        return result

    # Only exact bridge timestamp joins map render IDs to native dispatch IDs.
    bridge_dispatch = {stamp: int(value) for stamp, _, value in events["ordinary_bridge_dispatch"]}
    dispatch_to_render = {}
    for stamp, _, render in events["ordinary_bridge_begin"]:
        if stamp in bridge_dispatch:
            dispatch = bridge_dispatch[stamp]
            if dispatch in dispatch_to_render or render != int(render):
                raise ValueError("duplicate/non-integral bridge mapping")
            dispatch_to_render[dispatch] = int(render)
    appended, submitted, observed = (index(name) for name in
        ("inline_copy_appended", "inline_copy_submitted", "inline_copy_completion_observed"))
    accepted, pending = index("ordinary_submit_accepted_cpu"), index("ordinary_bridge_pending_cpu")
    verified = index("ordinary_pair_cpu_verified")
    gpu = [index("bridge_left_gpu"), index("bridge_right_gpu")]
    intervals = defaultdict(list)
    pairs = []
    incomplete = 0
    for dispatch, (accept, accept_duration) in accepted.items():
        render = dispatch_to_render.get(dispatch)
        tags = [dispatch*2, dispatch*2+1]
        if (render is None or dispatch not in pending or
                any(tag not in appended or tag not in submitted or tag not in observed for tag in tags) or
                any(render not in eye for eye in gpu)):
            incomplete += 1
            continue
        bridge_begin, bridge_duration = pending[dispatch]
        gpu_end = max(eye[render][0]+eye[render][1] for eye in gpu)
        joined = {"native_last_append_to_bridge_ms": bridge_begin-max(appended[t][0] for t in tags),
                  "native_last_submit_to_observed_ms": max(observed[t][0] for t in tags)-max(submitted[t][0] for t in tags),
                  "native_observed_to_bridge_ms": bridge_begin-max(observed[t][0] for t in tags),
                  "bridge_pending_ms": bridge_duration,
                  "bridge_gpu_end_to_submit_ms": accept-gpu_end,
                  "last_append_to_submit_end_ms": accept+accept_duration-max(appended[t][0] for t in tags)}
        if dispatch in verified:
            joined["native_last_append_to_cpu_verified_ms"] = verified[dispatch][0]-max(appended[t][0] for t in tags)
            joined["cpu_verified_to_bridge_ms"] = bridge_begin-verified[dispatch][0]
        # A queue-ordered bridge may enqueue before the CPU observes the native
        # fence. That observation interval is signed, not a GPU ordering failure.
        if any(value < -.1 for name, value in joined.items() if name != "native_observed_to_bridge_ms"):
            raise ValueError("joined timeline reversed beyond 0.1ms calibration tolerance")
        for name, value in joined.items():
            intervals[name].append(value)
        for eye, tag in enumerate(tags):
            delay = submitted[tag][0]-appended[tag][0]
            if delay < 0:
                raise ValueError("native submission precedes appended copy")
            intervals[f"inline_{eye}_append_to_submit_ms"].append(delay)
        pairs.append({"dispatch": dispatch, "render_id": render})
    selected = {name: distribution(item[1] for item in values) for name, values in events.items()
                if name.startswith(("scene_", "shadow_prepare_", "bridge_", "ordinary_scene_")) and values}
    # Existing SDK scopes provide an additional landmark inside the final
    # coarse native phase, without adding another native detour.
    sdk = sorted(events["ordinary_sdk_total_cpu"])
    sdk_starts = [item[0] for item in sdk]
    pre_upscale = {}
    execution = {}
    for eye in ("left", "right"):
        before, after, incomplete_phases = [], [], 0
        for stamp, duration, _ in events[f"scene_{eye}_after_third_cpu"]:
            first = bisect_left(sdk_starts, stamp)
            last = bisect_left(sdk_starts, stamp+duration)
            if last-first != 1 or sdk[first][0]+sdk[first][1] > stamp+duration:
                incomplete_phases += 1
                continue
            before.append(sdk[first][0]-stamp)
            after.append(stamp+duration-sdk[first][0])
        pre_upscale[eye] = {"third_wait_end_to_sdk_ms": distribution(before),
                            "sdk_start_to_scene_end_ms": distribution(after),
                            "unpartitioned_phases": incomplete_phases}
        wall, cpu = index(f"scene_{eye}_pre_sdk_wall"), index(f"scene_{eye}_pre_sdk_thread_cpu")
        matched = sorted(wall.keys() & cpu.keys())
        if any(wall[key][0] != cpu[key][0] for key in matched):
            raise ValueError("thread accounting and wall interval have different starts")
        wall_values, cpu_values = [wall[key][1] for key in matched], [cpu[key][1] for key in matched]
        execution[eye] = {"matched_intervals": len(matched), "wall_ms": distribution(wall_values),
                          "thread_cpu_ms": distribution(cpu_values),
                          "aggregate_running_fraction": sum(cpu_values)/sum(wall_values) if sum(wall_values) else None,
                          "accounting_quantized": True}
    return {"qpc_start_ms": start, "qpc_end_ms": end, "joined_pairs": len(pairs),
            "incomplete_accepted_pairs": incomplete, "accepted_pairs": len(accepted),
            "refused_pairs": len(events["ordinary_submit_refused_cpu"]),
            "gpu_probe_errors": sum(len(events[name]) for name in ("bridge_gpu_probe_unavailable", "bridge_gpu_probe_read_failed")),
            "intervals_ms": {name: distribution(values) for name, values in intervals.items()},
            "events": selected,
            "pre_upscale": pre_upscale,
            "pre_upscale_execution": execution,
            "handoff_modes": sorted(set(item[2] for item in events["ordinary_handoff_mode"])),
            "isolated_bridge_values": sorted(set(item[2] for item in events["ordinary_bridge_isolated"])),
            "submit_points": {name: sum(item[2] == point for item in events["ordinary_submit_point"])
                              for point, name in enumerate(("present", "post_frame", "post_present"))},
            "bridge_points": {name: sum(item[2] == point for item in events["ordinary_bridge_point"])
                              for point, name in enumerate(("present", "dispatch_complete"))},
            "shadow_refresh_values": {name: sorted(set(item[2] for item in events[name])) for name in
                                      ("shadow_prepare_left_cpu", "shadow_prepare_right_cpu")},
            "phase_complete": {eye: len(events[f"scene_{eye}_phase_complete"]) for eye in ("left", "right")}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("summary", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    window = json.loads(args.summary.read_text(encoding="utf-8-sig"))
    with args.trace.open(encoding="utf-8-sig", newline="") as source:
        result = analyze(csv.DictReader(source), window["qpc_start_ms"], window["qpc_end_ms"])
    args.output.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("joined_pairs", "incomplete_accepted_pairs", "gpu_probe_errors", "phase_complete")}))


if __name__ == "__main__":
    main()
