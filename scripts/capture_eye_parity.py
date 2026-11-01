"""Capture same-pose and native-pose eye pairs under several shading modes, then score them.

Usage (from a loaded -TestDriver -ScriptedHead -EyeSnapshots -EyeSourceControl launch):
  python scripts/capture_eye_parity.py DUMP_DIR [--stations default|NAME:YAW:PITCH,...]
                                       [--variants SOURCE/MODE,...] [--dry-run]

DUMP_DIR is the run's ordinary dump directory (build/ordinary/<date>/<tag>); it holds the live
control files shading.txt and eye_source.txt. The script queues one scripted-head scenario that
holds each station (head yaw/pitch, hands lowered out of view). At every station it applies each
variant in order: write eye_source.txt (native/left) and shading.txt (light_union,
light_union_shadow_refresh, ...), wait for the 0.5 s / 1 s polls, then request one submitted-eye
snapshot. With source `left` both slots render the same pose, so analyze_eye_parity.py measures
per-slot shading without parallax. The original control file contents are restored at the end.

A variant may add a third part, SOURCE/MODE/TONE, written to tone_source.txt (launch with
-ToneSourceControl): which tone-pass inputs the right eye takes from the left eye's tone call of the
same dispatch (native, bloom, scene, luminance, all; OrdinaryToneSource.h).

Order matters: the native cascade reuse being tested only diverges over time, so a baseline
variant is captured before the refresh variant at each station, and the first station's
baseline is the only one never preceded by a refresh. Results: DUMP_DIR/eye_parity_capture.json
(schedule, request id, terminal driver state) and DUMP_DIR/eye_parity.json (scores).
"""
import argparse
import json
import sys
import time
import uuid
from pathlib import Path

# Sibling scripts must import under `python -I` too, which leaves the script directory off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_eye_parity as parity  # noqa: E402
import vr_test_driver as driver  # noqa: E402

DEFAULT_STATIONS = (('fwd', 0, 5), ('sky', 0, 25), ('left45', 45, 0), ('right45', -45, 0))
DEFAULT_VARIANTS = (('left', 'light_union'), ('native', 'light_union'),
                    ('left', 'light_union_shadow_refresh'), ('native', 'light_union_shadow_refresh'))
SOURCES = ('native', 'left', 'right', 'swap')
# shading.txt modes -> short snapshot-name parts. The refresh_* modes keep shadow refresh and light
# union while switching off SSR / SSAO / screen-space shadows, to localize what still differs.
MODES = {'light_union': 'union', 'light_union_shadow_refresh': 'refresh', 'none': 'none',
         'native_reflections': 'noreflect', 'light_union_all': 'unionall',
         'light_union_shadow_refresh_ssr': 'refreshssr', 'light_union_shadow_refresh_ssao': 'refreshssao',
         'light_union_shadow_refresh_screen_shadows': 'refreshsss', 'light_union_shadow_refresh_all': 'refreshall',
         'light_union_shadow_refresh_native_volumetrics': 'nativevol'}
# tone_source.txt values -> short snapshot-name parts.
TONES = {'native': 'tnative', 'bloom': 'tbloom', 'scene': 'tscene', 'luminance': 'tlum', 'all': 'tall'}
LEFT_HAND = [-0.25, -0.6, 0, 0, 0, 0]
RIGHT_HAND = [0.25, -0.6, 0, 0, 0, 0]
MAX_SNAPSHOTS = 24  # EyeDumpRequest's per-process limit


def snapshot_name(station, source, mode, tone=None):
    return f'{station}_{source}_{MODES[mode]}' + (f'_{TONES[tone]}' if tone else '')


def normalize(variant):
    """(source, mode) or (source, mode, tone) -> (source, mode, tone or None)."""
    if len(variant) not in (2, 3):
        raise ValueError(f'variant {variant} needs SOURCE/MODE or SOURCE/MODE/TONE')
    return (*variant, None) if len(variant) == 2 else tuple(variant)


def build_plan(stations=DEFAULT_STATIONS, variants=DEFAULT_VARIANTS, settle_ms=3000, variant_ms=5000,
               apply_ms=2500, transition_ms=3000):
    """Return (scenario, schedule). Each schedule slot: name, controls, apply/request/end times in ms."""
    if not stations or not variants:
        raise ValueError('need at least one station and one variant')
    if not 0 < apply_ms < variant_ms:
        raise ValueError('apply_ms must leave time to capture inside a variant slot')
    variants = tuple(normalize(v) for v in variants)
    for source, mode, tone in variants:
        if source not in SOURCES or mode not in MODES or (tone is not None and tone not in TONES):
            raise ValueError(f'unknown variant {source}/{mode}/{tone}')
    if len(stations) * len(variants) > MAX_SNAPSHOTS:
        raise ValueError(f'at most {MAX_SNAPSHOTS} snapshots per launch')
    frames, schedule, t = [], [], 0
    names = set()
    for index, (name, yaw, pitch) in enumerate(stations):
        if index:
            t += transition_ms
        head = [0, 0, 0, yaw, pitch, 0]
        frames.append({'ms': t, 'head': head, 'left': LEFT_HAND, 'right': RIGHT_HAND})
        slot_start = t + settle_ms
        for k, (source, mode, tone) in enumerate(variants):
            start = slot_start + k * variant_ms
            snapshot = snapshot_name(name, source, mode, tone)
            if snapshot in names or len(snapshot) > 40:
                raise ValueError(f'duplicate or too long snapshot name {snapshot}')
            names.add(snapshot)
            schedule.append({'name': snapshot, 'station': name, 'yaw': yaw, 'pitch': pitch,
                             'source': source, 'mode': mode, 'tone': tone, 'apply_ms': start,
                             'request_ms': start + apply_ms, 'end_ms': start + variant_ms})
        t = slot_start + len(variants) * variant_ms
        frames.append({'ms': t, 'head': head, 'left': LEFT_HAND, 'right': RIGHT_HAND})
    if t > 120000:
        raise ValueError(f'scenario lasts {t} ms; the driver allows 120000')
    scenario = {'description': 'Eye parity: hold stations with hands lowered while shading/eye-source variants are captured.',
                'mode': 'scripted_head', 'duration_ms': t, 'frames': frames}
    driver.compile_script(scenario, 'eye_parity_check')  # validate before touching the game
    return scenario, schedule


def parse_stations(text):
    if text == 'default':
        return DEFAULT_STATIONS
    stations = []
    for item in text.split(','):
        name, yaw, pitch = item.split(':')
        stations.append((name, float(yaw), float(pitch)))
    return tuple(stations)


def parse_variants(text):
    if text == 'default':
        return DEFAULT_VARIANTS
    return tuple(tuple(item.split('/')) for item in text.split(','))


def write_control(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(value + '\n', encoding='ascii')
    temporary.replace(path)


class Clock:
    monotonic = staticmethod(time.monotonic)
    sleep = staticmethod(time.sleep)


def run(dump, mailbox, scenario, schedule, clock=Clock, request_id=None):
    dump, mailbox = Path(dump), Path(mailbox)
    shading, eye_source, tone_source = dump / 'shading.txt', dump / 'eye_source.txt', dump / 'tone_source.txt'
    if not shading.is_file():
        raise SystemExit(f'{shading} missing: launch with the headset launcher')
    if any(slot['source'] != 'native' for slot in schedule) and not eye_source.is_file():
        raise SystemExit(f'{eye_source} missing: launch with -EyeSourceControl')
    if any(slot.get('tone') for slot in schedule) and not tone_source.is_file():
        raise SystemExit(f'{tone_source} missing: launch with -ToneSourceControl')
    for slot in schedule:
        if (dump / slot['name']).exists():
            raise SystemExit(f"{dump / slot['name']} already exists; snapshot names are single-use per launch")
    original = {path: path.read_text(encoding='ascii') for path in (shading, eye_source, tone_source) if path.is_file()}
    request_id = request_id or 'parity_' + uuid.uuid4().hex
    (mailbox / (request_id + '.json')).write_text(json.dumps(scenario, indent=2))
    driver.write_request(mailbox, driver.compile_script(scenario, request_id))
    print(request_id, flush=True)
    status, applied, requested = {}, set(), set()
    deadline = clock.monotonic() + scenario['duration_ms'] / 1000 + 30
    try:
        while clock.monotonic() < deadline:
            try:
                status = json.loads((mailbox / 'status.json').read_text())
            except (OSError, json.JSONDecodeError):
                clock.sleep(.05)
                continue
            current = status.get('id') == request_id
            if current and status.get('state') not in ('preroll', 'running'):
                break
            elapsed = status.get('elapsed_ms', -1) if current and status.get('state') == 'running' else -1
            for slot in schedule:
                name = slot['name']
                if name not in applied and slot['apply_ms'] <= elapsed < slot['end_ms']:
                    if eye_source.is_file():
                        write_control(eye_source, slot['source'])
                    write_control(shading, slot['mode'])
                    if tone_source.is_file():
                        write_control(tone_source, slot.get('tone') or 'native')
                    applied.add(name)
                    print('apply', name, elapsed, flush=True)
                if name in applied and name not in requested and slot['request_ms'] <= elapsed < slot['end_ms']:
                    driver.write_request(dump, name + '\n')
                    requested.add(name)
                    print('snapshot', name, elapsed, flush=True)
            clock.sleep(.05)
        clock.sleep(3)
    finally:
        for path, text in original.items():
            write_control(path, text.strip())
    captures = []
    for slot in schedule:
        folder = dump / slot['name']
        saved = all((folder / f).is_file() for f in ('left.ppm', 'right.ppm', 'pair.json'))
        captures.append({**slot, 'applied': slot['name'] in applied, 'requested': slot['name'] in requested,
                         'saved': saved})
    receipt = {'id': request_id, 'terminal': status, 'captures': captures,
               'restored': {path.name: text.strip() for path, text in original.items()}}
    (dump / 'eye_parity_capture.json').write_text(json.dumps(receipt, indent=2))
    ok = status.get('id') == request_id and status.get('state') == 'completed' and all(c['saved'] for c in captures)
    return ok, receipt


def score(dump, receipt):
    results = []
    for capture in receipt['captures']:
        if capture['saved']:
            result = parity.analyze_folder(Path(dump) / capture['name'], write_heatmap=True)
            results.append({**{k: capture.get(k) for k in ('station', 'source', 'mode', 'tone')}, **result})
    (Path(dump) / 'eye_parity.json').write_text(json.dumps(results, indent=2))
    if results:
        parity.contact_sheet(results, [Path(dump) / r['snapshot'] for r in results], Path(dump) / 'eye_parity_sheet.png')
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('dump')
    parser.add_argument('--stations', default='default')
    parser.add_argument('--variants', default='default')
    parser.add_argument('--dry-run', action='store_true', help='print the plan without queueing')
    args = parser.parse_args(argv)
    scenario, schedule = build_plan(parse_stations(args.stations), parse_variants(args.variants))
    if args.dry_run:
        print(json.dumps({'duration_ms': scenario['duration_ms'], 'schedule': schedule}, indent=2))
        return 0
    ok, receipt = run(args.dump, driver.mailbox(), scenario, schedule)
    for result in score(args.dump, receipt):
        print(f"{result['snapshot']:28} {result['verdict']:9} mean_abs={result['mean_abs']:.3f} "
              f"over={result['over_threshold']:.4f}")
    print('final', receipt['terminal'].get('state'), receipt['terminal'].get('error'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
