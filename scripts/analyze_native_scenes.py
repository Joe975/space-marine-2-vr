"""Audit read-only native scene scope; this never proves a GPU stereo pair."""
import argparse
import json
import math
from pathlib import Path


def integer(value, minimum=0, maximum=(1 << 64) - 1):
    return type(value) is int and minimum <= value <= maximum


def analyze_outputs(scenes):
    """Historical descriptor correlation and fresh CPU bindings, never a lease."""
    errors, mappings = [], []
    try:
        for scene in scenes:
            a, b = scene['resource_before'], scene['resource_after']
            if (scene['output_wrapper_before'] != scene['output_wrapper_after']
                    or not integer(a['resource'], 1) or not integer(b['resource'], 1) or a['resource'] != b['resource']
                    or not integer(a['epoch'], 1) or not integer(b['epoch'], 1) or a['epoch'] != b['epoch']
                    or not integer(a['serial'], 1) or not integer(b['serial'], a['serial'])):
                raise ValueError('output resource, wrapper or descriptor epoch changed')
            signatures = set()
            fresh = False
            for item in (a, b):
                if (type(item['status']) is not int or item['status'] != 5 or item['lease_verified'] is not False
                        or type(item['rtvs']) is not list or not 1 <= len(item['rtvs']) <= 8):
                    raise ValueError('missing bounded historical output descriptors')
                handles = set()
                for view in item['rtvs']:
                    for name in ('resource', 'identity', 'handle', 'created', 'width', 'height',
                                 'storage', 'format', 'depth_or_array', 'mips', 'samples', 'planes', 'subresources'):
                        if not integer(view[name], 1):
                            raise ValueError('malformed output descriptor')
                    if (view['known'] is not True or view['resource'] != item['resource']
                            or view['handle'] in handles or view['created'] > item['serial']
                            or not integer(view['flags'], 0, (1 << 32) - 1)
                            or type(view['resource_dimension']) is not int or view['resource_dimension'] != 3
                            or not integer(view['dimension'], 0, 8)
                            or not integer(view['bound'], 0, item['serial'])
                            or not integer(view['command_list'])
                            or view['subresources'] != view['mips'] * view['depth_or_array'] * view['planes']):
                        raise ValueError('inconsistent output descriptor or subresource count')
                    if view['bound'] and (view['bound'] <= view['created'] or not view['command_list']):
                        raise ValueError('malformed output binding')
                    handles.add(view['handle'])
                    signatures.add(tuple(view[name] for name in ('identity', 'width', 'height', 'storage',
                                         'resource_dimension', 'flags', 'depth_or_array', 'mips', 'samples', 'planes', 'subresources')))
                    if item is b and view['bound'] > a['serial']:
                        fresh = True
            if len(signatures) != 1 or not fresh:
                raise ValueError('output identity changed or no binding occurred during the actual scene')
            identity, width, height, storage, _, flags, _, _, _, _, subresources = next(iter(signatures))
            mappings.append(dict(slot=scene['slot'], resource=a['resource'], identity=identity,
                                 width=width, height=height, storage=storage, flags=flags, subresources=subresources))
        if len(mappings) != 2 or len({m['resource'] for m in mappings}) != 2 or len({m['identity'] for m in mappings}) != 2:
            raise ValueError('control does not map two distinct output resources')
    except (KeyError, TypeError, ValueError, IndexError, OverflowError) as error:
        errors.append(str(error))
    return dict(output_mapping_pass=not errors, output_mapping_errors=errors, output_resources=mappings,
                output_lease_verified=False, final_writer_verified=False)


def analyze(capture):
    errors, ordinary, pip, outputs = [], [], [], []
    try:
        dispatch = capture['native_dispatch']
        sequence, native_list = dispatch['sequence'], dispatch['list']
        calls, scenes = dispatch['scene_calls'], dispatch['scenes']
        revision, receipt = dispatch['camera_revision'], dispatch['camera_list_receipt']
        if (dispatch['cpu_only'] is not True or dispatch['scene_observer'] is not True
                or not integer(sequence, 1) or not integer(native_list, 1, (1 << 64) - 0x29c7c)
                or not integer(dispatch['renderer'], 1) or not integer(calls, 1, 64)
                or type(scenes) is not list or len(scenes) != calls
                or type(dispatch['scene_refused']) is not int or dispatch['scene_refused'] != 0
                or type(dispatch['scene_dropped']) is not int or dispatch['scene_dropped'] != 0):
            raise ValueError('missing, refused or dropped scene calls')
        for name in ('revision', 'render_list', 'render_id', 'main_entry', 'pip_entry'):
            if not integer(receipt[name], 1):
                raise ValueError('malformed native CPU camera receipt')
        for name in ('main_slot', 'pip_slot'):
            if not integer(receipt[name], 0, 63):
                raise ValueError('malformed native CPU camera slot')
        if (not integer(revision, 1) or receipt['revision'] != revision
                or receipt['render_list'] != native_list
                or receipt['cpu_producing_frame_verified'] is not True
                or not integer(receipt['render_id'], 1)):
            raise ValueError('missing matching native CPU camera receipt')
        before, after = {}, {}
        for values, target in ((dispatch['before'], before), (dispatch['after'], after)):
            for view in values:
                slot = view['slot']
                if not integer(slot, 0, 63) or slot in target:
                    raise ValueError('invalid or duplicate view slot')
                target[slot] = view
        counters, seen = set(), set()
        for ordinal, scene in enumerate(scenes, 1):
            for name in ('ordinal', 'dispatch_sequence', 'renderer', 'list', 'entry', 'active_entry_after',
                         'camera_revision_before', 'camera_revision_after', 'view_flags_before', 'view_flags_after'):
                if not integer(scene[name], 1):
                    raise ValueError('malformed native scene receipt')
            if not integer(scene['render_counter_after'], 0, (1 << 32) - 1):
                raise ValueError('malformed native render counter')
            slot = scene['slot']
            if not integer(slot, 0, 63) or slot in seen:
                raise ValueError('invalid or repeated scene slot')
            seen.add(slot)
            entry = native_list + 0x168 + slot * 0xa60
            if (scene['ordinal'] != ordinal or type(scene['ordinal']) is not int
                    or scene['dispatch_sequence'] != sequence or scene['list'] != native_list or scene['renderer'] != dispatch['renderer']
                    or scene['entry'] != entry or scene['active_entry_after'] != entry
                    or scene['valid'] is not True or type(scene['refusal']) is not int or scene['refusal'] != 0
                    or not integer(scene['flags'], 0, (1 << 32) - 1)):
                raise ValueError('scene scope, ordering or active entry mismatch')
            counter = scene['render_counter_before']
            if (not integer(counter, 0, (1 << 32) - 1) or scene['render_counter_after'] != counter
                    or scene['camera_revision_before'] != revision or scene['camera_revision_after'] != revision):
                raise ValueError('scene counter or camera generation changed')
            counters.add(counter)
            flags = scene['view_flags_before']
            if (not integer(flags, 1) or scene['view_flags_after'] != flags
                    or before[slot]['flags'] != flags or after[slot]['flags'] != flags):
                raise ValueError('scene view flags disagree with dispatcher')
            a, b = before[slot]['world'], after[slot]['world']
            if (len(a) != 16 or len(b) != 16 or any(type(x) not in (int, float) or not math.isfinite(x) for x in a + b)
                    or any(abs(x - y) > 1e-4 for x, y in zip(a, b))):
                raise ValueError('scene world changed across dispatch')
            for name in ('output_wrapper_before', 'output_wrapper_after'):
                if not integer(scene[name], 1):
                    raise ValueError('missing actual scene output wrapper')
            if flags & 0x40:
                pip.append(slot)
            elif flags & 0x50000044 == 0:
                ordinary.append(slot)
                outputs.append(scene['output_wrapper_before'])
            else:
                raise ValueError('scene has an unclassified view role')
        if len(counters) != 1:
            raise ValueError('scenes do not share one native render counter')
        if (ordinary != [receipt['main_slot']] or pip != [receipt['pip_slot']]
                or receipt['main_entry'] != native_list + 0x168 + ordinary[0] * 0xa60
                or receipt['pip_entry'] != native_list + 0x168 + pip[0] * 0xa60):
            raise ValueError('observed scenes do not exactly cover the current main/PIP control')
    except (KeyError, TypeError, ValueError, IndexError, OverflowError) as error:
        errors.append(str(error))
    output_result = analyze_outputs(scenes) if not errors else dict(output_mapping_pass=False, output_mapping_errors=['CPU scene scope refused'])
    return dict(cpu_scene_scope_pass=not errors, ordinary_slots=ordinary, pip_slots=pip, **output_result,
                ordinary_output_wrappers=outputs, errors=errors, gpu_pair_verified=False,
                simulation_snapshot_verified=False, runtime_submission=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--require-outputs', action='store_true', help='also require distinct resource correlation and fresh scene bindings')
    args = parser.parse_args()
    results = []
    for phase in range(4):
        try:
            result = analyze(json.loads((args.root / f'phase_{phase}' / 'capture.json').read_text(encoding='utf-8-sig')))
        except (OSError, ValueError) as error:
            result = dict(cpu_scene_scope_pass=False, errors=[str(error)], gpu_pair_verified=False,
                          simulation_snapshot_verified=False, runtime_submission=False)
        results.append(dict(phase=phase, **result))
    summary = dict(cpu_scene_scope_pass=all(item['cpu_scene_scope_pass'] for item in results),
                   output_mapping_pass=all(item.get('output_mapping_pass', False) for item in results),
                   output_mapping_required=args.require_outputs,
                   phases=results, gpu_pair_verified=False, simulation_snapshot_verified=False,
                   runtime_submission=False)
    (args.root / 'scene_audit.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))
    return 0 if summary['cpu_scene_scope_pass'] and (not args.require_outputs or summary['output_mapping_pass']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
