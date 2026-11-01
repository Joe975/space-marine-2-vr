"""Standing eye height from recorded VR traces (docs/BODY_SCALE.md).

Usage: measure_eye_height.py TRACE_ABSOLUTE [TRACE_ABSOLUTE ...]

Reads the input thread's raw standing-space head position (input.head, metres, floor at 0).
Counts only armed gameplay samples with a plausible standing height (1.0-2.1 m, the same
range the mod accepts) and the gaze within 30 degrees of level, so a headset set down,
looking at the floor or a seated menu does not count. Crouches and leans are a small
minority, so the median is the standing eye height. Prints per-trace and pooled results
as JSON.
"""
import json
import math
import statistics
import sys
from pathlib import Path

MIN_HEIGHT, MAX_HEIGHT = 1.0, 2.1
MAX_GAZE_PITCH = math.sin(math.radians(30))


def heights(lines):
    for line in lines:
        if '"head_forward"' not in line:
            continue
        try:
            record = json.loads(line)
        except ValueError:
            continue
        sample = record.get("input")
        if not isinstance(sample, dict) or not sample.get("armed"):
            continue
        head, forward = sample.get("head"), sample.get("head_forward")
        if not head or not forward or abs(forward[1]) > MAX_GAZE_PITCH:
            continue
        if MIN_HEIGHT <= head[1] <= MAX_HEIGHT:
            yield head[1]


def summary(values):
    if not values:
        return {"samples": 0}
    q = statistics.quantiles(values, n=20)
    return {"samples": len(values), "median": round(statistics.median(values), 3),
            "p10": round(q[1], 3), "p90": round(q[17], 3)}


def main(paths):
    pooled, result = [], {}
    for path in paths:
        with Path(path).open(encoding="utf-8", errors="replace") as lines:
            values = list(heights(lines))
        pooled += values
        result[path] = summary(values)
    result["pooled"] = summary(pooled)
    print(json.dumps(result, indent=1))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
