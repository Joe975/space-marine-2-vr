"""Run a test-driver scenario and request submitted-eye snapshots at chosen elapsed times.

Usage (from a loaded -TestDriver -ScriptedHead -EyeSnapshots launch):
  python scripts/capture_timed_snapshots.py DUMP_DIR SCENARIO_JSON name@ms [name@ms ...]

DUMP_DIR is the run's ordinary dump directory (build/ordinary/<date>/<tag>). Each name@ms
writes DUMP_DIR/<name>/{left,right}.ppm,pair.json once the scenario reaches that elapsed time.
Unlike capture_hand_visibility.py there is no fixed 14 s grid, so snapshots can land
mid-action (e.g. during a sprint or a held grip). Snapshots come from ordinary stereo pairs
only: menu-only frames (native pause) are not captured. The driver cancels a script on a
sampling gap over 250 ms (status clock_gap), e.g. when pause stops stereo; queue a new one.
"""
import json
import sys
import time
import uuid
from pathlib import Path

import vr_test_driver as driver


def main(argv):
    if len(argv) < 4:
        raise SystemExit(__doc__)
    dump, scenario = Path(argv[1]), Path(argv[2])
    shots = sorted((int(a.split('@')[1]), a.split('@')[0]) for a in argv[3:])
    mailbox = driver.mailbox()
    request_id = 'hands_' + uuid.uuid4().hex
    script = json.loads(scenario.read_text())
    (mailbox / (request_id + '.json')).write_text(json.dumps(script, indent=2))
    driver.write_request(mailbox, driver.compile_script(script, request_id))
    print(request_id, flush=True)
    taken, status = 0, {}
    deadline = time.monotonic() + script['duration_ms'] / 1000 + 30
    while time.monotonic() < deadline:
        try:
            status = json.loads((mailbox / 'status.json').read_text())
        except (OSError, json.JSONDecodeError):
            time.sleep(.05)
            continue
        elapsed = status.get('elapsed_ms', -1) if status.get('id') == request_id else -1
        while taken < len(shots) and elapsed >= shots[taken][0] + 1500:
            print('missed', shots[taken][1], flush=True)
            taken += 1
        if taken < len(shots) and status.get('state') == 'running' and shots[taken][0] <= elapsed:
            driver.write_request(dump, shots[taken][1] + '\n')
            print('snapshot', shots[taken][1], elapsed, flush=True)
            taken += 1
        if status.get('id') == request_id and status.get('state') not in ('preroll', 'running'):
            break
        time.sleep(.05)
    time.sleep(3)
    print('final', status.get('state'), status.get('error'))
    ok = status.get('state') == 'completed'
    for _, name in shots:
        saved = (dump / name / 'pair.json').exists()
        ok &= saved
        print(name, saved)
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
