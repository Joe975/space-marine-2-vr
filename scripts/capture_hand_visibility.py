"""Run the fixed 50cm hand sweep and request eight same-frame eye snapshots.

Requires a loaded, focused -TestDriver -ScriptedHead -EyeSnapshots launch.
Pass its ordinary dump directory as --output. Captures measure images, not FPS.
"""
import argparse
import json
import time
import uuid
from pathlib import Path
import vr_test_driver as driver


DEFAULT_NAMES = tuple('heading_' + str(i * 45) for i in range(8))


def capture_name(index, status, request_id, names=DEFAULT_NAMES):
    if (0 <= index < len(names) and status.get('id') == request_id and status.get('state') == 'running'
            and index * 14000 + 3500 <= status.get('elapsed_ms', -1) < index * 14000 + 8000):
        return names[index]
    return None


def joined_metadata(pair, draw, name):
    if not isinstance(pair, dict) or not isinstance(draw, dict):
        return False
    target = pair.get('joined_capture')
    eyes = pair.get('eyes')
    dispatch, render_id = draw.get('dispatch'), draw.get('render_id')
    return bool(isinstance(target, dict) and target.get('verified') is True and target.get('id') == name and
                pair.get('submitted') is True and draw.get('joined_capture_requested') is True and
                type(dispatch) is int and dispatch > 0 and type(render_id) is int and render_id > 0 and
                pair.get('dispatch') == dispatch == target.get('draw_dispatch') and target.get('draw_render_id') == render_id and
                isinstance(eyes, list) and len(eyes) == 2 and all(isinstance(e, dict) and type(e.get('eye')) is int for e in eyes) and sorted(e['eye'] for e in eyes) == [0, 1] and
                all(e.get('render_id') == render_id for e in eyes))


def run(directory, output, script_path=None, names=DEFAULT_NAMES, draw_visibility=False, *, joined_capture=False):
    if not output.is_absolute() or not output.is_dir():
        raise ValueError('Output must be the existing absolute ordinary dump directory')
    if (not 1 <= len(names) <= 8 or len(set(names)) != len(names) or
            any(not name or len(name) > 40 or any(not (c.isascii() and (c.isalnum() or c in '_-')) for c in name) for name in names)):
        raise ValueError('One to eight unique safe capture names required')
    if any((output / name).exists() for name in names):
        raise ValueError('Use a fresh capture launch; heading output already exists')
    if joined_capture and not draw_visibility:
        raise ValueError('Joined capture requires the native draw visibility probe')
    request_id = 'hands_' + uuid.uuid4().hex
    script = json.loads((script_path or driver.ROOT / 'config/test_driver/hands_50cm_rotation.json').read_text())
    (directory / (request_id + '.json')).write_text(json.dumps(script, indent=2))
    driver.write_request(directory, driver.compile_script(script, request_id))
    print(request_id, flush=True)
    captures, status = [], {}
    deadline = time.monotonic() + 135
    while time.monotonic() < deadline:
        try:
            status = json.loads((directory / 'status.json').read_text())
        except (OSError, json.JSONDecodeError):
            time.sleep(.1)
            continue
        name = capture_name(len(captures), status, request_id, names)
        if name:
            if joined_capture:
                temporary = output / 'joined_capture_request.tmp'
                temporary.write_text(name + '\n', encoding='ascii')
                temporary.replace(output / 'joined_capture_request.txt')
            else:
                driver.write_request(output, name + '\n')
            if draw_visibility and not joined_capture:
                temporary = output / 'draw_visibility_request.tmp'
                temporary.write_text(name + '\n', encoding='ascii')
                temporary.replace(output / 'draw_visibility_request.txt')
            captures.append(dict(name=name, status=status, wall_time=time.time()))
            print(json.dumps(captures[-1]), flush=True)
        if status.get('id') == request_id and status.get('state') not in ('preroll', 'running'):
            break
        time.sleep(.1)
    complete = status.get('id') == request_id and status.get('state') == 'completed'
    for capture in captures:
        folder = output / capture['name']
        capture['saved'] = all((folder / name).is_file() for name in ('left.ppm', 'right.ppm', 'pair.json'))
        if draw_visibility:
            try:
                receipt = json.loads((output / (capture['name'] + '.draw_visibility.json')).read_text())
                capture['draw_saved'] = (receipt.get('model') == 'ordinary_object_visibility_v1' and
                                         len(receipt.get('eyes', [])) == 2 and
                                         all(e.get('complete') and not e.get('invalid') and not e.get('truncated') for e in receipt['eyes']))
                capture['draw_dispatch'] = receipt.get('dispatch')
                if joined_capture:
                    pair = json.loads((folder / 'pair.json').read_text())
                    capture['exact_image_draw_join'] = joined_metadata(pair, receipt, capture['name'])
            except (OSError, ValueError):
                capture['draw_saved'] = False
                if joined_capture:
                    capture['exact_image_draw_join'] = False
    result = dict(id=request_id, captures=captures, terminal=status,
                  joined_capture=joined_capture,
                  capture_complete=complete and len(captures) == len(names) and all(c['saved'] and (not draw_visibility or c['draw_saved']) and (not joined_capture or c.get('exact_image_draw_join') is True) for c in captures))
    (output / 'sweep_requests.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(terminal=status, capture_complete=result['capture_complete'])), flush=True)
    return result['capture_complete']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--draw-visibility', action='store_true')
    parser.add_argument('--joined-capture', action='store_true')
    args = parser.parse_args()
    raise SystemExit(0 if run(driver.mailbox(args.directory), args.output, draw_visibility=args.draw_visibility or args.joined_capture, joined_capture=args.joined_capture) else 1)
