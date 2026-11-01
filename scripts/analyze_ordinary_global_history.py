"""Audit observed native target77/78 and creation-time descriptor contracts only."""
import argparse
import importlib.util
import json
from pathlib import Path

_spec = importlib.util.spec_from_file_location('owned_bloom', Path(__file__).with_name('analyze_ordinary_owned_bloom.py'))
bloom_audit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bloom_audit)
uint = bloom_audit.uint


def check(value, message):
    if not value:
        raise ValueError(message)


def analyze(camera, sdk, temporal, creation, bloom, history, first=None, previous=None):
    errors, inventory = [], []
    try:
        check(bloom_audit.analyze(camera, sdk, temporal, creation, bloom,
              first=first[:5] if first else None, previous=previous[:5] if previous else None)['owned_bloom_use_pass'],
              'accepted camera/SDK/native/owned-bloom control required')
        check(history['model'] == 'ordinary_global_history_contract_v1' and history['installed'] is True and history['read_only'] is True,
              'explicit read-only global-history observation required')
        check(all(history[k] is False for k in ('resource_leases', 'writer_verified', 'native_gpu_histories_verified', 'gpu_pair_verified', 'runtime_submission')),
              'creation observation cannot certify ownership, writer or GPU/runtime completion')
        check(all(type(history[k]) is int and history[k] == 0 for k in ('refused', 'errors', 'dropped')), 'history observation has refused or lost evidence')
        check(len(history['events']) == len(temporal['events']) == 2, 'exact two actual temporal events required')
        for event, actual, scene in zip(history['events'], temporal['events'], camera['scenes']):
            expected = dict(dispatch=camera['dispatch'], render_id=camera['ordinary_pair']['render_id'],
                            revision=camera['ordinary_pair']['revision'], token=camera['ordinary_pair']['token'],
                            thread=actual['thread'], ordinal=actual['scene_ordinal'], slot=actual['slot'], entry=actual['entry'],
                            list=camera['list'], renderer=camera['renderer'], mode=actual['mode'], refusal=0,
                            counter=actual['before']['counter'])
            check(all(type(event[k]) is int and event[k] == v for k, v in expected.items()), 'history receipt does not join actual producing eye/job/thread')
            before, after = event['before'], event['after']
            pool = before['pool']
            check(pool == after['pool'] and pool['model'] == 'native_texture_contract_v1' and pool['complete'] is True,
                  'native global-history pool incomplete or changed')
            check(pool['read_only'] is True and pool['native_calls'] is False and pool['resource_leases'] is False, 'borrowed native contract claims calls/leases')
            native = actual['texture_contract_before']
            for k in ('module_base', 'factory', 'factory_vtable', 'factory_create', 'pool', 'pool_mode', 'banks', 'bank', 'bank_count', 'bank_variant', 'bank_dimensions', 'records', 'record_count'):
                check(pool[k] == native[k], 'global-history contract differs from authenticated current native context')
            parity = event['counter'] & 1
            check(pool['selected_id'] == 0x77 + parity and len(pool['targets']) == 2 and [t['id'] for t in pool['targets']] == [0x77, 0x78], 'exact native target77/78 parity required')
            for target in pool['targets']:
                rows = target['initialization_words']
                check(target['readable'] is True and type(target['record']) is int and 0 <= target['record'] < pool['record_count'] and
                      len(target['definition']) == 5 and all(uint(v) for v in target['definition']), 'unreadable native target definition/record')
                check(type(target['no_uav']) is int and target['no_uav'] in (0, 1) and type(target['variants']) is int and target['variants'] in (0, 1) and
                      1 <= len(rows) <= 30 and len(target['wrappers']) == len(rows) * (target['no_uav'] + 1) and all(uint(v) and v for v in target['wrappers']), 'unbounded initializer/wrapper table')
                check(all(len(row) == 6 and all(type(v) is int and 0 <= v < 2**32 for v in row) for row in rows), 'six native initializer words required')
            check(len(before['inputs']) == len(after['inputs']) == 2 and len(pool['wrappers']) == 9, 'current/previous input inventory required')
            for index, (inp, end) in enumerate(zip(before['inputs'], after['inputs'])):
                label, descriptor_index = ('previous', 4) if index else ('current', 0)
                check(inp['role'] == end['role'] == label and inp['native'] == end['native'] == actual['before']['descriptor_inputs'][descriptor_index] and
                      inp['resource'] == end['resource'] == actual['outputs_before'][4 + descriptor_index]['resource'] and type(inp['native']) is int and uint(inp['native']) and
                      type(inp['readable']) is bool and inp['readable'] is end['readable'] and uint(inp['resource']),
                      'global history does not join actual descriptor wrapper/resource')
                if not inp['native']:
                    check(index == 1 and not previous and inp['resource'] == 0 and inp['readable'] is False and scene['persistent_before']['words'][6] == 0,
                          'only an actually null initial previous native bank may omit descriptor evidence')
                    for phase in (inp, end):
                        desc = phase['descriptors']
                        check(desc['model'] == 'history_descriptor_creation_v1' and desc['creation_only'] is True and desc['resource_leases'] is False and
                              desc['bound_writer_verified'] is False and desc['installed'] is True and desc['errors'] == 0 and desc['views'] == [], 'null input acquired descriptor evidence or false claims')
                    wrapper = pool['wrappers'][index]
                    check(wrapper['address'] == wrapper['vtable'] == wrapper['flags'] == 0 and wrapper['readable'] is False and wrapper['virtual_18_20_b0_c0'] == [0, 0, 0, 0],
                          'null previous input acquired a native class contract')
                    inventory.append(dict(slot=event['slot'], role=label, native=0, resource=0, initial_null=True))
                    continue
                check(inp['readable'] is True and inp['resource'], 'nonnull global input unreadable or lacks a native resource')
                wrapper = pool['wrappers'][index]
                check(wrapper['address'] == inp['native'] and wrapper['readable'] is True and uint(wrapper['flags']) and
                      uint(wrapper['vtable']) and wrapper['vtable'] and len(wrapper['virtual_18_20_b0_c0']) == 4 and
                      all(uint(v) and v for v in wrapper['virtual_18_20_b0_c0']), 'native input class/flags missing')
                if index:
                    check(inp['native'] == scene['persistent_before']['words'][6], 'previous input differs from native per-slot word6 load')
                    if previous:
                        check(inp['native'] == previous[0]['scenes'][event['slot']]['persistent_after']['words'][6], 'previous native word6 lacks same-slot continuity')
                else:
                    check(inp['native'] == scene['persistent_after']['words'][6], 'current input differs from native word6 export')
                target = pool['targets'][1-parity if index else parity]
                row_index = pool['bank_variant'] if target['variants'] else 0
                check(type(row_index) is int and 0 <= row_index < len(target['initialization_words']), 'active initializer row unbounded')
                row = target['initialization_words'][row_index]
                # Actual Scene acquire20790f0 uses third argument0, unlike bloom1.
                check(target['wrappers'][row_index * (target['no_uav'] + 1)] == inp['native'], 'actual global input differs from native parity/variant/acquisition0')
                descriptors = inp['descriptors']
                for phase in (inp, end):
                    desc = phase['descriptors']
                    check(desc['model'] == 'history_descriptor_creation_v1' and desc['creation_only'] is True and
                          desc['resource_leases'] is False and desc['bound_writer_verified'] is False and desc['installed'] is True and
                          type(desc['errors']) is int and desc['errors'] == 0 and uint(desc['serial']) and uint(desc['evictions']) and desc['views'],
                          'actual creation-time descriptor evidence missing or overclaims writer/ownership')
                check(descriptors['views'] == end['descriptors']['views'] and end['descriptors']['errors'] == 0 and end['descriptors']['installed'] is True,
                      'creation descriptors changed or failed during native pass')
                identities, devices, shapes = set(), set(), set()
                for view in descriptors['views']:
                    check(view['kind'] in ('srv', 'dsv') and view['explicit_view'] is True and view['known'] is True and view['resource'] == inp['resource'] and
                          all(uint(view[k]) and view[k] for k in ('identity', 'device', 'handle', 'serial', 'format', 'storage', 'mips', 'arrays', 'samples', 'planes', 'subresources')),
                          'actual typed SRV/DSV resource identity or shape incomplete')
                    check(all(type(view[k]) is int and 0 <= view[k] < 2**32 for k in ('height', 'storage', 'format', 'dimension', 'flags', 'mips', 'arrays', 'samples', 'planes', 'subresources', 'view_dimension', 'view_flags', 'mip_slice', 'first_array', 'array_size', 'plane_slice')) and uint(view['width']),
                          'descriptor metadata must contain actual bounded integer resource/view fields')
                    check([view['width'], view['height'], view['arrays'], view['mips']] == [row[0], row[1], row[2], row[3]] and view['dimension'] == 3 and
                          view['subresources'] == view['mips'] * view['arrays'] * view['planes'], 'actual resource description differs from selected native initializer')
                    if view['kind'] == 'dsv':
                        check(view['flags'] & 2 and view['format'] in (20, 40, 45, 55), 'DSV evidence lacks actual depth flag/typed depth format')
                    identities.add(view['identity']); devices.add(view['device'])
                    shapes.add(tuple(view[k] for k in ('width', 'height', 'storage', 'dimension', 'flags', 'mips', 'arrays', 'samples', 'planes', 'subresources')))
                check(len(identities) == len(devices) == len(shapes) == 1, 'creation records conflict on identity/device/resource shape')
                check(next(iter(devices)) == creation['device_identity'], 'history descriptor device differs from accepted creation device')
                inventory.append(dict(slot=event['slot'], role=label, native=inp['native'], resource=inp['resource'], identity=next(iter(identities)),
                                      target=target['id'], initializer=row, native_flags=wrapper['flags'], kinds=sorted({v['kind'] for v in descriptors['views']}),
                                      formats=sorted({v['format'] for v in descriptors['views']}), shape=list(next(iter(shapes)))))
    except (KeyError, TypeError, ValueError, IndexError, StopIteration) as error:
        errors.append(str(error))
    return dict(global_history_contract_pass=not errors, errors=errors, inventory=inventory, independent_right_backing=False,
                writer_verified=False, native_gpu_histories_verified=False, gpu_pair_verified=False, runtime_submission=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    controls, first, previous = [], None, None
    for index in range(4):
        try:
            observed = tuple(json.loads((args.directory / f'{name}_{index}.json').read_text()) for name in ('second', 'upscaler', 'temporal', 'creation', 'bloom', 'global_history'))
            controls.append(analyze(*observed, first=first, previous=previous))
            first = first or observed
            previous = observed
        except (OSError, ValueError) as error:
            controls.append(dict(global_history_contract_pass=False, errors=[str(error)]))
    result = dict(all_controls_pass=all(c['global_history_contract_pass'] for c in controls), controls=controls,
                  independent_right_backing=False, writer_verified=False, native_gpu_histories_verified=False, gpu_pair_verified=False, runtime_submission=False)
    (args.directory / 'global_history_contract_audit.json').write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result['all_controls_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
