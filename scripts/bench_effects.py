"""Effects benchmark: hold one head pose and step through shading.txt modes, one timed window each.

    .venv/Scripts/python.exe -I scripts/bench_effects.py DUMP_DIR --label PACK
        [--modes baseline,ssr,ssao,screen_shadows,all,baseline,mono] [--station 0:5]
        [--hold-s 60] [--settle-s 15]

Needs a launch with -Controls -TestDriver -ScriptedHead -PerfCampaign [-FlareHideControl] (the companion's PresentMon
and nvidia-smi logs and the stereo hold file). Modes are short names for shading.txt values that all
keep the default light union and shadow refresh, so each window changes one thing:

  baseline        light_union_shadow_refresh (the normal headset default)
  ssr / ssao / screen_shadows / all   ..._ssr / _ssao / _screen_shadows / _all (screen-space effects off)
  native_vol      light_union_shadow_refresh_native_volumetrics
  mono            baseline shading with the stereo hold set (one native view per frame; reference)
  flares          baseline shading with lamp flares hidden (flare_hide.txt; launch with -FlareHideControl)
  everything      all screen-space effects off and lamp flares hidden
  hzb_off / hzb_left / hzb_objects / hzb_shared / hzb_native   hzb.txt off / left_only / shared_objects / shared / native
                  (launch with -HzbControl)
  mono_hzb        mono with the game's occlusion culling on
  no_refresh      light_union alone: the game's cascade reuse, no forced shadow refresh

--snapshots also saves one eye snapshot per step (DUMP_DIR/fx_<label>_<step>/left.ppm, right.ppm;
launch with -EyeSnapshots), taken 5 s before the measured part of the hold.

Each mode gets its own scripted-head hold of --hold-s seconds at the station (yaw:pitch, degrees,
relative to the start pose) with the hands lowered; the first --settle-s seconds are not measured.
Writes DUMP_DIR/bench_effects_<label>.json with the local wall-clock window of every mode and prints
the analyze_campaign_perf.py command that reports them. shading.txt and the stereo hold are restored.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
import uuid
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vr_test_driver as driver  # noqa: E402

BASE = 'light_union_shadow_refresh'
MODES = {'baseline': BASE, 'ssr': BASE + '_ssr', 'ssao': BASE + '_ssao',
         'screen_shadows': BASE + '_screen_shadows', 'all': BASE + '_all',
         'native_vol': BASE + '_native_volumetrics', 'mono': BASE, 'flares': BASE, 'everything': BASE + '_all',
         'hzb_off': BASE, 'hzb_left': BASE, 'hzb_native': BASE, 'hzb_shared': BASE, 'hzb_objects': BASE, 'mono_hzb': BASE, 'no_refresh': 'light_union'}
HZB = {'hzb_off': 'off', 'hzb_left': 'left_only', 'hzb_native': 'native', 'hzb_shared': 'shared', 'hzb_objects': 'shared_objects', 'mono_hzb': 'left_only'}
MONO = {'mono', 'mono_hzb'}
FLARES_HIDDEN = {'flares', 'everything'}
LEFT_HAND = [-0.25, -0.6, 0, 0, 0, 0]
RIGHT_HAND = [0.25, -0.6, 0, 0, 0, 0]


def plan(modes: list[str], hold_s: int, settle_s: int) -> list[dict]:
    if not modes:
        raise ValueError('need at least one mode')
    unknown = [m for m in modes if m not in MODES]
    if unknown:
        raise ValueError(f'unknown mode(s) {unknown}; known: {sorted(MODES)}')
    if not 0 <= settle_s < hold_s <= 120:
        raise ValueError('need 0 <= settle < hold <= 120 seconds (the driver limit)')
    labels, out = {}, []
    for m in modes:
        labels[m] = labels.get(m, 0) + 1
        out.append({'mode': m, 'label': m if labels[m] == 1 else f'{m}#{labels[m]}',
                    'shading': MODES[m], 'stereo_hold': 'mono' if m in MONO else 'stereo',
                    'flare_hide': 'hide' if m in FLARES_HIDDEN else 'native', 'hzb': HZB.get(m, 'off')})
    return out


def hold_script(yaw: float, pitch: float, hold_s: int) -> dict:
    head = [0, 0, 0, yaw, pitch, 0]
    frame = {'head': head, 'left': LEFT_HAND, 'right': RIGHT_HAND}
    return {'description': 'Effects benchmark: hold one pose, hands lowered.', 'mode': 'scripted_head',
            'duration_ms': hold_s * 1000, 'frames': [{'ms': 0, **frame}, {'ms': hold_s * 1000, **frame}]}


def write_control(path: Path, value: str) -> None:
    temporary = path.with_suffix('.tmp')
    temporary.write_text(value + '\n', encoding='ascii')
    temporary.replace(path)


def snapshot_name(label: str, step_label: str) -> str:
    return re.sub(r'[^A-Za-z0-9_]', '_', f'fx_{label}_{step_label}')[:40]


def wait_done(mailbox: Path, request_id: str, timeout_s: float, at_ms: int = -1, on_time=None) -> dict:
    """Wait for the hold to end; call on_time() once when it has run at_ms (the eye snapshot)."""
    deadline, status, fired = time.monotonic() + timeout_s, {}, False
    while time.monotonic() < deadline:
        try:
            status = json.loads((mailbox / 'status.json').read_text())
        except (OSError, json.JSONDecodeError):
            status = {}
        current = status.get('id') == request_id
        if current and status.get('state') not in ('preroll', 'running'):
            return status
        if on_time and not fired and current and status.get('state') == 'running' and status.get('elapsed_ms', -1) >= at_ms:
            on_time()
            fired = True
        time.sleep(.2)
    return status


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dump', type=Path)
    parser.add_argument('--label', required=True)
    parser.add_argument('--modes', default='baseline,ssr,ssao,screen_shadows,all,baseline,mono')
    parser.add_argument('--station', default='0:5')
    parser.add_argument('--hold-s', type=int, default=60)
    parser.add_argument('--settle-s', type=int, default=15)
    parser.add_argument('--snapshots', action='store_true', help='one eye snapshot per step, 5 s before measuring (-EyeSnapshots)')
    args = parser.parse_args(argv)
    steps = plan(args.modes.split(','), args.hold_s, args.settle_s)
    yaw, pitch = (float(x) for x in args.station.split(':'))
    dump = args.dump.resolve()
    shading, hold, flare = dump / 'shading.txt', dump / 'stereo_hold.txt', dump / 'flare_hide.txt'
    hzb = dump / 'hzb.txt'
    if not shading.is_file():
        raise SystemExit('shading.txt missing: launch with the headset launcher')
    if any(s['stereo_hold'] == 'mono' for s in steps) and not hold.is_file():
        raise SystemExit('stereo_hold.txt missing: the mono step needs -PerfCampaign')
    if any(s['flare_hide'] == 'hide' for s in steps) and not flare.is_file():
        raise SystemExit('flare_hide.txt missing: launch with -FlareHideControl')
    if any(s['mode'] in HZB for s in steps) and not hzb.is_file():
        raise SystemExit('hzb.txt missing: launch with -HzbControl')
    mailbox = driver.mailbox()
    original = {p: p.read_text(encoding='ascii').strip() for p in (shading, hold, flare, hzb) if p.is_file()}
    results = []
    try:
        for step in steps:
            write_control(shading, step['shading'])
            if hold.is_file():
                write_control(hold, step['stereo_hold'])
            if flare.is_file():
                write_control(flare, step['flare_hide'])
            if hzb.is_file():
                write_control(hzb, step['hzb'])
            request_id = 'bench_' + uuid.uuid4().hex[:16]
            script = hold_script(yaw, pitch, args.hold_s)
            (mailbox / (request_id + '.json')).write_text(json.dumps(script, indent=2))
            driver.write_request(mailbox, driver.compile_script(script, request_id))
            start = time.time()
            shot = snapshot_name(args.label, step['label']) if args.snapshots else None
            status = wait_done(mailbox, request_id, args.hold_s + 40, max(0, args.settle_s - 5) * 1000,
                               (lambda name=shot: driver.write_request(dump, name + '\n')) if shot else None)
            end = time.time()
            ok = status.get('id') == request_id and status.get('state') == 'completed'
            window = (datetime.fromtimestamp(min(start + args.settle_s, end)).strftime('%H:%M:%S'),
                      datetime.fromtimestamp(end - 1).strftime('%H:%M:%S'))
            results.append({**step, 'request_id': request_id, 'completed': ok, 'state': status.get('state'),
                            'window': window, 'snapshot': shot})
            print(f"{step['label']}: {window[0]}-{window[1]} {'ok' if ok else status.get('state')}", flush=True)
    finally:
        for path, text in original.items():
            write_control(path, text)
    out = dump / f'bench_effects_{args.label}.json'
    out.write_text(json.dumps({'label': args.label, 'station': [yaw, pitch], 'hold_s': args.hold_s,
                               'settle_s': args.settle_s, 'steps': results}, indent=2) + '\n')
    windows = ' '.join(f"--window {r['window'][0]}-{r['window'][1]}={args.label}:{r['label']}" for r in results)
    print(f'analysis: .venv/Scripts/python.exe -I scripts/analyze_campaign_perf.py {dump} {windows} --min-seconds 20')
    return 0 if all(r['completed'] for r in results) else 1


if __name__ == '__main__':
    sys.exit(main())
