"""Audit actual colour boundaries for both ordinary views; no GPU retention claim."""
import argparse
import importlib.util
import json
from pathlib import Path

_spec = importlib.util.spec_from_file_location('pair_colour_bloom', Path(__file__).with_name('analyze_ordinary_owned_bloom.py'))
bloom_audit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bloom_audit)
uint = bloom_audit.uint


def check(value, message):
    if not value:
        raise ValueError(message)


def analyze(camera, sdk, temporal, creation, bloom, colour, first=None, previous=None):
    errors, inventory = [], []
    shared = False
    try:
        check(bloom_audit.analyze(camera, sdk, temporal, creation, bloom,
              first=first[:5] if first else None, previous=previous[:5] if previous else None)['owned_bloom_use_pass'],
              'accepted actual camera/SDK/native/owned-bloom control required')
        check(colour['model'] == 'ordinary_pair_colour_boundaries_v1' and all(colour[k] is True for k in ('cpu_only', 'armed', 'installed', 'instruction_guards')),
              'explicit pair scope and guarded native hooks required')
        check(all(colour[k] is False for k in ('pip_enabled', 'resource_leases', 'final_writer_verified', 'gpu_completion_verified', 'gpu_pair_verified', 'runtime_submission')),
              'colour observation cannot claim PIP, source ownership, final writer or GPU/runtime completion')
        check(all(type(colour[k]) is int and colour[k] == 0 for k in ('refused', 'errors', 'dropped', 'raw_errors')), 'pair observer refused or lost evidence')
        # Older read-only captures predate the optional consumer/unwind tap.
        # Whenever exported, its failures cannot be ignored by this source audit.
        check(all(type(colour[k]) is int and colour[k] == 0 for k in ('aborted', 'consumer_errors') if k in colour),
              'native compositor aborted or synchronous consumer failed')
        check(all(type(colour[k]) is int and 0 < colour[k] <= 32768 for k in ('display_width', 'display_height')), 'actual swapchain dimensions required')
        events = colour['events']
        check(len(events) == 6 and [(e['slot'], e['stage']) for e in events] == [(slot, stage) for slot in (0, 1) for stage in (0, 1, 2)],
              'exact post/compositor-before/compositor-after for left then right required')
        pair = camera['ordinary_pair']
        evidence = bloom['record']['scope_evidence']
        resources = [[], []]
        prior_serial = None
        for event in events:
            slot, stage = event['slot'], event['stage']
            scene = camera['scenes'][slot]
            expected = dict(dispatch=camera['dispatch'], ordinal=slot+1, slot=slot, entry=scene['entry'], list=camera['list'], renderer=camera['renderer'],
                            render_id=pair['render_id'], revision=pair['revision'], token=pair['token'], producer_list=pair['producer_list'],
                            producer_revision=pair['producer_revision'], left_entry=pair['left_entry'], right_entry=pair['right_entry'],
                            thread=temporal['events'][slot]['thread'], refusal=0)
            check(all(type(event[k]) is int and event[k] == v for k, v in expected.items()), 'colour output does not join actual producing eye/job/thread')
            check(uint(event['serial']) and event['serial'] and (prior_serial is None or event['serial'] == prior_serial+1), 'native stage serials missing, duplicated or reordered')
            prior_serial = event['serial']
            check(event['before'] == event['after'], 'native scene inventory changed during output observation')
            scope = event['before']
            check(scope['model'] == 'ordinary_scene_view_inventory_v1' and type(scope['mask']) is int and scope['mask'] == 0 and
                  type(scope['reads']) is int and scope['reads'] == 15 and type(scope['counter']) is int and scope['counter'] == scene['counter_before'] and
                  type(scope['active']) is int and scope['active'] == scene['entry'] and type(scope['count']) is int and 2 <= scope['count'] <= 64,
                  'incomplete actual native scene scope')
            flags = scope['view_flags']
            check(scope['count'] == len(flags) == evidence['count'] and flags == evidence['view_flags'] and all(uint(v) for v in flags) and
                  flags[:2] == [camera['views'][0]['flags'], camera['views'][1]['flags']] and scope['flags'] == flags[slot] and
                  not any(v & 0x50000044 for v in flags[:2]) and all(v & 0x50000044 and not v & 0x40 for v in flags[2:]),
                  'scope must inventory exactly two ordinary slots and all auxiliary non-PIP slots')
            check(type(event['entry_argument']) is int and type(event['renderer_argument']) is int and
                  (event['entry_argument'], event['renderer_argument']) == ((scene['entry'], 0) if stage == 0 else (0, camera['renderer']+0x10)),
                  'actual post/compositor argument differs from eye/renderer scope')
            check(all(uint(event[k]) and event[k] for k in ('wrapper', 'resource', 'epoch', 'serial_at_capture', 'scene_begin_epoch', 'scene_begin_serial')) and
                  type(event['status']) is int and event['status'] == 5 and event['epoch'] == event['scene_begin_epoch'] and event['serial_at_capture'] >= event['scene_begin_serial'],
                  'unobserved resource or stale descriptor-cache epoch')
            rtvs = event['rtvs']
            check(1 <= len(rtvs) <= 8, 'bounded real RTV creation/binding observations required')
            identities, commands, resource_flags = set(), set(), set()
            fresh = False
            for view in rtvs:
                check(view['known'] is True and view['resource'] == event['resource'] and
                      all(uint(view[k]) and view[k] for k in ('resource', 'identity', 'handle', 'created')) and
                      all(type(view[k]) is int and 0 <= view[k] < 2**32 for k in ('height', 'storage', 'format', 'resource_dimension', 'view_dimension', 'flags', 'arrays', 'mips', 'samples', 'planes', 'subresources')),
                      'RTV creation identity/shape missing')
                # Native full-colour contract observed before the second camera:
                # typeless RGBA8 with UNORM/SRGB views. Native post processing
                # also selects RT-only storage when the late effect is disabled.
                check(type(view['width']) is int and (view['width'], view['height']) == (colour['display_width'], colour['display_height']) and
                      view['storage'] == 27 and view['format'] in (28, 29) and view['resource_dimension'] == 3 and view['view_dimension'] == 4 and view['flags'] in (1, 5) and
                      all(view[k] == 1 for k in ('arrays', 'mips', 'samples', 'planes', 'subresources')), 'actual native full-colour format/dimensions/subresources differ')
                check(uint(view['bound']) and uint(view['command_list']) and
                      ((view['bound']==0 and view['command_list']==0) or
                       (view['created'] <= view['bound'] <= event['serial_at_capture'] and view['command_list']>0)),
                      'RTV binding serial or actual command identity missing')
                if view['bound'] > event['scene_begin_serial']:
                    check(view['command_list'], 'fresh binding requires actual command list')
                    fresh = True
                    commands.add(view['command_list'])
                identities.add(view['identity'])
                resource_flags.add(view['flags'])
            check(len(identities) == 1 and len(resource_flags) == 1 and fresh and len(commands) == 1, 'each eye boundary requires one stable resource identity/description and actual fresh recording binding')
            identity = next(iter(identities))
            resources[slot].append((event['wrapper'], event['resource'], identity))
            inventory.append(dict(slot=slot, stage=stage, wrapper=event['wrapper'], resource=event['resource'], identity=identity,
                                  commands=next(iter(commands)), counter=scope['counter'], native_count=scope['count']))
        check(all(len(set(eye)) == 1 for eye in resources), 'native eye source changed across post/compositor boundaries')
        shared = resources[0][0][1:] == resources[1][0][1:]
    except (KeyError, TypeError, ValueError, IndexError, StopIteration) as error:
        errors.append(str(error))
    return dict(pair_colour_observation_pass=not errors, errors=errors, inventory=inventory, shared_source_observed=shared,
                pre_overwrite_retention_verified=False, resource_leases=False, final_writer_verified=False, gpu_completion_verified=False,
                native_gpu_histories_verified=False, gpu_pair_verified=False, runtime_submission=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    controls, first, previous = [], None, None
    for index in range(4):
        try:
            value = tuple(json.loads((args.directory / f'{name}_{index}.json').read_text()) for name in ('second', 'upscaler', 'temporal', 'creation', 'bloom', 'pair_colour'))
            controls.append(analyze(*value, first=first, previous=previous))
            first = first or value
            previous = value
        except (OSError, ValueError) as error:
            controls.append(dict(pair_colour_observation_pass=False, errors=[str(error)]))
    result = dict(all_controls_pass=all(c['pair_colour_observation_pass'] for c in controls), controls=controls,
                  pre_overwrite_retention_verified=False, resource_leases=False, final_writer_verified=False, gpu_completion_verified=False,
                  native_gpu_histories_verified=False, gpu_pair_verified=False, runtime_submission=False)
    (args.directory / 'pair_colour_boundaries_audit.json').write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result['all_controls_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
