"""Per-step pair rate, pose age and one-eye flicker for a scripts/hangar-route.ps1 output folder.

Usage: summarize_hangar_route.py <abs route dir>   (holding segments.csv and runs.csv, as
own-queue-hangar-ab.ps1 writes them; docs/OPENXR_OWN_QUEUE.md)

For each recorded step: the run's VrSession status lines inside the step's wall-clock window
(summarize_session_rate.summarize) and viewer_flicker.one_eye_blips over the both-eye mirror stream.
Then one line per arm (the letter after the launch number: A, B) and step kind, pooling the launches.
"""
from __future__ import annotations

import argparse
import csv
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402

import summarize_session_rate  # noqa: E402
import viewer_flicker  # noqa: E402

QUEUE = re.compile(r"queue=(own|game)")
THREAD = re.compile(r"submit_thread=true")


def arm(name: str) -> str:
    return re.sub(r"^\d+", "", name)


def kind(step: str) -> str:
    return re.sub(r"^\d+", "", step)


def queue_in(lines, start, end):
    """The queue(s) the step's status lines name, with "+thr" when the submit thread ran."""
    window = [line for line in lines if start <= line[1:9] <= end]
    seen = {m[1] for line in window for m in [QUEUE.search(line)] if m}
    thread = any(THREAD.search(line) for line in window)
    return (",".join(sorted(seen)) or "?") + ("+thr" if thread else "")


def rows(route: Path):
    runs = {name: Path(path) for name, path in csv.reader((route / "runs.csv").read_text().splitlines())}
    logs = {}
    for seg in csv.DictReader((route / "segments.csv").read_text().splitlines()):
        prefix = route / f"{seg['name']}_{seg['step']}"
        if not prefix.with_suffix(".rgb").is_file() or seg["name"] not in runs:
            continue
        if seg["name"] not in logs:
            logs[seg["name"]] = (runs[seg["name"]] / "sm2vr.log").read_text(encoding="utf-8", errors="replace").splitlines()
        lines = logs[seg["name"]]
        rate = summarize_session_rate.summarize(lines, seg["start"], seg["end"])
        left, right, times = viewer_flicker.mirror_luma(prefix)
        only_left, only_right, frames = viewer_flicker.one_eye_blips(left, right)
        change_left, change_right, seconds = viewer_flicker.one_eye_changes(left, right, times)
        yield {
            "name": seg["name"], "step": seg["step"], "state": seg["state"], "queue": queue_in(lines, seg["start"], seg["end"]),
            "pairs": rate.get("pairs_per_second", {}).get("median"), "pose": rate.get("pose_age_ms", {}).get("median"),
            "frames": frames, "left_only": 100 * only_left / max(frames, 1), "right_only": 100 * only_right / max(frames, 1),
            # Meaningful on still steps only: in motion both images change every frame.
            "left_changes": change_left / seconds if seconds else None, "right_changes": change_right / seconds if seconds else None,
        }


def fmt(value, spec):
    return "-" if value is None else format(value, spec)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("route", type=Path)
    a = p.parse_args(argv)
    if not a.route.is_absolute() or not (a.route / "segments.csv").is_file():
        p.error("route must be an absolute folder holding segments.csv and runs.csv")
    table = list(rows(a.route))
    print("launch step               queue  state      pairs/s  pose ms  frames  left-only/100  right-only/100  L-only chg/s  R-only chg/s")
    for r in table:
        print(f"{r['name']:6} {r['step']:19} {r['queue']:6} {r['state']:10} {fmt(r['pairs'], '7.1f')}  {fmt(r['pose'], '7.1f')}  "
              f"{r['frames']:6}  {r['left_only']:13.2f}  {r['right_only']:14.2f}  {fmt(r['left_changes'], '12.2f')}  {fmt(r['right_changes'], '12.2f')}")
    pooled = defaultdict(list)
    for r in table:
        pooled[(arm(r["name"]), kind(r["step"]))].append(r)
    print("\narm step              n  pairs/s  pose ms  left-only/100  right-only/100  L-only chg/s  R-only chg/s   (medians of the launches' steps)")
    for (a_, k), rs in sorted(pooled.items(), key=lambda x: (x[0][1], x[0][0])):
        med = lambda key: statistics.median([r[key] for r in rs if r[key] is not None]) if any(r[key] is not None for r in rs) else None
        print(f"{a_:3} {k:16} {len(rs):2}  {fmt(med('pairs'), '7.1f')}  {fmt(med('pose'), '7.1f')}  {fmt(med('left_only'), '13.2f')}  "
              f"{fmt(med('right_only'), '14.2f')}  {fmt(med('left_changes'), '12.2f')}  {fmt(med('right_changes'), '12.2f')}")


if __name__ == "__main__":
    main()
