"""Independently audit WARP pair retention receipts and pixels, never game readiness."""
import argparse
import json
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def uint(value, positive=False):
    return type(value) is int and (0 < value if positive else 0 <= value) and value < 2**64


def analyze(value):
    errors, inventory = [], []
    try:
        require(value['model'] == 'ordinary_pair_gpu_retention_fixture_v1' and
                value['actual_game_run'] is False and value['synthetic_native_camera'] is True,
                'fixture must explicitly declare synthetic native scopes and no actual game run')
        require(all(value[k] is True for k in ('actual_gpu', 'blocked_completion_tested', 'independent_rgba_pixels')),
                'actual GPU, blocked fences and independent pixel readback required')
        pairs = value['pairs']
        require(len(pairs) == 2, 'exactly two distinct producing jobs required')
        outputs, images, tags, jobs, sdk_ordinals = set(), set(), set(), [], []
        for pair in pairs:
            require(pair['model'] == 'ordinary_pair_gpu_retention_v1' and
                    pair['refusal'] == 0 and type(pair['refusal']) is int and
                    pair['cpu_verified'] is True and pair['gpu_retention_verified'] is True,
                    'accepted CPU receipt and actual completed GPU retention required')
            require(all(pair[k] is False for k in ('native_gpu_histories_verified', 'gpu_pair_verified', 'runtime_submission')) and
                    pair['retirement'] == 'process_lifetime', 'component fixture cannot promote histories, runtime or safe continuous retirement')
            require(all(uint(pair[k], True) for k in ('dispatch', 'render_id', 'revision', 'list', 'renderer', 'producer_list', 'producer_revision')) and
                    uint(pair['token']) and pair['dispatch'] <= (2**64-1)//2, 'bounded producing-job provenance required')
            require(all(uint(pair['sdk'][k], True) for k in ('epoch', 'ordinal')) and uint(pair['sdk']['token']), 'accepted SDK job required')
            jobs.append(pair['dispatch']); sdk_ordinals.append((pair['sdk']['epoch'], pair['sdk']['ordinal']))
            eyes = pair['eyes']
            require(len(eyes) == 2, 'exactly left and right retained images required')
            for slot, eye in enumerate(eyes):
                require(type(eye['slot']) is int and eye['slot'] == slot and uint(eye['tag'], True) and
                        eye['tag'] == pair['dispatch']*2+slot and eye['tag'] not in tags,
                        'eye tag must join exact producing job and slot')
                tags.add(eye['tag'])
                require(eye['begun'] is True and eye['ended'] is True and
                        uint(eye['begin_serial'], True) and eye['end_serial'] == eye['begin_serial']+1 and
                        type(eye['end_serial']) is int and uint(eye['buffer_index']) and
                        all(uint(eye[k], True) for k in ('thread', 'swapchain', 'native_buffer', 'native_buffer_identity', 'private_output', 'fence')),
                        'actual bounded native scope, GetBuffer identity and completion fence required')
                require(eye['private_output'] != eye['native_buffer_identity'] and eye['private_output'] not in outputs,
                        'private output cannot alias native output or any prior eye/job output')
                outputs.add(eye['private_output'])
                copy, redirect = eye['copy'], eye['redirect']
                require(all(copy[k] is True for k in ('appended', 'submitted', 'completed', 'heap_known', 'private_display')) and
                        all(type(copy[k]) is int and copy[k] == 0 for k in ('refusal', 'hazards')) and
                        all(uint(copy[k], True) for k in ('generation', 'writes', 'submission', 'fence_value', 'source', 'commands', 'queue', 'image_identity')),
                        'actual writer/recording/submission/fence and private resource metadata required')
                require(copy['tag'] == eye['tag'] and type(copy['tag']) is int and copy['identity'] == eye['private_output'] and
                        copy['thread'] == eye['thread'] and copy['image_identity'] not in images and
                        copy['image_identity'] not in outputs and copy['image_identity'] != eye['native_buffer_identity'],
                        'copy must retain its exact private producer with a distinct completed image')
                images.add(copy['image_identity'])
                expected = dict(width=8, height=4, format=28, source_flags=1, source_state=4, dimension=3,
                                array=1, mips=1, samples=1, quality=0, layout=0, heap_type=1,
                                heap_flags=0x8c, creation_node=1, visible_node=1)
                require(all(type(copy[k]) is int and copy[k] == v for k, v in expected.items()), 'actual WARP texture shape/private heap/state differs')
                require(all(redirect[k] is True for k in ('restored', 'restore_required', 'private_backing', 'source_heap_known')) and
                        redirect['active'] is False and type(redirect['refusal']) is int and redirect['refusal'] == 0 and
                        uint(redirect['bindings'], True) and redirect['restores'] == 1 and type(redirect['restores']) is int,
                        'real redirection must restore exactly one current native binding')
                for key, expected_value in dict(tag=eye['tag'], source=eye['native_buffer_identity'], target=eye['private_output'],
                        commands=copy['commands'], generation=copy['generation'], width=8, height=4, format=28, source_flags=1, source_heap_flags=0x8d).items():
                    require(type(redirect[key]) is int and redirect[key] == expected_value, 'redirect/copy/native-source identity or generation differs')
                access = copy['access']
                require(all(type(access[k]) is int and access[k] == 0 for k in ('aliases', 'alias_null', 'alias_target', 'alias_other', 'first_extended')) and
                        access['extended_calls'] == [0]*8 and access['extended_blocked'] == [0]*8,
                        'this fixture expects no unaccounted alias/enhanced writer events')
                inventory.append(dict(dispatch=pair['dispatch'], slot=slot, tag=eye['tag'], image=copy['image_identity'],
                                      source=copy['identity'], submission=copy['submission'], fence=eye['fence'], fence_value=copy['fence_value']))
            require(all(eyes[0][k] == eyes[1][k] for k in ('buffer_index', 'swapchain', 'native_buffer', 'native_buffer_identity', 'thread')) and
                    eyes[0]['end_serial'] < eyes[1]['begin_serial'], 'both eyes must join one native output/job in order')
            a, b = (e['copy'] for e in eyes)
            require(a['queue'] == b['queue'] and a['submission'] <= b['submission'] and type(pair['same_submission']) is bool and
                    pair['same_submission'] == (a['submission'] == b['submission']), 'both eye completions must join the ordered native queue')
        require(jobs[0] < jobs[1] and sdk_ordinals[0] < sdk_ordinals[1] and outputs.isdisjoint(images), 'jobs and all four private outputs/images must remain distinct')
        if 'native_colour_detours' in value:
            require(type(value['native_colour_detours']) is bool, 'native hook claim must be explicit boolean')
            if not value['native_colour_detours']:
                require(value['native_calls'] == 0 and value['colour_scopes'] == [], 'direct fixture cannot relabel actual native hook receipts')
        if value.get('native_colour_detours', False):
            require(value['native_colour_detours'] is True and type(value['native_calls']) is int and value['native_calls'] == 4 and
                    len(value['colour_scopes']) == 2, 'actual detour fixture needs four native calls and two explicit scope inventories')
            for pair, scope in zip(pairs, value['colour_scopes']):
                require(scope['armed'] is True and scope['installed'] is True and scope['instruction_guards'] is False and
                        all(type(scope[k]) is int and scope[k] == 0 for k in ('refused', 'errors', 'dropped', 'raw_errors', 'aborted', 'consumer_errors')),
                        'fixture native hooks must not impersonate guarded game code or lose consumer evidence')
                require([(e['slot'], e['stage']) for e in scope['events']] == [(slot, stage) for slot in (0,1) for stage in (0,1,2)],
                        'actual detours must deliver all six native boundaries in eye order')
                for eye in pair['eyes']:
                    for stage, key in ((1, 'begin_serial'), (2, 'end_serial')):
                        event = scope['events'][eye['slot']*3+stage]
                        require(event['serial'] == eye[key] and event['dispatch'] == pair['dispatch'] and event['thread'] == eye['thread'] and
                                event['list'] == pair['list'] and event['renderer'] == pair['renderer'] and event['refusal'] == 0 and
                                event['before'] == event['after'] and event['before']['mask'] == 0,
                                'retention eye must join its actual authenticated native entry/exit callback')
        require(len(value['readbacks']) == 4, 'four actual pixel readbacks required')
        for image, colour in zip(value['readbacks']+[value['native_readback']], [(255,0,0), (0,0,255), (0,255,0), (255,255,255), (255,0,255)]):
            require(type(image['width']) is int and type(image['height']) is int and (image['width'], image['height']) == (8,4) and
                    len(image['rgb']) == 8*4*3 and all(type(v) is int and 0 <= v <= 255 for v in image['rgb']) and
                    image['rgb'] == list(colour)*32, 'actual retained pixels or restored native magenta target differ')
    except (KeyError, TypeError, ValueError, IndexError) as error:
        errors.append(str(error))
    return dict(fixture_retention_pass=not errors, errors=errors, inventory=inventory, actual_game_run=False,
                actual_game_retention_verified=False, native_gpu_histories_verified=False, gpu_pair_verified=False,
                runtime_submission=False, headset_ready=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        result = analyze(json.loads(args.fixture.read_text(encoding='utf-8-sig')))
    except (OSError, ValueError) as error:
        result = dict(fixture_retention_pass=False, errors=[str(error)], headset_ready=False)
    report = json.dumps(result, indent=2, allow_nan=False)+'\n'
    if args.output:
        args.output.write_text(report, encoding='utf-8')
    print(report, end='')
    return 0 if result['fixture_retention_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
