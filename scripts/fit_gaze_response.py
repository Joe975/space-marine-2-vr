"""Measure the native camera's right-stick response from a playtest trace.

The gaze camera (mod/src/VrGazeCamera.h) inverts this curve: for each stick deflection held
over consecutive samples, how fast did the native camera (camera.game_forward) turn? Uses
first-person samples with an armed pad only, and pairs the stick of sample i with the turn
between samples i+1 and i+2 (one poll of latency; the result barely changes with 0-4).

Usage (repo venv, isolated mode):
    .venv/Scripts/python.exe -I scripts/fit_gaze_response.py <run>/sm2vr_trace.jsonl [--json]

Prints median degrees/second per 0.05 of stick for yaw and pitch, with sample counts.
"""
import argparse
import json
import math
import statistics
import sys
from collections import defaultdict


def rows_from(lines):
    rows = []
    for line in lines:
        if '"type":"sample"' not in line:
            continue
        try:
            r = json.loads(line)
        except ValueError:
            continue  # The launcher kills the game, so the last line can be cut off.
        cam, inp = r.get('camera'), r.get('input')
        if not cam or not inp or not cam.get('game_forward') or cam.get('mode') != 'body' or not inp.get('armed'):
            continue
        fx, fy, fz = cam['game_forward']
        pad = inp.get('pad') or {}
        heading = math.degrees(math.atan2(-fx, -fz))  # Grows when the camera turns left.
        pitch = math.degrees(math.asin(max(-1.0, min(1.0, fy))))
        rows.append((r['t'], heading, pitch, pad.get('rx', 0) / 32767, pad.get('ry', 0) / 32767))
    return rows


def wrap(degrees):
    return (degrees + 180.0) % 360.0 - 180.0


def fit(rows, step=0.05, minimum=3):
    yaw, pitch = defaultdict(list), defaultdict(list)
    for a, b, c in zip(rows, rows[1:], rows[2:]):
        dt = c[0] - b[0]
        if not 0.02 < dt < 0.06 or abs(a[3] - b[3]) > 0.02 or abs(a[4] - b[4]) > 0.02:
            continue  # Only a stick held across the measured interval.
        key = lambda s: round(round(abs(s) / step) * step, 2)
        if abs(a[3]) > step:
            yaw[key(a[3])].append(abs(wrap(c[1] - b[1])) / dt)
        if abs(a[4]) > step:
            pitch[key(a[4])].append(abs(c[2] - b[2]) / dt)
    table = lambda d: [{'stick': s, 'degrees_per_second': round(statistics.median(v), 1), 'samples': len(v)}
                       for s, v in sorted(d.items()) if len(v) >= minimum]
    return {'yaw': table(yaw), 'pitch': table(pitch), 'samples': len(rows)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('trace')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args(argv)
    with open(args.trace, encoding='utf-8') as f:
        result = fit(rows_from(f))
    if args.json:
        print(json.dumps(result, indent=1))
        return 0
    print(f"{result['samples']} armed first-person samples")
    for axis in ('yaw', 'pitch'):
        print(axis, ' '.join(f"{p['stick']:.2f}:{p['degrees_per_second']:.1f}(n{p['samples']})" for p in result[axis]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
