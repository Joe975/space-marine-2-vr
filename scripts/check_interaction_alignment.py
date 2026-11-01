"""Where was the game's interaction camera pointing when the player pressed interact?

Space Marine 2 chooses the "Game.Use" target (Right B = native B) from its own gameplay
camera: a 90 degree use cone along the camera heading and a 25x25 degree focus cone around
the native crosshair (docs/INTERACTION.md). The VR mod keeps that camera out of the view, so
it can point far from the player's gaze. This lists every interact press with the gaze-to-native
camera misalignment and summarises how many presses could not have focused what the player
was looking at.

Usage (repo venv, isolated mode):
    .venv/Scripts/python.exe -I scripts/check_interaction_alignment.py <run>/sm2vr_trace.jsonl [--log <run>/sm2vr.log] [--json]

Traces written after 2026-10-20 carry camera.gaze_native directly. Older traces do not; with
--log the native camera heading/pitch is recovered from the native UI's own world-to-view
pairs (WorldMarkerCamera log lines, about one every 5 s; -Trace runs only since 26 Oct) and the trace's native camera
position. That recovery has two mirror solutions per line; the smaller native pitch is kept
because the native camera pitch only changes with right-stick input.
"""
import argparse
import bisect
import datetime as dt
import json
import math
import re
import sys

# Interact moved from native X to B on 31 Oct (docs/BUTTON_HINTS.md); older traces keep the old name.
INTERACT_BUTTONS = ('B_interact_reload', 'X_interact_reload')
FOCUS_HALF_DEGREES = 12.5   # 3dmarker_focus: ConeSquare 25 x 25 degrees from the native camera.
USE_HALF_DEGREES = 45.0     # usable_radius: ConeCircle angle 90 along the camera heading.
MARKER = re.compile(r'^\[(\d+):(\d+):(\d+)\.(\d+)\].*WorldMarkerCamera: caller=0xbf3d8c .*vr=false world=\[([^\]]+)\] view=\[([^\]]+)\]')


def wrap(degrees):
    return (degrees + 180.0) % 360.0 - 180.0


def heading_pitch(forward):
    """Game-world heading/pitch of a forward vector (right-handed, Y up: the title camera
    looks along -Z with +X to its right). Heading grows when turning left."""
    x, y, z = forward
    n = math.sqrt(x * x + y * y + z * z)
    if n < 1e-6:
        return None
    return math.degrees(math.atan2(-x, -z)), math.degrees(math.asin(max(-1.0, min(1.0, y / n))))


def camera_from_marker(world, camera, view):
    """Solve the native camera heading/pitch (roll 0) from one native world->view pair.
    Returns the candidate with the smaller |pitch|, or None when the camera position does not
    explain the distance (wrong camera)."""
    d = [world[i] - camera[i] for i in range(3)]
    nd, nv = math.sqrt(sum(c * c for c in d)), math.sqrt(sum(c * c for c in view))
    if nd < 1.0 or abs(nd - nv) > 0.08 * nd:
        return None
    vx, vy, vz = view
    horizontal = d[0] * d[0] + d[2] * d[2]
    along_sq = horizontal - vx * vx
    if along_sq < 0:
        along_sq = 0.0
    best = None
    for along in (math.sqrt(along_sq), -math.sqrt(along_sq)):
        # Horizontal: d = right*vx + forward_h*along, forward_h = (-sin h, 0, -cos h), right = (cos h, 0, -sin h).
        # Solve h from d_x = vx cos h - along sin h, d_z = -vx sin h - along cos h.
        h = math.atan2(-(d[0] * along + d[2] * vx), vx * d[0] - along * d[2])
        # Vertical: (along, d_y) rotated by pitch gives (vz, vy).
        p = math.atan2(d[1], along) - math.atan2(vy, vz) if (along or d[1]) else 0.0
        p = math.degrees(math.atan2(math.sin(p), math.cos(p)))
        candidate = (math.degrees(h), p)
        if best is None or abs(candidate[1]) < abs(best[1]):
            best = candidate
    return best


def load(trace_path):
    header, samples, presses = None, [], []
    with open(trace_path, encoding='utf-8') as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get('type') == 'header':
                header = r
            elif r.get('type') == 'event' and r.get('event') == 'button' and r.get('name') in INTERACT_BUTTONS:
                presses.append((r['t'], bool(r.get('down'))))
            elif r.get('type') == 'sample' and r.get('camera'):
                samples.append(r)
    return header, samples, presses


def markers(log_path, t0):
    out = []
    with open(log_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MARKER.match(line)
            if not m:
                continue
            h, mi, s, ms = map(int, m.groups()[:4])
            wall = t0.replace(hour=h, minute=mi, second=s, microsecond=ms * 1000)
            out.append(((wall - t0).total_seconds(), [float(v) for v in m.group(5).split(',')], [float(v) for v in m.group(6).split(',')]))
    return out


def analyse(trace_path, log_path=None):
    header, samples, presses = load(trace_path)
    t0 = dt.datetime.fromisoformat(header['wall_clock_local']) if header and header.get('wall_clock_local') else None
    times = [s['t'] for s in samples]

    def sample_at(t):
        i = bisect.bisect_left(times, t)
        near = [j for j in (i - 1, i) if 0 <= j < len(times)]
        return samples[min(near, key=lambda j: abs(times[j] - t))] if near else None

    recovered = []
    if log_path and t0:
        for t, world, view in markers(log_path, t0):
            s = sample_at(t)
            if s is None or abs(s['t'] - t) > 0.1 or not s['camera'].get('game'):
                continue
            solved = camera_from_marker(world, s['camera']['game'], view)
            if solved:
                recovered.append((t, solved))

    rows = []
    downs = [t for t, down in presses if down]
    ups = [t for t, down in presses if not down]
    for t in downs:
        s = sample_at(t)
        if s is None:
            continue
        cam = s['camera']
        hold = next((u - t for u in ups if u > t), None)
        yaw = pitch = None
        source = None
        if cam.get('gaze_native'):
            yaw, pitch = cam['gaze_native']['yaw_degrees'], cam['gaze_native']['pitch_degrees']
            source = 'trace'
        elif recovered and cam.get('eye_forward'):
            nt, native = min(recovered, key=lambda r: abs(r[0] - t))
            gaze = heading_pitch(cam['eye_forward'])
            if abs(nt - t) <= 3 and gaze:
                # Heading grows to the left; gaze to the camera's right is positive yaw.
                yaw, pitch = wrap(native[0] - gaze[0]), gaze[1] - native[1]
                source = 'log'
        rows.append({'t': t, 'wall': (t0 + dt.timedelta(seconds=t)).strftime('%H:%M:%S.%f')[:-3] if t0 else None,
                     'hold_ms': round(hold * 1000) if hold is not None else None, 'yaw_degrees': yaw, 'pitch_degrees': pitch, 'source': source})
    measured = [r for r in rows if r['yaw_degrees'] is not None]
    outside_use = [r for r in measured if abs(r['yaw_degrees']) > USE_HALF_DEGREES]
    outside_focus = [r for r in measured if abs(r['yaw_degrees']) > FOCUS_HALF_DEGREES or abs(r['pitch_degrees']) > FOCUS_HALF_DEGREES]
    yaws = sorted(abs(r['yaw_degrees']) for r in measured)
    summary = {'presses': len(rows), 'measured': len(measured),
               'median_abs_yaw_degrees': yaws[len(yaws) // 2] if yaws else None,
               'outside_use_cone': len(outside_use), 'outside_focus_cone': len(outside_focus),
               'recovered_native_samples': len(recovered)}
    return {'summary': summary, 'presses': rows}


# Measured 2026-10-21: the native camera yaws at most about 120 deg/s (full stick, edge
# boost) and pitches at about 55% of that. After the gaze stops, the camera must catch up
# within 0.6 s plus the remaining yaw / 80 or pitch / 45 deg/s, whichever is longer.
SETTLE_BASE_SECONDS = 0.6
SETTLE_YAW_DEGREES_PER_SECOND = 80.0
SETTLE_PITCH_DEGREES_PER_SECOND = 45.0
GAZE_MOVING_DEGREES_PER_SECOND = 5.0   # Slower than this counts as a still gaze.
LOOK_MIN_DEGREES = 10.0                # Smaller gaze movements are not judged as looks.


def _angle(a, b):
    na, nb = math.sqrt(sum(x * x for x in a)), math.sqrt(sum(x * x for x in b))
    if na < 1e-6 or nb < 1e-6:
        return 0.0
    c = sum(x * y for x, y in zip(a, b)) / (na * nb)
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def follow(samples, start=None, end=None, reengage=5.0, rest_after_s=0.25, merge_gap_s=0.3):
    """Gaze camera check. Looks are found from the gaze itself (camera.eye_forward moving
    faster than 5 deg/s, gaps under 0.3 s merged, at least 10 degrees in total), because the
    camera tracks a moving gaze and the error rarely grows large. For each look: when the gaze
    stops, the camera must be within the controller's re-engage band (5 degrees on both axes)
    with the right stick centred inside 0.6 s + the remaining yaw / 80 or pitch / 45 deg/s.
    While the gaze is still and the error stays inside that band, the stick must rest
    (no hunting); corrections while the gaze moves are tracking, not hunting.
    Body-mode samples with gaze_native and eye_forward only."""
    rows = [s for s in samples if s['camera'].get('mode') == 'body' and s['camera'].get('gaze_native') and s['camera'].get('eye_forward')
            and (start is None or s['t'] >= start) and (end is None or s['t'] <= end)]

    def error(s):
        g = s['camera']['gaze_native']
        return abs(g['yaw_degrees']), abs(g['pitch_degrees'])

    def resting(s):
        pad = (s.get('input') or {}).get('pad') or {}
        return pad.get('rx', 0) == 0 and pad.get('ry', 0) == 0

    moving = [False] * len(rows)
    for i in range(1, len(rows)):
        dt = rows[i]['t'] - rows[i - 1]['t']
        if dt > 0 and _angle(rows[i - 1]['camera']['eye_forward'], rows[i]['camera']['eye_forward']) / dt > GAZE_MOVING_DEGREES_PER_SECOND:
            moving[i] = True
    episodes, i = [], 0
    while i < len(rows):
        if not moving[i]:
            i += 1
            continue
        first, last = i, i
        while i < len(rows) and (moving[i] or rows[i]['t'] - rows[last]['t'] < merge_gap_s):
            if moving[i]:
                last = i
            i += 1
        size = _angle(rows[first - 1]['camera']['eye_forward'] if first else rows[first]['camera']['eye_forward'], rows[last]['camera']['eye_forward'])
        if size >= LOOK_MIN_DEGREES:
            episodes.append((first, last, size))
    looks = []
    for n, (first, last, size) in enumerate(episodes):
        stop = rows[last]['t']
        yaw, pitch = error(rows[last])
        lag = max(max(error(r)) for r in rows[first:last + 1])
        limit = SETTLE_BASE_SECONDS + max(yaw / SETTLE_YAW_DEGREES_PER_SECOND, pitch / SETTLE_PITCH_DEGREES_PER_SECOND)
        horizon = episodes[n + 1][0] if n + 1 < len(episodes) else len(rows)
        settle = None
        for r in rows[last:horizon]:
            if max(error(r)) <= reengage and resting(r):
                settle = r['t'] - stop
                break
        looks.append({'start': round(rows[first]['t'], 2), 'stop': round(stop, 2), 'gaze_degrees': round(size, 1),
                      'tracking_lag_degrees': round(lag, 1), 'yaw_at_stop': round(yaw, 1), 'pitch_at_stop': round(pitch, 1),
                      'settle_seconds': round(settle, 2) if settle is not None else None, 'limit_seconds': round(limit, 2)})
    # Hunting: the gaze is still, the error inside the band, settled for rest_after_s.
    rest, settled_since = [], None
    for i, r in enumerate(rows):
        still = not moving[i]
        inside = max(error(r)) <= reengage
        if not still or not inside:
            settled_since = None
            continue
        if settled_since is None:
            settled_since = r['t'] if resting(r) else None
            continue
        if r['t'] - settled_since >= rest_after_s:
            rest.append(resting(r))
    within = [r for r in rows if max(error(r)) <= 10]
    unsettled = [l for l in looks if l['settle_seconds'] is None]
    slow = [l for l in looks if l['settle_seconds'] is not None and l['settle_seconds'] > l['limit_seconds']]
    hunting = rest.count(False)
    return {'samples': len(rows), 'looks': len(looks), 'settles': looks,
            'settle_seconds': [l['settle_seconds'] for l in looks],
            'worst_settle_seconds': max((l['settle_seconds'] for l in looks if l['settle_seconds'] is not None), default=None),
            'slow_looks': len(slow), 'unsettled': len(unsettled),
            'share_within_10_degrees': round(len(within) / len(rows), 3) if rows else None,
            'rest_samples': len(rest), 'hunting_samples': hunting, 'stick_rests_when_settled': hunting == 0 if rest else None,
            'passed': bool(looks) and not unsettled and not slow and hunting == 0}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('trace')
    parser.add_argument('--log', help='sm2vr.log of the same run (older traces without gaze_native)')
    parser.add_argument('--follow', action='store_true', help='gaze camera candidate: settle time after each look (config/test_driver/gaze_camera_follow.json)')
    parser.add_argument('--start', type=float, help='trace seconds: only samples from here (--follow)')
    parser.add_argument('--end', type=float, help='trace seconds: only samples up to here (--follow)')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args(argv)
    if args.follow:
        _, samples, _ = load(args.trace)
        result = follow(samples, args.start, args.end)
        print(json.dumps(result, indent=1))
        return 0 if result['passed'] else 1
    result = analyse(args.trace, args.log)
    if args.json:
        print(json.dumps(result, indent=1))
        return 0
    for r in result['presses']:
        if r['yaw_degrees'] is None:
            print(f"{r['wall'] or r['t']}  hold {r['hold_ms']} ms  misalignment unknown")
        else:
            print(f"{r['wall'] or r['t']}  hold {r['hold_ms']} ms  gaze vs native camera yaw {r['yaw_degrees']:+7.1f}  pitch {r['pitch_degrees']:+6.1f}  ({r['source']})")
    s = result['summary']
    print(f"{s['presses']} presses, {s['measured']} measured; median |yaw| {s['median_abs_yaw_degrees']}; "
          f"outside the 90 degree use cone {s['outside_use_cone']}; outside the 25 degree focus cone {s['outside_focus_cone']}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
