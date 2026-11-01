"""Summarize headset stereo rate and output size from a run's sm2vr.log (docs/RESOLUTION.md).

Usage: summarize_session_rate.py SM2VR_LOG [--start HH:MM:SS] [--end HH:MM:SS]

Reads the 5-second VrSession status lines (per_eye_hz = fresh stereo pairs per second,
oldest_pose_age_ms) and the mod's "swap chain WxH" lines. Seconds with no submission
(loading, menus, pause) are excluded, and reported as a count. This is a coarse playtest
comparison, not a qualified benchmark (scripts/benchmark_driver_performance.py).
"""
import argparse
import json
import re
import statistics
import sys
from pathlib import Path

SESSION = re.compile(r'^\[(\d\d:\d\d:\d\d)\.\d+\].*VrSession: .*submitted_pairs=(\d+) per_eye_hz=([0-9.]+).*oldest_pose_age_ms=([0-9.]+)')
SWAP = re.compile(r'^\[(\d\d:\d\d:\d\d)\.\d+\].*Sm2Probe: ([a-z0-9 ]+): swap chain (\d+)x(\d+)')
SCALING = re.compile(r'Sm2Probe: ([a-z0-9 ]+): swap chain scaling=(\d+) alpha=(\d+) window client (-?\d+)x(-?\d+)')
RECOMMENDED = re.compile(r'recommended eye (\d+)x(\d+) \(max (\d+)x(\d+)\)')
# Eye images are capped at 3840x2160 by the launcher's -Resolution and 4096x2160 by the mod's copies.
MAX_WIDTH, MAX_HEIGHT = 3840, 2160


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def suggest_resolution(width, height, max_width=MAX_WIDTH, max_height=MAX_HEIGHT):
    """Largest even WxH with the runtime's recommended eye aspect that fits the caps."""
    if width <= 0 or height <= 0:
        return None
    scale = min(1.0, max_width / width, max_height / height)
    return f'{int(width * scale) // 2 * 2}x{int(height * scale) // 2 * 2}'


def summarize(lines, start=None, end=None):
    rates, ages, idle, swaps, recommended, window = [], [], 0, [], None, None
    for line in lines:
        found = RECOMMENDED.search(line)
        if found and recommended is None:
            w, h, mw, mh = (int(v) for v in found.groups())
            recommended = {'width': w, 'height': h, 'max_width': mw, 'max_height': mh,
                           'suggested_resolution': suggest_resolution(w, h)}
        found = SCALING.search(line)
        if found:
            window = {'reason': found[1], 'scaling': int(found[2]), 'alpha': int(found[3]),
                      'client_width': int(found[4]), 'client_height': int(found[5])}
            continue
        swap = SWAP.match(line)
        if swap:
            swaps.append({'time': swap[1], 'reason': swap[2], 'width': int(swap[3]), 'height': int(swap[4])})
            continue
        session = SESSION.match(line)
        if not session or (start and session[1] < start) or (end and session[1] > end):
            continue
        rate, age = float(session[3]), float(session[4])
        if rate <= 0:
            idle += 1
            continue
        rates.append(rate); ages.append(age)
    result = {'samples': len(rates), 'idle_samples': idle, 'swap_chain': swaps[-1] if swaps else None,
              'swap_chain_window': window, 'runtime_recommended_eye': recommended}
    if rates:
        result['pairs_per_second'] = {'median': statistics.median(rates), 'p10': percentile(rates, .1),
                                      'p90': percentile(rates, .9), 'min': min(rates), 'max': max(rates)}
        result['pose_age_ms'] = {'median': statistics.median(ages), 'p90': percentile(ages, .9)}
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('log', type=Path)
    parser.add_argument('--start', help='first wall-clock time to include, HH:MM:SS')
    parser.add_argument('--end', help='last wall-clock time to include, HH:MM:SS')
    args = parser.parse_args(argv)
    for value in (args.start, args.end):
        if value and not re.fullmatch(r'\d\d:\d\d:\d\d', value):
            parser.error('times must be HH:MM:SS')
    if not args.log.is_absolute() or not args.log.is_file():
        parser.error('log must be an absolute path to an existing sm2vr.log')
    with args.log.open(encoding='utf-8', errors='replace') as handle:
        print(json.dumps(summarize(handle, args.start, args.end), indent=2))


if __name__ == '__main__':
    sys.exit(main())
