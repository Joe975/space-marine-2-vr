"""What does the render thread's depth-ready (ZMIPS_READY_0) wait wait for? From a -QueueTimeline trace.

    python -I scripts/analyze_depth_ready_wait.py ABS_cpu_trace.csv [--eye left|right] [--start QPC --end QPC] [--out ABS.json]

For each depth-ready wait of the eye (scene_<eye>_depth_ready_cpu, -PerformanceProbes):
- the fence and value the render thread registered inside it (fence_cpu_wait on the same thread);
- the queued Signal of that value: which queue, which thread, and how long before the wait it was enqueued;
- when the GPU reached it (fence_completed), against the wait's end;
- on that queue, between the previous completed Signal and this one: GPU-side waits (queue_wait) and when
  their fence values were signalled (queued or from the CPU) and completed, and the submissions.
A GPU wait whose fence value is signalled late is a stall; without one, the queue was working through
earlier submissions (backlog). See GpuQueueTimeline.h for the row packing and
docs/validation/depth-ready-fence-2026-10-21.md for the reading.
"""
from __future__ import annotations

import argparse
import csv
import json
import statistics
import sys
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from pathlib import Path

PACK_QUEUE, PACK_FENCE = 2 ** 48, 2 ** 32
KEEP = {"queue_identity", "queue_execute", "queue_signal", "queue_wait", "fence_cpu_signal", "fence_cpu_wait", "fence_completed",
        "scene_left_depth_ready_cpu", "scene_right_depth_ready_cpu", "ordinary_dispatch_native_cpu"}
# CPU scopes that may enclose the point where the queue stops (named in the report).
SCOPES = ("openxr_wait_frame_cpu", "openxr_begin_frame_cpu", "openxr_acquire_wait_cpu", "openxr_copy_enqueue_cpu", "openxr_end_frame_cpu",
          "present_hook", "ordinary_dispatch_native_cpu")
KEEP |= set(SCOPES)
TYPES = {0: "direct", 1: "bundle", 2: "compute", 3: "copy"}


def unpack(value):
    whole = int(round(value))
    return whole // PACK_QUEUE, (whole // PACK_FENCE) % 65536, whole % PACK_FENCE


def load(rows):
    events = defaultdict(list)
    for row in rows:
        if row.get("event") not in KEEP:
            continue
        thread = row.get("thread") or ""
        events[row["event"]].append((float(row["qpc_start_ms"]), float(row["duration_ms"]), float(row["value"]), int(thread) if thread.isdigit() else None))
    for values in events.values():
        values.sort()
    return events


def median(values):
    return statistics.median(values) if values else None


def analyze(events, eye="left", start=None, end=None):
    queues = {}
    for _, _, value, _ in events.get("queue_identity", []):
        queues[int(value) // 16] = TYPES.get(int(value) % 16, str(int(value) % 16))
    # Signals and completions keyed by (fence, value); a value is identified by its low 32 bits.
    signals, completed, cpu_signals = {}, {}, {}
    per_queue_signals = defaultdict(list)
    for stamp, _, value, thread in events.get("queue_signal", []):
        q, f, v = unpack(value)
        signals.setdefault((f, v), (stamp, q, thread))
        per_queue_signals[q].append((stamp, f, v))
    for stamp, _, value, _ in events.get("fence_completed", []):
        _, f, v = unpack(value)
        completed.setdefault((f, v), stamp)
    for stamp, _, value, thread in events.get("fence_cpu_signal", []):
        _, f, v = unpack(value)
        cpu_signals.setdefault((f, v), (stamp, thread))
    waits_by_queue = defaultdict(list)
    for stamp, _, value, thread in events.get("queue_wait", []):
        q, f, v = unpack(value)
        waits_by_queue[q].append((stamp, f, v, thread))
    executes = defaultdict(list)
    for stamp, _, value, thread in events.get("queue_execute", []):
        executes[int(value) // 1000].append(stamp)
    signal_starts = {q: [p[0] for p in values] for q, values in per_queue_signals.items()}
    wait_starts = {q: [w[0] for w in values] for q, values in waits_by_queue.items()}
    scopes = {name: events.get(name, []) for name in SCOPES}
    scope_starts = {name: [r[0] for r in values] for name, values in scopes.items()}
    registrations = events.get("fence_cpu_wait", [])
    reg_starts = [r[0] for r in registrations]
    results, culprits = [], Counter()
    for stamp, duration, _, thread in events.get(f"scene_{eye}_depth_ready_cpu", []):
        if (start is not None and stamp < start) or (end is not None and stamp > end):
            continue
        finish = stamp + duration
        inside = [r for r in registrations[bisect_left(reg_starts, stamp):bisect_right(reg_starts, finish)] if r[3] == thread]
        if not inside:
            continue
        _, f, v = unpack(inside[-1][2])
        signal = signals.get((f, v))
        item = {"wait_ms": duration, "fence": f, "value": v}
        if signal is None:
            item["cause"] = "awaited value never queued (CPU signal or untraced queue)"
            culprits[item["cause"]] += 1
            results.append(item)
            continue
        s_at, q, s_thread = signal
        item.update({"queue": q, "queue_type": queues.get(q), "signal_thread": s_thread, "signal_before_wait_ms": stamp - s_at,
                     "completed_after_wait_end_ms": completed[(f, v)] - finish if (f, v) in completed else None})
        # The window of this queue's work ahead of the signal: since the previous signal on the queue completed.
        index = bisect_left(signal_starts[q], s_at)
        window_start = None
        for p_at, pf, pv in reversed(per_queue_signals[q][max(0, index - 2000):index]):
            done = completed.get((pf, pv))
            if done is not None and done <= stamp:
                window_start = p_at
                break
        window_start = window_start if window_start is not None else float("-inf")
        gpu_waits = []
        starts = wait_starts.get(q, [])
        for w_at, wf, wv, w_thread in waits_by_queue[q][bisect_left(starts, window_start):bisect_right(starts, s_at)]:
            if True:
                source = signals.get((wf, wv))
                cpu = cpu_signals.get((wf, wv))
                gpu_waits.append({"fence": wf, "value": wv,
                                  "signalled_on": f"queue {source[1]} ({queues.get(source[1])})" if source else ("the CPU" if cpu else "nothing in this process (another process, e.g. the VR compositor)"),
                                  "signal_after_wait_start_ms": ((source or cpu)[0] - stamp) if (source or cpu) else None,
                                  "signal_thread": (source[2] if source else cpu[1]) if (source or cpu) else None,
                                  "completed_after_wait_start_ms": completed[(wf, wv)] - stamp if (wf, wv) in completed else None})
        item["gpu_waits_ahead"] = gpu_waits
        item["submissions_ahead"] = bisect_right(executes[q], s_at) - bisect_left(executes[q], window_start)
        # The latest-satisfied GPU wait ahead of the signal, if it ended inside the CPU wait, is the stall.
        late = [w for w in gpu_waits if w["completed_after_wait_start_ms"] is not None and w["completed_after_wait_start_ms"] > 0.5]
        # Where the queue stopped: the first signal ahead that the GPU reached only after the CPU wait
        # began, when the one before it was reached promptly (a GPU-side wait we could not see, or slow work).
        stop = None
        for i in range(bisect_left(signal_starts[q], window_start), index):
            p_at, pf, pv = per_queue_signals[q][i]
            done = completed.get((pf, pv))
            if done is not None and done > stamp + 0.5:
                prev = per_queue_signals[q][i - 1] if i else None
                prev_done = completed.get((prev[1], prev[2])) if prev else None
                scope = next((name for name in SCOPES for a, d, _, _ in scopes[name][max(0, bisect_right(scope_starts[name], p_at) - 1):bisect_right(scope_starts[name], p_at)] if a <= p_at <= a + d), None)
                stop = {"queued_before_wait_ms": stamp - p_at, "reached_after_wait_start_ms": done - stamp,
                        "previous_reached_after_queue_ms": (prev_done - prev[0]) if prev_done is not None else None, "inside": scope}
                break
        item["queue_stopped_at"] = stop
        if late:
            worst = max(late, key=lambda w: w["completed_after_wait_start_ms"])
            item["cause"] = f"GPU wait on fence {worst['fence']} signalled on {worst['signalled_on']}"
        elif stop and stop["inside"] and stop["previous_reached_after_queue_ms"] is not None and stop["previous_reached_after_queue_ms"] < 1:
            item["cause"] = f"queue stalled behind work queued inside {stop['inside']} (no traced GPU wait)"
        else:
            item["cause"] = "backlog: earlier work on the same queue"
        culprits[item["cause"]] += 1
        results.append(item)
    summary = {"eye": eye, "waits": len(results), "queues": queues, "causes": dict(culprits.most_common()),
               "wait_ms_median": median([r["wait_ms"] for r in results]),
               "signal_before_wait_ms_median": median([r["signal_before_wait_ms"] for r in results if "signal_before_wait_ms" in r]),
               "completed_after_wait_end_ms_median": median([r["completed_after_wait_end_ms"] for r in results if r.get("completed_after_wait_end_ms") is not None]),
               "submissions_ahead_median": median([r["submissions_ahead"] for r in results if "submissions_ahead" in r]),
               "queue_stopped_before_wait_ms_median": median([r["queue_stopped_at"]["queued_before_wait_ms"] for r in results if r.get("queue_stopped_at")]),
               "awaited": dict(Counter(f"fence {r['fence']} on queue {r.get('queue')} ({r.get('queue_type')})" for r in results).most_common(3))}
    return {"summary": summary, "examples": results[:5]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace", type=Path)
    parser.add_argument("--eye", choices=("left", "right"), default="left")
    parser.add_argument("--start", type=float)
    parser.add_argument("--end", type=float)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    if not args.trace.is_absolute() or not args.trace.is_file():
        parser.error("trace must be an absolute path to cpu_trace.csv")
    with args.trace.open(encoding="utf-8-sig", newline="") as handle:
        report = analyze(load(csv.DictReader(handle)), args.eye, args.start, args.end)
    text = json.dumps(report, indent=2)
    if args.out:
        args.out.write_text(text + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
