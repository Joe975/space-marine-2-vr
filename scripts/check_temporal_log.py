"""Compare sampled Streamline history with consecutive eye and center transforms.

This is a matrix diagnostic, not a visual quality or motion-vector-buffer test.
"""
import argparse
import bisect
import json
import math
import re
import statistics
from pathlib import Path


def multiply(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inverse(a):
    rows = [list(row) + [float(i == j) for j in range(4)] for i, row in enumerate(a)]
    for j in range(4):
        pivot = max(range(j, 4), key=lambda i: abs(rows[i][j]))
        rows[j], rows[pivot] = rows[pivot], rows[j]
        scale = rows[j][j]
        if abs(scale) < 1e-12:
            raise ValueError("Singular transform")
        rows[j] = [v / scale for v in rows[j]]
        for i in range(4):
            if i != j:
                factor = rows[i][j]
                rows[i] = [v - factor * w for v, w in zip(rows[i], rows[j])]
    return [row[4:] for row in rows]


def world(c, center=False):
    pos = [p - r * c['shift'] for p, r in zip(c['pos'], c['right'])] if center else c['pos']
    return [c[k] + [0] for k in ('right', 'up', 'fwd')] + [pos + [1]]


def history(current, previous, current_center=False, previous_center=False):
    return multiply(multiply(multiply(inverse(current['projection']), world(current, current_center)),
                             inverse(world(previous, previous_center))), previous['projection'])


def fields(line):
    result = {}
    for key, value in re.findall(r'(\w+)=(\[[^\]]+\]|\S+)', line):
        result[key] = json.loads(value) if value.startswith('[') else float(value)
    return result


def matrix(values):
    if len(values) != 16 or not all(math.isfinite(v) for v in values):
        raise ValueError("Malformed matrix")
    return [values[i:i + 4] for i in range(0, 16, 4)]


def check_log(text, require_motion=False):
    engines, cameras = [], []
    for line in text.splitlines():
        if 'TemporalEngine: t=' in line:
            engines.append(fields(line))
        elif 'TemporalStreamline: t=' in line:
            cameras.append(fields(line))
    engines.sort(key=lambda e: e['t'])
    cameras.sort(key=lambda c: c['t'])
    if len(engines) < 3 or len(cameras) < 3:
        raise ValueError('Insufficient temporal trace')
    for a, b in zip(engines, engines[1:]):
        if b['seq'] != a['seq'] + 1 or not math.isclose(a['shift'], -b['shift'], abs_tol=1e-6) or abs(a['shift']) < 1e-5:
            raise ValueError('Main submissions did not alternate consecutively')
    times = [e['t'] for e in engines]
    paired = []
    for c in cameras:
        i = bisect.bisect_right(times, c['t']) - 1
        if i < 0:
            continue
        # Streamline may consume the preceding submitted camera, not the most recent add.
        candidates = [e for e in engines[max(0, i - 2):i + 1]
                      if c['t'] - e['t'] <= 100000 and math.dist(c['pos'], e['pos']) <= 1e-4
                      and math.dist(c['right'], e['right']) <= 1e-4]
        if not candidates:
            continue
        e = candidates[-1]
        if c['result'] != 0:
            raise ValueError('Streamline rejected a traced call')
        if math.dist(e['pos'], [p + r * e['shift'] for p, r in zip(e['center'], e['right'])]) > 1e-4:
            raise ValueError('Submitted position disagrees with eye offset')
        c['shift'], c['seq'] = e['shift'], e['seq']
        c['projection'], c['history'] = matrix(c['projection']), matrix(c['history'])
        paired.append(c)
    errors = {name: [] for name in ('eye_to_eye', 'previous_center', 'center_to_center')}
    moving_errors = {name: [] for name in errors}
    moving = 0
    segment_start, segment_moving = None, 0
    position_change = forward_change = 0.0
    substantial_motion = False
    for previous, current in zip(paired, paired[1:]):
        if current['seq'] != previous['seq'] + 1 or current['frame'] != previous['frame'] + 1 or current['reset']:
            segment_start, segment_moving = None, 0
            continue
        if segment_start is None:
            segment_start = previous
        is_moving = (math.dist(world(current, True)[3][:3], world(previous, True)[3][:3]) > 1e-4
                     or math.dist(current['fwd'], previous['fwd']) > 1e-4)
        moving += is_moving
        segment_moving += is_moving
        segment_position_change = math.dist(world(current, True)[3][:3], world(segment_start, True)[3][:3])
        segment_forward_change = math.dist(current['fwd'], segment_start['fwd'])
        position_change = max(position_change, segment_position_change)
        forward_change = max(forward_change, segment_forward_change)
        substantial_motion |= segment_moving >= 2 and max(segment_position_change, segment_forward_change) > 0.01
        for name, flags in (('eye_to_eye', (False, False)), ('previous_center', (False, True)), ('center_to_center', (True, True))):
            expected = history(current, previous, *flags)
            error = max(abs(current['history'][i][j] - expected[i][j]) for i in range(4) for j in range(4))
            errors[name].append(error)
            if is_moving:
                moving_errors[name].append(error)
    count = len(errors['eye_to_eye'])
    if count < 2:
        raise ValueError('No consecutive, paired, non-reset frames')
    if require_motion and not substantial_motion:
        raise ValueError('No substantial camera movement or turning in consecutive history pairs')
    return {'engine_samples': len(engines), 'streamline_samples': len(cameras), 'paired_samples': len(paired),
            'consecutive_history_pairs': count, 'moving_pairs': moving,
            'max_center_position_change': position_change, 'max_forward_change': forward_change,
            'median_max_matrix_error': {name: statistics.median(values) for name, values in errors.items()},
            'pairs_with_error_below_0_005': {name: sum(v < 0.005 for v in values) for name, values in errors.items()},
            'moving_median_max_matrix_error': {name: statistics.median(values) if values else None for name, values in moving_errors.items()},
            'moving_pairs_with_error_below_0_005': {name: sum(v < 0.005 for v in values) for name, values in moving_errors.items()}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--require-motion', action='store_true', help='Reject stationary traces and tiny drift')
    args = parser.parse_args()
    try:
        print(json.dumps(check_log(args.log.read_text(encoding='utf-8'), args.require_motion), indent=2))
    except (ValueError, KeyError) as error:
        parser.exit(1, f'Temporal check failed: {error}\n')
