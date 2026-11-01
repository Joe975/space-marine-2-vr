"""Audit ordinary-only CPU colour boundaries; never authenticates a GPU eye pair."""
import argparse
import json
import math
from pathlib import Path


def integer(value, low=0, high=(1 << 64) - 1):
    return type(value) is int and low <= value <= high


def analyze(capture):
    errors, resources = [], []
    try:
        if (capture['model'] != 'ordinary_colour_boundaries_v1' or capture['cpu_only'] is not True
                or capture['pip_enabled'] is not False or capture['gpu_pair_verified'] is not False
                or capture['runtime_submission'] is not False):
            raise ValueError('wrong ordinary-only CPU observation model')
        for name in ('scene_refused', 'scene_dropped', 'refused', 'errors'):
            if type(capture[name]) is not int or capture[name] != 0:
                raise ValueError('missing or refused boundary evidence')
        for name in ('dispatch', 'list', 'renderer', 'display_width', 'display_height'):
            if not integer(capture[name], 1):
                raise ValueError('malformed dispatch/display scope')
        if type(capture['scene_calls']) is not int or capture['scene_calls'] != 1 or len(capture['scenes']) != 1:
            raise ValueError('control requires exactly one ordinary scene')
        primary, scene = capture['primary'], capture['scenes'][0]
        for name in ('add_list', 'render_list', 'entry', 'revision', 'render_id'):
            if not integer(primary[name], 1):
                raise ValueError('missing exact primary producer receipt')
        if (not integer(primary['token'], 0, (1 << 32) - 1) or not integer(primary['slot'], 0, 63)
                or primary['render_list'] != capture['list'] or primary['add_list'] != capture['list'] + 0x160
                or primary['entry'] != capture['list'] + 0x168 + primary['slot'] * 0xa60):
            raise ValueError('primary receipt list/slot mismatch')
        if (type(scene['ordinal']) is not int or scene['ordinal'] != 1 or type(scene['refusal']) is not int or scene['refusal'] != 0
                or not integer(scene['slot'], 0, 63) or scene['slot'] != primary['slot']
                or scene['entry'] != primary['entry'] or scene['active_entry_after'] != primary['entry']
                or not integer(scene['flags'], 1) or scene['flags'] & 0x50000044 or not scene['flags'] & 0x20000
                or not integer(scene['flags_after'], 1) or scene['flags_after'] != scene['flags']
                or not integer(scene['counter_before'], 0, (1 << 32) - 1) or not integer(scene['counter_after'], 0, (1 << 32) - 1)
                or scene['counter_before'] != scene['counter_after']
                or not integer(scene['revision_before'], 1) or not integer(scene['revision_after'], 1)
                or scene['revision_before'] != primary['revision'] or scene['revision_after'] != primary['revision']):
            raise ValueError('ordinary scene changed scope, flags, counter or producer revision')
        a, b = capture['views'], capture['views_after']
        if len(a) != 1 or len(b) != 1:
            raise ValueError('control contains additional views')
        for view in a + b:
            if (not integer(view['slot'], 0, 63) or view['slot'] != primary['slot'] or view['flags'] != scene['flags']
                    or len(view['world']) != 16 or any(type(x) not in (int, float) or not math.isfinite(x) for x in view['world'])):
                raise ValueError('invalid ordinary world metadata')
        if any(abs(x-y) > 1e-4 for x, y in zip(a[0]['world'], b[0]['world'])):
            raise ValueError('ordinary world changed across the dispatch')
        events = capture['events']
        if len(events) != 3 or [e['stage'] for e in events] != [0, 1, 2] or any(type(e['stage']) is not int for e in events):
            raise ValueError('missing, duplicate or reordered post/compositor boundaries')
        previous = 0
        identities, wrappers, epochs = set(), set(), set()
        for event in events:
            for name in ('serial', 'dispatch', 'ordinal', 'entry', 'render_id', 'revision', 'wrapper', 'resource', 'scene_begin_epoch', 'scene_begin_serial', 'epoch', 'serial_at_capture'):
                if not integer(event[name], 1):
                    raise ValueError('malformed colour boundary receipt')
            if (event['serial'] <= previous or event['dispatch'] != capture['dispatch'] or event['ordinal'] != 1
                    or not integer(event['slot'], 0, 63) or event['slot'] != primary['slot'] or event['entry'] != primary['entry']
                    or event['render_id'] != primary['render_id'] or not integer(event['token'], 0, (1 << 32) - 1) or event['token'] != primary['token']
                    or event['revision'] != primary['revision'] or type(event['status']) is not int or event['status'] != 5
                    or event['epoch'] != event['scene_begin_epoch'] or event['serial_at_capture'] < event['scene_begin_serial']
                    or event['lease_verified'] is not False or event['final_writer_verified'] is not False):
                raise ValueError('colour boundary producer or descriptor scope mismatch')
            previous = event['serial']
            wrappers.add(event['wrapper'])
            epochs.add(event['epoch'])
            views, fresh, seen = event['rtvs'], False, set()
            if type(views) is not list or not 1 <= len(views) <= 8:
                raise ValueError('missing bounded actual colour descriptors')
            for view in views:
                for name in ('resource', 'identity', 'handle', 'created', 'width', 'height', 'storage', 'format', 'subresources'):
                    if not integer(view[name], 1):
                        raise ValueError('malformed colour resource descriptor')
                if (view['resource'] != event['resource'] or view['known'] is not True or view['handle'] in seen
                        or view['created'] > event['serial_at_capture'] or not integer(view['bound'], 0, event['serial_at_capture'])
                        or not integer(view['command_list']) or not integer(view['flags'], 0, (1 << 32) - 1)
                        or not integer(view['dimension'], 0, 8)
                        or view['width'] != capture['display_width'] or view['height'] != capture['display_height']):
                    raise ValueError('colour resource is unknown, inconsistent or not full display resolution')
                if view['bound'] and (view['bound'] <= view['created'] or not view['command_list']):
                    raise ValueError('malformed colour resource binding')
                seen.add(view['handle'])
                identities.add((event['resource'], view['identity'], view['width'], view['height'], view['storage'], view['flags'], view['subresources']))
                fresh |= view['bound'] > event['scene_begin_serial']
            if not fresh:
                raise ValueError('colour resource has no binding during this actual ordinary scene')
            resources.append(event['resource'])
        if len(identities) != 1 or len(wrappers) != 1 or len(epochs) != 1:
            raise ValueError('colour source changed between post and compositor boundaries')
    except (KeyError, TypeError, ValueError, IndexError, OverflowError) as error:
        errors.append(str(error))
    return dict(ordinary_cpu_boundary_pass=not errors, errors=errors, resources=resources,
                gpu_pair_verified=False, output_lease_verified=False, final_writer_verified=False,
                simulation_snapshot_verified=False, runtime_submission=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    results = []
    for index in range(4):
        try:
            result = analyze(json.loads((args.root/f'ordinary_{index}.json').read_text(encoding='utf-8-sig')))
        except (OSError, ValueError) as error:
            result = dict(ordinary_cpu_boundary_pass=False, errors=[str(error)])
        results.append(dict(index=index, **result))
    summary = dict(ordinary_cpu_boundary_pass=all(r['ordinary_cpu_boundary_pass'] for r in results),
                   controls=results, gpu_pair_verified=False, runtime_submission=False)
    (args.root/'ordinary_audit.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))
    return 0 if summary['ordinary_cpu_boundary_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
