"""Check animation takeovers and viewer scale in a playtest trace (docs/ANIMATION_CAMERA.md).

Usage:
  python scripts/check_animation_camera.py TRACE.jsonl [--eye-height 1.7] [--json OUT.json]

Takeover segments (camera mode "takeover"): the rendered eye centre must hold the spot where
the takeover started, whatever the character does: its horizontal drift from the first sample
and its height range (both in metres) include only the player's own head motion, so with a
still (scripted) head they should be about zero; --max-height-range sets the limit for both
(default 0.03 m). The eye's distance from the root is reported (the character moving away).

Control back (docs/TAKEOVER_RETURN_2026-10-26.md): per takeover, the first moment the root moves
at CONTROL_SPEED or more within CONTROL_ANGLE of the stick the game was sent (input.pad, read against
the game's camera, camera.game_forward) for CONTROL_TIME: the game has given the player control back.
The report gives that time from the takeover's start and how long the hold lasted after it
(before the early return: 0.85-1.1 s after executions, 1.4-3.3 s after the Vanguard's grapnel).
Early returns (camera.released) are counted.

Ride segments (camera.ride true, the jump pack's dash and leap: first person rides along): the
eye must stay on the Marine. After RIDE_CATCH_UP seconds (a held view catching up) its horizontal
distance from the root stays under --max-ride-distance (default 1 m; first person sits 0.2-0.3 m
from it, plus the player's own head motion). The peak travel speed and the root's climb are
reported.

Viewer segments (camera mode "viewer"): per scale (units_per_metre), the stereo baseline in
metres (eye_separation / units_per_metre, should equal the IPD at every scale) and the eye
height above the character's floor in metres ((eye_y - floor) / units_per_metre). With
--eye-height (the player's real eye height; scripted-head launches use 1.7) each grounded
scale must match it within 5 cm.

Exit status 0 when every check passes, 1 otherwise.
"""
import argparse
import json
import math
import sys
from pathlib import Path


def samples(path):
    with open(path, encoding='utf-8') as handle:
        for line in handle:
            if '"sample"' not in line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if row.get('type') == 'sample':
                yield row


RIDE_CATCH_UP = 0.3
CONTROL_SPEED, CONTROL_ANGLE, CONTROL_TIME, SPEED_WINDOW = 1.5, 30.0, 0.1, 0.06


def control_back(segment, upm_default=0.9):
    """Trace seconds at which the root started moving along the stick (see the module doc), or None."""
    track, since = [], None
    for row in segment:
        camera, character, pad = row.get('camera') or {}, row.get('character') or {}, (row.get('input') or {}).get('pad')
        root, forward = character.get('root'), camera.get('game_forward')
        if not root or not forward or pad is None:
            continue
        upm = character.get('units_per_metre') or camera.get('units_per_metre') or upm_default
        track.append((row['t'], root))
        older = next((p for p in reversed(track) if row['t'] - p[0] >= SPEED_WINDOW), None)
        along = False
        if older is not None:
            dt = row['t'] - older[0]
            vx, vz = (root[0] - older[1][0]) / dt / upm, (root[2] - older[1][2]) / dt / upm
            x, y, f = pad.get('lx', 0) / 32767, pad.get('ly', 0) / 32767, math.hypot(forward[0], forward[2])
            speed = math.hypot(vx, vz)
            if speed >= CONTROL_SPEED and math.hypot(x, y) >= 0.5 and f > 1e-4:
                fx, fz = forward[0] / f, forward[2] / f
                dx, dz = -fz * x + fx * y, fx * x + fz * y
                along = (vx * dx + vz * dz) / (math.hypot(dx, dz) * speed) >= math.cos(math.radians(CONTROL_ANGLE))
        if not along:
            since = None
        elif since is None:
            since = row['t']
        elif row['t'] - since >= CONTROL_TIME:
            return since
    return None


def segments(rows, mode, ride=False, released=False):
    out, current = [], None
    for row in rows:
        camera = row.get('camera') or {}
        if (camera.get('released') is True) if released else (camera.get('ride') is True) if ride else camera.get('mode') == mode:
            if current is None:
                current = []
                out.append(current)
            current.append(row)
        else:
            current = None
    return out


def takeover_report(segment, upm_default=0.9):
    distances, heights, native, drifts, start = [], [], [], [], None
    for row in segment:
        camera, character = row.get('camera') or {}, row.get('character') or {}
        eye, root = camera.get('eye_centre'), character.get('root')
        if not eye or not root:
            continue
        upm = character.get('units_per_metre') or camera.get('units_per_metre') or upm_default
        start = start or eye
        distances.append(math.hypot(eye[0] - root[0], eye[2] - root[2]) / upm)
        drifts.append(math.hypot(eye[0] - start[0], eye[2] - start[2]) / upm)
        heights.append(eye[1] / upm)
        if camera.get('game'):
            native.append((camera['game'][1] - root[1]) / upm)
    if not heights:
        return None
    control = control_back(segment, upm_default)
    return {
        'control': control, 'held_after_control': (segment[-1]['t'] - control) if control is not None else None,
        'start': segment[0]['t'], 'end': segment[-1]['t'], 'samples': len(heights),
        'distance_min': min(distances), 'distance_max': max(distances),
        'drift_max': max(drifts), 'height_range': max(heights) - min(heights),
        'native_height_range': (max(native) - min(native)) if native else None,
    }


def ride_report(segment, upm_default=0.9):
    distances, speeds, climb, start = [], [], [], segment[0]['t']
    for row in segment:
        camera, character = row.get('camera') or {}, row.get('character') or {}
        eye, root = camera.get('eye_centre'), character.get('root')
        if camera.get('travel_speed_metres_per_second') is not None:
            speeds.append(camera['travel_speed_metres_per_second'])
        if not eye or not root:
            continue
        upm = character.get('units_per_metre') or camera.get('units_per_metre') or upm_default
        climb.append(root[1] / upm)
        if row['t'] - start >= RIDE_CATCH_UP:
            distances.append(math.hypot(eye[0] - root[0], eye[2] - root[2]) / upm)
    return {
        'start': start, 'end': segment[-1]['t'], 'samples': len(segment),
        'speed_max': max(speeds) if speeds else None,
        'climb': (max(climb) - climb[0]) if climb else None,
        'distance_max': max(distances) if distances else None,
    }


def viewer_report(segment, floor):
    by_scale = {}
    for row in segment:
        camera = row.get('camera') or {}
        eye, upm, separation = camera.get('eye_centre'), camera.get('units_per_metre'), camera.get('eye_separation')
        if not eye or not upm:
            continue
        entry = by_scale.setdefault(round(upm, 3), {'units_per_metre': round(upm, 3), 'samples': 0, 'baseline': [], 'eye_height': []})
        entry['samples'] += 1
        if separation:
            entry['baseline'].append(separation / upm)
        if floor is not None:
            entry['eye_height'].append((eye[1] - floor) / upm)
    rows = []
    for entry in by_scale.values():
        rows.append({
            'units_per_metre': entry['units_per_metre'], 'samples': entry['samples'],
            'baseline_metres': sum(entry['baseline']) / len(entry['baseline']) if entry['baseline'] else None,
            'eye_height_metres': sorted(entry['eye_height'])[len(entry['eye_height']) // 2] if entry['eye_height'] else None,
        })
    return {'start': segment[0]['t'], 'end': segment[-1]['t'], 'floor': floor, 'scales': rows}


def check(path, eye_height=None, max_height_range=0.03, max_ride_distance=1.0):
    rows = list(samples(path))
    report = {'takeovers': [], 'rides': [], 'viewers': [], 'failures': [],
              'releases': len(segments(rows, None, released=True))}
    for segment in segments(rows, None, ride=True):
        result = ride_report(segment)
        report['rides'].append(result)
        if result['distance_max'] is not None and result['distance_max'] > max_ride_distance:
            report['failures'].append(f"ride at {result['start']:.1f}s: eye {result['distance_max']:.2f} m from the root")
    for segment in segments(rows, 'takeover'):
        result = takeover_report(segment)
        if result is None:
            continue
        report['takeovers'].append(result)
        if result['drift_max'] > max_height_range:
            report['failures'].append(f"takeover at {result['start']:.1f}s: eye drifted {result['drift_max']:.3f} m")
        if result['height_range'] > max_height_range:
            report['failures'].append(f"takeover at {result['start']:.1f}s: eye height moved {result['height_range']:.3f} m")
    for segment in segments(rows, 'viewer'):
        # The floor is the last character root seen before the viewer froze the game.
        before = [r for r in rows if r['t'] < segment[0]['t'] and (r.get('character') or {}).get('root')]
        floor = before[-1]['character']['root'][1] if before else None
        result = viewer_report(segment, floor)
        report['viewers'].append(result)
        for scale in result['scales']:
            if eye_height is not None and scale['eye_height_metres'] is not None and abs(scale['eye_height_metres'] - eye_height) > 0.05:
                report['failures'].append(f"viewer at {result['start']:.1f}s, scale {scale['units_per_metre']}: eye {scale['eye_height_metres']:.2f} m above the floor (real {eye_height:.2f})")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--eye-height', type=float)
    parser.add_argument('--max-height-range', type=float, default=0.03)
    parser.add_argument('--max-ride-distance', type=float, default=1.0)
    parser.add_argument('--json', type=Path)
    args = parser.parse_args(argv)
    report = check(args.trace, args.eye_height, args.max_height_range, args.max_ride_distance)
    for t in report['takeovers']:
        native = f", native camera height range {t['native_height_range']:.2f} m" if t['native_height_range'] is not None else ''
        print(f"takeover {t['start']:.1f}-{t['end']:.1f}s: eye drift {t['drift_max']:.3f} m, height range {t['height_range']:.3f} m, "
              f"root {t['distance_min']:.2f}-{t['distance_max']:.2f} m from the eye{native}"
              + (f", control back +{t['control'] - t['start']:.2f}s, held {t['held_after_control']:.2f}s after it" if t['control'] is not None else ''))
    print(f"early returns: {report['releases']}")
    for r in report['rides']:
        speed = f"{r['speed_max']:.1f} m/s" if r['speed_max'] is not None else 'n/a'
        distance = f"{r['distance_max']:.2f} m" if r['distance_max'] is not None else 'n/a'
        climb = f"{r['climb']:.2f} m" if r['climb'] is not None else 'n/a'
        print(f"ride {r['start']:.1f}-{r['end']:.1f}s: peak {speed}, root climbed {climb}, eye at most {distance} from the root")
    for v in report['viewers']:
        print(f"viewer {v['start']:.1f}-{v['end']:.1f}s (floor {v['floor']}):")
        for s in v['scales']:
            baseline = f"{s['baseline_metres'] * 1000:.1f} mm" if s['baseline_metres'] is not None else 'n/a'
            height = f"{s['eye_height_metres']:.2f} m" if s['eye_height_metres'] is not None else 'n/a'
            print(f"  units_per_metre {s['units_per_metre']}: baseline {baseline}, eye height {height} ({s['samples']} samples)")
    for failure in report['failures']:
        print('FAIL', failure)
    if args.json:
        args.json.write_text(json.dumps(report, indent=1))
    print('PASS' if not report['failures'] else 'FAIL')
    return 0 if not report['failures'] else 1


if __name__ == '__main__':
    sys.exit(main())
