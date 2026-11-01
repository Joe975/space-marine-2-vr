"""Queue a test-driver scenario, writing eye-snapshot requests and takeover-control words on time.

Usage (from a loaded -TestDriver -ScriptedHead -EyeSnapshots [-TakeoverControl] launch):
  python scripts/capture_animcam_live.py DUMP_DIR SCENARIO_JSON item@ms [item@ms ...]

An item is either a snapshot name (letters, digits, '-', '_') or 'control=WORD', which writes
WORD ('takeover' or 'off') to DUMP_DIR/takeover.txt (docs/ANIMATION_CAMERA.md). Run it from the
tree whose launcher started the game: it reads that tree's build/test_driver/current.json.
Exit status 0 when the scenario completed (or, with --allow-cancel, ended) and every snapshot
was saved.
"""
import argparse
import json
import sys
import time
import uuid
from pathlib import Path

import vr_test_driver as driver


def parse(items):
    out = []
    for item in items:
        name, _, ms = item.rpartition('@')
        if not name or not ms.isdigit():
            raise SystemExit(f'bad item {item!r}: expected name@ms or control=WORD@ms')
        if name.startswith('control='):
            word = name.split('=', 1)[1]
            if word not in ('takeover', 'off'):
                raise SystemExit(f'control word must be takeover or off, got {word!r}')
            out.append((int(ms), 'control', word))
        else:
            out.append((int(ms), 'snapshot', name))
    return sorted(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dump', type=Path)
    parser.add_argument('scenario', type=Path)
    parser.add_argument('items', nargs='*')
    parser.add_argument('--allow-cancel', action='store_true', help='a cancelled script (e.g. clock_gap at a pause) still counts')
    args = parser.parse_args(argv)
    plan = parse(args.items)
    mailbox = driver.mailbox()
    request_id = 'animcam_' + uuid.uuid4().hex
    script = json.loads(args.scenario.read_text())
    (mailbox / (request_id + '.json')).write_text(json.dumps(script, indent=2))
    driver.write_request(mailbox, driver.compile_script(script, request_id))
    print(request_id, flush=True)
    done, status = 0, {}
    deadline = time.monotonic() + script['duration_ms'] / 1000 + 30
    while time.monotonic() < deadline:
        try:
            status = json.loads((mailbox / 'status.json').read_text())
        except (OSError, json.JSONDecodeError):
            time.sleep(.05)
            continue
        elapsed = status.get('elapsed_ms', -1) if status.get('id') == request_id else -1
        while done < len(plan) and status.get('state') == 'running' and plan[done][0] <= elapsed:
            ms, kind, value = plan[done]
            if kind == 'control':
                staged = args.dump / 'takeover.txt.tmp'
                staged.write_text(value + '\n', encoding='ascii')
                staged.replace(args.dump / 'takeover.txt')
            else:
                driver.write_request(args.dump, value + '\n')
            print(kind, value, elapsed, flush=True)
            done += 1
        if status.get('id') == request_id and status.get('state') not in ('preroll', 'running'):
            break
        time.sleep(.05)
    time.sleep(3)
    print('final', status.get('state'), status.get('error'))
    ok = status.get('state') == 'completed' or (args.allow_cancel and status.get('id') == request_id)
    for ms, kind, value in plan:
        if kind == 'snapshot':
            saved = (args.dump / value / 'pair.json').exists()
            ok &= saved
            print(value, saved)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
