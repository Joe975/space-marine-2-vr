"""Check observed native PIP writer batches, without claiming final-writer or frame identity."""
import argparse
import importlib.util
import json
import math
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


tone = module('tone', REPO / 'scripts/analyze_tonemap_sweep.py')
KINDS = ('draw', 'indexed', 'clear', 'copy', 'copy_texture', 'resolve')


def writer_errors(gpu):
    errors = []
    if not tone.valid_gpu(gpu):
        errors.append('invalid completed GPU batch')
    counts = gpu.get('writer_counts', {})
    if (set(counts) not in (set(KINDS), set(KINDS) | {'indirect_possible'}) or any(type(v) is not int or v < 0 for v in counts.values())
            or sum(counts.values()) != gpu.get('writes')):
        errors.append('invalid writer-kind counts')
    if any(type(gpu.get(key)) is not int or gpu[key] <= 0 for key in
           ('last_write_callsite', 'execute_callsite', 'last_write_thread')):
        errors.append('missing CPU recording/submission origins')
    return errors


def origin_errors(capture):
    origin = capture['gpu_observed_batch'].get('observed_writer_origin')
    if not isinstance(origin, dict):
        return ['no uniquely matched source camera job']
    try:
        source, rendered = origin['source_world'], origin['render_world']
        if (type(origin['source_render_id']) is not int or origin['source_render_id'] <= 0
                or type(origin['source_token']) is not int or not 0 <= origin['source_token'] <= 0xffffffff
                or origin['record'] != capture['pip_record_index']
                or len(source) != 16 or len(rendered) != 16
                or not all(math.isfinite(v) for v in source + rendered)
                or max(abs(a-b) for a, b in zip(source, rendered)) > 1.01e-4):
            return ['invalid source camera job/pose binding']
        if origin.get('identity_model') == 'native_copy_slot_v1':
            receipt = capture['native_dispatch']['camera_list_receipt']
            if (receipt['model'] != 'native_add_slot_v1' or receipt['cpu_producing_frame_verified'] is not True
                    or receipt['render_id'] != origin['source_render_id'] or receipt['token'] != origin['source_token']
                    or any(type(origin[k]) is not int or origin[k] <= 0 for k in
                           ('camera_revision', 'camera_render_list', 'camera_pip_entry'))
                    or receipt['revision'] != origin['camera_revision'] or receipt['render_list'] != origin['camera_render_list']
                    or receipt['pip_entry'] != origin['camera_pip_entry']
                    or receipt['add_list'] != receipt['render_list'] + 0x160
                    or any(type(receipt[k]) is not int or not 0 <= receipt[k] < 64 for k in ('main_slot', 'pip_slot'))
                    or receipt['main_slot'] == receipt['pip_slot']
                    or any(receipt[k+'_entry'] != receipt['add_list'] + 8 + receipt[k+'_slot'] * 0xa60 for k in ('main', 'pip'))):
                return ['invalid causal native camera slot binding']
        elif (origin.get('identity_model') is not None or origin['unique_recent_pose'] is not True
              or origin['unique_over_experiment'] is not True):
            return ['invalid unique-pose camera binding']
    except (KeyError, TypeError, ValueError):
        return ['malformed source camera job/pose binding']
    return []


def boundary_errors(capture):
    gpu = capture['gpu_observed_batch']
    boundary = capture.get('readback_boundary')
    if not isinstance(boundary, dict):
        return ['missing guarded readback boundary']
    try:
        if (capture['readback_mode'] != 'pip_at_writer_batch_main_at_present' or capture['paired_readback'] is not False
                or boundary['copy_state_source'] != 'observed_raster_and_legacy_transitions'
                or type(boundary['state_from_raster']) is not bool or type(boundary['common_state_decay']) is not bool
                or (boundary['common_state_decay'] and boundary['copy_source_state'] != 0)
                or (boundary['state_from_raster'] and boundary['copy_source_state'] != (0 if boundary['common_state_decay'] else 4))
                or type(boundary['copy_source_state']) is not int or boundary['copy_source_state'] not in (0, 4, 128, 192, 2048)
                or any(type(boundary[k]) is not int or boundary[k] <= 0 for k in
                ('writer_serial', 'writer_submission', 'copy_submission', 'executions', 'queue', 'resource', 'commands'))
                or type(gpu['submission_sequence']) is not int or gpu['submission_sequence'] <= 0
                or type(gpu['lists']) is not int or not 1 <= gpu['lists'] <= 1024
                or type(gpu['observed_target_lists']) is not int or gpu['observed_target_lists'] != 1
                or gpu['all_lists_tracked'] is not True
                or boundary['writer_serial'] != gpu['serial']
                or boundary['writer_submission'] != gpu['submission_sequence']
                or boundary['copy_submission'] != boundary['writer_submission'] + 1
                or boundary['executions'] != 1 or boundary['queue'] != gpu['queue']
                or boundary['resource'] != gpu['resource'] or boundary['fence_completed'] is not True
                or type(boundary['retries']) is not int or boundary['retries'] < 0
                or boundary['observed_queue_boundary_verified'] is not True
                or boundary['all_writer_coverage_verified'] is not False
                or boundary['source_version_verified'] is not False):
            return ['invalid guarded readback boundary or unsupported identity claim']
    except (KeyError, TypeError):
        return ['malformed guarded readback boundary']
    return []


def resource_errors(capture):
    """Independently audit single-subresource RT inference and whole-batch decay."""
    try:
        desc = capture['pip_resource_desc']
        keys = ('dimension', 'width', 'height', 'depth_or_array_size', 'mip_levels', 'format',
                'sample_count', 'sample_quality', 'layout', 'flags', 'plane_count', 'subresource_count')
        if any(type(desc[k]) is not int or desc[k] < 0 for k in keys):
            return ['invalid resource descriptor types']
        boundary = capture['readback_boundary']
        count = desc['mip_levels'] * desc['depth_or_array_size'] * desc['plane_count']
        if (desc['dimension'] != 3 or not 1 <= desc['width'] <= 1280 or not 1 <= desc['height'] <= 720
                or desc['sample_count'] != 1 or desc['sample_quality'] != 0
                or desc['format'] not in (10, 27, 28, 29, 87, 90, 91) or desc['layout'] != 0
                or not desc['flags'] & 1 or desc['flags'] & 2
                or count != 1 or count != desc['subresource_count']
                or (boundary['state_from_raster'] and count != 1)
                or boundary['common_state_decay'] != bool(desc['flags'] & 32)):
            return ['resource descriptor contradicts supported copy/state/decay model']
    except (KeyError, TypeError):
        return ['missing or malformed resource descriptor']
    return []


def access_errors(capture):
    """Independently audit the narrow allocation proof behind every exclusion."""
    try:
        value = capture['readback_boundary']['access_exclusions']
        writers = value['writers']
        extended = value['extended']
        counts = [value['aliases'], value['uavs'], value['indirect_compute'], value['indirect_raster'], *writers, *extended]
        if (value['model'] != 'private_committed_gpu_rt_v3' or type(writers) is not list or len(writers) != 5
                or type(extended) is not list or len(extended) != 8 or extended[3] or extended[4]
                or any(type(n) is not int or not 0 <= n < 2**64 for n in counts)
                or writers[2] or value['indirect_compute'] + value['indirect_raster'] != writers[1]):
            return ['unsupported access exclusion counts/model']
        if value['allocation'] is not None and type(value['allocation']) is not dict:
            return ['invalid allocation observation']
        if not any(counts):
            return []
        a = value['allocation']
        keys = ('api', 'kind', 'serial', 'device', 'resource', 'resource_interface', 'heap', 'heap_serial',
                'offset', 'size', 'heap_size', 'alignment', 'heap_flags', 'actual_heap_flags',
                'heap_type', 'cpu_page', 'memory_pool', 'creation_node', 'visible_node')
        if any(type(a[k]) is not int or not 0 <= a[k] < 2**64 for k in keys):
            return ['invalid allocation evidence types']
        desc = capture['pip_resource_desc']
        if (a['api'] not in (0, 2) or a['kind'] != 1 or not a['serial'] or not a['device'] or not a['resource']
                or a['resource_interface'] != capture['gpu_observed_batch']['resource']
                or a['resource_interface'] != capture['readback_boundary']['resource']
                or a['heap'] or a['heap_serial'] or a['offset'] or not a['size'] or a['size'] == 2**64-1
                or a['size'] != a['heap_size'] or not a['alignment'] or a['alignment'] & (a['alignment']-1)
                or a['size'] % a['alignment'] or (a['heap_flags'] | a['actual_heap_flags']) & ~0x84
                or a['actual_heap_flags'] & a['heap_flags'] != a['heap_flags'] or a['properties_known'] is not True
                or a['heap_type'] != 1 or a['cpu_page'] != 0 or a['memory_pool'] != 0
                or a['creation_node'] != 1 or a['visible_node'] != 1
                or type(desc['flags']) is not int or desc['flags'] != 1
                or type(desc['dimension']) is not int or desc['dimension'] != 3
                or type(desc['layout']) is not int or desc['layout'] != 0
                or type(desc['sample_quality']) is not int or desc['sample_quality'] != 0
                or any(type(desc[k]) is not int or desc[k] != 1 for k in ('mip_levels', 'depth_or_array_size', 'sample_count'))):
            return ['access exclusion lacks private committed non-UAV GPU target proof']
    except (KeyError, TypeError):
        return ['missing or malformed access exclusion receipt']
    return []


def barrier_errors(capture):
    try:
        observation = capture['readback_boundary']['barrier_observation']
        if (observation['model'] != 'legacy_with_recording_identity_v6'
                or observation['capabilities_known'] is not True
                or type(observation['enhanced_supported']) is not bool
                or type(observation['enhanced_hook_installed']) is not bool
                or (observation['enhanced_supported'] and not observation['enhanced_hook_installed'])
                or type(observation['base_writer_hook_mask']) is not int or observation['base_writer_hook_mask'] != 31
                or observation['binding_capabilities_known'] is not True
                or type(observation['list4_supported']) is not bool
                or type(observation['binding_hook_mask']) is not int
                or observation['binding_hook_mask'] != (7 if observation['list4_supported'] else 1)
                or observation['signature_hook_installed'] is not True or observation['recording_binding_coverage_verified'] is not True
                or observation['command_object_receipts_verified'] is not True or observation['base_implementations_verified'] is not True
                or observation['extended_capabilities_known'] is not True
                or type(observation['list1_supported']) is not bool or type(observation['list6_supported']) is not bool
                or type(observation['extended_hook_mask']) is not int
                or observation['extended_hook_mask'] != (131 | (4 if observation['list1_supported'] else 0)
                                                        | (56 if observation['list4_supported'] else 0) | (64 if observation['list6_supported'] else 0))
                or type(capture['gpu_observed_batch']['writer_counts']['indirect_possible']) is not int
                or capture['gpu_observed_batch']['writer_counts']['indirect_possible'] < 0
                or type(observation['hazards']) is not int or observation['hazards'] != 0
                or type(capture['gpu_observed_batch']['barrier_hazards']) is not int
                or capture['gpu_observed_batch']['barrier_hazards'] != observation['hazards']):
            return ['unsupported or unobserved barrier/base-writer path cannot certify this copy']
    except (KeyError, TypeError):
        return ['missing or malformed barrier observation receipt']
    return access_errors(capture)


def pool_errors(capture):
    """Audit a Present-time native slot observation, never GPU lifetime proof."""
    try:
        value = capture['pip_native_pool']
        addresses = ('context', 'manager', 'records', 'camera', 'pool', 'slots', 'slot', 'texture', 'resource')
        if (value['model'] != 'present_slot_observation_v1'
                or any(type(value[k]) is not int or not 0 < value[k] < 2**64 for k in addresses)
                or type(value['record_index']) is not int or not 0 <= value['record_index'] < 64
                or value['record_index'] != capture['pip_record_index']
                or type(value['texture_index']) is not int or not 0 <= value['texture_index'] < 1024
                or value['slot'] != value['slots'] + value['texture_index'] * 24
                or value['resource'] != capture['gpu_observed_batch']['resource']
                or any(type(value[k]) is not int or value[k] != capture['pip_resource_desc'][k] for k in ('width', 'height'))
                or not 64 <= value['width'] <= 1280 or not 64 <= value['height'] <= 720
                or any(type(value[k]) is not int or value[k] != 0 for k in ('record_recycle', 'record_deferred_release', 'pool_release_marked'))
                or value['owner_lock_held'] is not True or value['writer_boundary_lease_verified'] is not False
                or value['gpu_retirement_verified'] is not False):
            return ['invalid Present-time pool ownership observation or unsupported lifetime claim']
    except (KeyError, TypeError):
        return ['missing or malformed native pool observation']
    return []


def queue_errors(capture):
    try:
        q = capture['queue_observation']
        if (any(q[k] is not True for k in ('device_known', 'early_device_return', 'primary_known', 'all_observed_methods_match'))
                or any(type(q[k]) is not int or q[k] <= 0 for k in ('device_canonical', 'primary_canonical', 'primary_execute', 'primary_serial', 'method_count'))
                or type(q['device9']) is not bool or q['primary_type'] != 0 or q['creation_hook_mask'] != (3 if q['device9'] else 1)
                or any(type(q[k]) is not int or q[k] != 0 for k in ('failed', 'observer_errors', 'close_observer_errors', 'unknown_foreign', 'foreign_target_writes', 'untracked_target_writes', 'foreign_fence_failures'))):
            return ['invalid actual queue/observer coverage']
        keys = ('foreign_unknown_lists', 'foreign_precreation_copies', 'foreign_fenced_lists', 'foreign_fenced_submissions')
        if any(type(q[k]) is not int or q[k] < 0 for k in keys):
            return ['invalid foreign completion accounting']
        if q['foreign_precreation_copies']:
            return ['full pre-creation exclusion receipts not exported for this strict queue audit']
        receipts = q['foreign_completions']
        if (not isinstance(receipts, list) or len(receipts) > 128 or len(receipts) != q['foreign_fenced_submissions']
                or q['foreign_unknown_lists'] != q['foreign_precreation_copies'] + q['foreign_fenced_lists']):
            return ['unresolved unknown foreign command lists']
        previous = 0
        total = 0
        for r in receipts:
            if (any(type(r[k]) is not int or r[k] <= 0 for k in ('submission', 'lists', 'signaled', 'completed', 'queue_canonical', 'device'))
                    or r['completed'] == 2**64-1 or r['completed'] < r['signaled'] or r['device'] != q['device_canonical']
                    or r['queue_canonical'] == q['primary_canonical'] or not previous < r['submission'] < capture['gpu_observed_batch']['submission_sequence']
                    or not isinstance(r['wait_ms'], (int, float)) or not math.isfinite(r['wait_ms']) or r['wait_ms'] < 0):
                return ['invalid actual foreign completion receipt']
            total += r['lists']
            previous = r['submission']
        if total != q['foreign_fenced_lists']:
            return ['foreign fence receipts do not cover unknown list count']
    except (KeyError, TypeError, ValueError):
        return ['missing or malformed actual queue completion evidence']
    return []


def analyze(root, require_origin=False, require_boundary=False, require_resource_desc=False, require_barrier_coverage=False, require_native_pool=False, colour_sweep=False, require_queue=False):
    require_native_pool = require_native_pool or require_queue
    require_barrier_coverage = require_barrier_coverage or require_native_pool
    require_resource_desc = require_resource_desc or require_barrier_coverage
    require_boundary = require_boundary or require_resource_desc
    require_origin = require_origin or require_boundary
    phases = []
    for phase in range(4):
        try:
            capture = json.loads((root / f'phase_{phase}' / 'capture.json').read_text())
        except (OSError, ValueError):
            phases.append({'phase': phase, 'errors': ['missing or unreadable capture evidence'], 'gpu': {},
                           'readback_boundary': None, 'native_origins_located': False, 'observed_raster': False})
            continue
        gpu = capture['gpu_observed_batch']
        errors = writer_errors(gpu) + tone.lens.geometry_errors(capture, 0, 0)
        if require_origin:
            errors += origin_errors(capture)
            if (capture['gpu_observed_batch'].get('observed_writer_origin') or {}).get('identity_model') != 'native_copy_slot_v1' and not capture.get('pose_tags_enabled'):
                errors.append('source-origin control requires desktop camera tags')
        if require_boundary:
            errors += boundary_errors(capture)
        if require_resource_desc:
            errors += resource_errors(capture)
        if require_barrier_coverage:
            errors += barrier_errors(capture)
        if require_native_pool:
            errors += pool_errors(capture)
        if require_queue:
            errors += queue_errors(capture)
        if capture['lens_phase'] != phase or capture['scene_tonemap'] is not bool(colour_sweep and phase % 2):
            errors.append('unexpected camera phase or colour intervention')
        phases.append({'phase': phase, 'errors': errors, 'gpu': gpu, 'queue_observation': capture.get('queue_observation'), 'readback_boundary': capture.get('readback_boundary'), 'native_pool': capture.get('pip_native_pool'),
                       'native_origins_located': gpu.get('last_write_rva', 0) > 0 and gpu.get('execute_rva', 0) > 0,
                       'observed_raster': sum(gpu.get('writer_counts', {}).get(k, 0) for k in ('draw', 'indexed')) > 0})
    metadata = not any(p['errors'] for p in phases)
    origins = [p['gpu'].get('observed_writer_origin') for p in phases]
    advancing = (all(isinstance(o, dict) and type(o.get('source_render_id')) is int
                     and o['source_render_id'] > 0 for o in origins)
                 and all(a['source_render_id'] < b['source_render_id']
                         for a, b in zip(origins, origins[1:])))
    origin_pass = (require_origin and metadata and advancing
                   and all(p['observed_raster'] and p['native_origins_located'] for p in phases))
    boundary_pass = (require_boundary and origin_pass and all(
        a['gpu']['serial'] < b['gpu']['serial']
        and a['readback_boundary']['copy_submission'] < b['readback_boundary']['writer_submission']
        and a['gpu']['queue'] == b['gpu']['queue'] and a['gpu']['resource'] == b['gpu']['resource']
        for a, b in zip(phases, phases[1:])))
    barrier_stable = (require_barrier_coverage and boundary_pass and all(
        p['readback_boundary']['barrier_observation'] == phases[0]['readback_boundary']['barrier_observation']
        for p in phases))
    if barrier_stable:
        access = [p['readback_boundary']['access_exclusions'] for p in phases]
        barrier_stable = (all(a['allocation'] == access[0]['allocation'] for a in access)
                          and all(all(x <= y for x, y in zip([a['aliases'], a['uavs'], a['indirect_compute'], a['indirect_raster'], *a['writers'], *a['extended']],
                                                           [b['aliases'], b['uavs'], b['indirect_compute'], b['indirect_raster'], *b['writers'], *b['extended']]))
                                  for a, b in zip(access, access[1:])))
    pool_stable = (require_native_pool and barrier_stable and all(p['native_pool'] == phases[0]['native_pool'] for p in phases))
    queue_stable = require_queue and bool(pool_stable)
    if queue_stable:
        queues = [p['queue_observation'] for p in phases]
        queue_stable = all(
            all(a[k] == b[k] for k in ('device_canonical', 'primary_canonical', 'primary_execute', 'primary_serial', 'creation_hook_mask', 'device9'))
            and b['foreign_completions'][:len(a['foreign_completions'])] == a['foreign_completions']
            and a['foreign_unknown_lists'] <= b['foreign_unknown_lists']
            for a, b in zip(queues, queues[1:]))
    return {'metadata_pass': metadata, 'colour_sweep_control': colour_sweep,
            'observed_raster_gate_pass': metadata and all(p['observed_raster'] for p in phases),
            'observed_writer_origin_gate_pass': origin_pass,
            'readback_queue_boundary_gate_pass': boundary_pass,
            'resource_descriptor_audit_pass': require_resource_desc and boundary_pass,
            'barrier_observation_audit_pass': bool(barrier_stable),
            'native_pool_observation_audit_pass': bool(pool_stable),
            'foreign_queue_completion_audit_pass': bool(queue_stable),
            'writer_boundary_lease_verified': False, 'gpu_retirement_verified': False,
            'phases': phases, 'final_writer_verified': False, 'simulation_frame_verified': False,
            'readback_source_version_verified': False,
            'runtime_submission': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--require-origin', action='store_true')
    parser.add_argument('--require-boundary', action='store_true')
    parser.add_argument('--require-resource-desc', action='store_true', help='Also audit actual descriptor against state/decay inference')
    parser.add_argument('--require-barrier-coverage', action='store_true', help='Also require known capabilities, enhanced/base-writer hooks and zero unsupported hazards; not complete final-writer proof')
    parser.add_argument('--require-native-pool', action='store_true', help='Also require stable Present-time native slot ownership observations; not GPU retirement proof')
    parser.add_argument('--colour-sweep', action='store_true', help='Require native/scene/native/scene intervention pattern; colour acceptance remains a separate audit')
    parser.add_argument('--require-queue', action='store_true', help='Also require actual early queue coverage and completed foreign COPY receipts for every unknown list')
    args = parser.parse_args()
    summary = analyze(args.directory, args.require_origin, args.require_boundary, args.require_resource_desc, args.require_barrier_coverage, args.require_native_pool, args.colour_sweep, args.require_queue)
    (args.directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    gate = ('foreign_queue_completion_audit_pass' if args.require_queue else 'native_pool_observation_audit_pass' if args.require_native_pool else 'barrier_observation_audit_pass' if args.require_barrier_coverage else 'resource_descriptor_audit_pass' if args.require_resource_desc else 'readback_queue_boundary_gate_pass' if args.require_boundary else
            'observed_writer_origin_gate_pass' if args.require_origin else 'observed_raster_gate_pass')
    raise SystemExit(0 if summary[gate] else 1)
