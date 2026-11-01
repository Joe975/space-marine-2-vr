"""Per-eye image changes at a still view, for a scripts/hangar-route.ps1 folder (RIGHT_EYE_FLICKER_2026-10-28.md).

Usage: still_view_changes.py <abs route dir> [step filter, e.g. camhold]

For each recorded step: after a 2 s settle, the captures in which at least N pixels of an eye change by
more than T luma levels since the previous capture, per second, and the 99.9th percentile of each
pixel's standard deviation. At a still view both eyes should be 0; the second render pass (right image)
changing alone is the slot-1 instability. Pixel-level on purpose: the changes are a few small specular
spots, under the grid-cell threshold of viewer_flicker.
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import viewer_flicker  # noqa: E402

T, N, SETTLE = 6.0, 4, 2.0


def changes(eye, times, threshold=T, pixels=N):
    """(captures with a change, per second, p99.9 of per-pixel std) for one eye's luma stream."""
    if len(eye) < 2:
        return 0, 0.0, 0.0
    d = np.abs(np.diff(eye, axis=0))
    count = int(((d > threshold).sum(axis=(1, 2)) >= pixels).sum())
    span = float(times[-1] - times[0])
    return count, count / span if span > 0 else 0.0, float(np.percentile(eye.std(axis=0), 99.9))


def step(prefix, reference=None):
    """Both eyes' changes after the settle, the capture count, and (with a reference left-eye frame) the
    mean absolute luma difference of this step's first settled left frame from it: the same camera view
    gives about 1, a different one 5 and more."""
    left, right, times = viewer_flicker.mirror_luma(prefix)
    skip = int(np.searchsorted(times - times[0], SETTLE))
    view = float(np.abs(left[skip] - reference).mean()) if reference is not None and reference.shape == left[skip].shape else None
    return [changes(e[skip:], times[skip:]) for e in (left, right)], len(times) - skip, view


def first_left(prefix):
    left, _, times = viewer_flicker.mirror_luma(prefix)
    return left[int(np.searchsorted(times - times[0], SETTLE))]


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("route", type=Path)
    p.add_argument("filter", nargs="?", default="")
    p.add_argument("--reference", type=Path, help="a baseline step's prefix (no suffix): report how far each step's view is from it")
    a = p.parse_args(argv)
    reference = first_left(a.reference) if a.reference else None
    if not a.route.is_absolute() or not (a.route / "segments.csv").is_file():
        p.error("route must be an absolute folder holding segments.csv")
    for seg in csv.DictReader((a.route / "segments.csv").read_text().splitlines()):
        prefix = a.route / f"{seg['name']}_{seg['step']}"
        if a.filter not in seg["step"] or not prefix.with_suffix(".rgb").is_file():
            continue
        (left, right), n, view = step(prefix, reference)
        print(f"{seg['name']}_{seg['step']:22} captures {n:4d}  left {left[1]:6.2f}/s (std {left[2]:5.2f})  right {right[1]:6.2f}/s (std {right[2]:5.2f})"
              + ("" if view is None else f"  view diff {view:5.2f}"))


if __name__ == "__main__":
    main()
