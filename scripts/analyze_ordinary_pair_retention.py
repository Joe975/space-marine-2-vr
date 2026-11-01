"""Audit actual ordinary-pair retention, independently joining four source jobs."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

_spec = importlib.util.spec_from_file_location('retention_pair_colour', Path(__file__).with_name('analyze_ordinary_pair_colour.py'))
colour_audit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(colour_audit)


def check(value, message):
    if not value:
        raise ValueError(message)


def uint(value, positive=False):
    return type(value) is int and (0 < value if positive else 0 <= value) and value < 2**64


def equal_fields(value, expected, message):
    check(all(type(value[k]) is int and value[k] == v for k, v in expected.items()), message)


def backing(copy):
    check(all(copy[k] is True for k in ('heap_known', 'private_display')) and
          all(uint(copy[k]) for k in ('heap_flags', 'alignment', 'heap_type', 'heap_cpu', 'heap_pool', 'creation_node', 'visible_node')) and
          copy['heap_flags'] & 8 and not copy['heap_flags'] & ~0x8c and copy['alignment'] in (0, 65536),
          'actual nonshared committed display backing is required')
    equal_fields(copy, dict(heap_type=1, heap_cpu=0, heap_pool=0, creation_node=1, visible_node=1,
                           source_flags=1, dimension=3, array=1, mips=1, samples=1, quality=0, layout=0),
                 'private source description/subresource or node contract differs')
    check(type(copy['source_state']) is int and copy['source_state'] in (0, 4) and
          type(copy['format']) is int and copy['format'] in (28,29,87,91), 'unproven private source state/format')
    access = copy['access']
    for k in ('aliases', 'alias_null', 'alias_target', 'alias_other', 'alias_before', 'alias_after', 'first_extended'):
        check(uint(access[k]), 'malformed writer/alias evidence')
    check(access['alias_target'] == 0 and access['aliases'] == access['alias_null']+access['alias_other'] and
          uint(copy['excluded_aliases']) and copy['excluded_aliases'] == access['aliases'] and uint(copy['excluded_uavs']) and
          access['alias_before'] != copy['identity'] and access['alias_after'] != copy['identity'],
          'alias exclusion must join actual private nonaliasable backing, never the retained source')
    calls, blocked = access['extended_calls'], access['extended_blocked']
    check(len(calls) == len(blocked) == 8 and all(uint(v) for v in calls+blocked) and not any(blocked) and
          not any(calls[k] for k in (1,3,4,5)) and access['first_extended'] == 0,
          'blocked or opaque extended writer cannot gain retention acceptance')
    # Counts for discard/resolve/mesh/tiles may be safely excluded by the existing
    # private-target contract. This aggregate does NOT certify all final writers.


def analyze(jobs, captures, pixels, status):
    errors, controls, inventory = [], [], []
    try:
        check(len(jobs) == 4, 'four complete actual camera/SDK/native/owned-bloom/colour source jobs required')
        for n, value in enumerate(jobs):
            result = colour_audit.analyze(*value, first=jobs[0] if n else None, previous=jobs[n-1] if n else None)
            controls.append(result)
        check(all(c['pair_colour_observation_pass'] for c in controls), 'actual source/native transaction or six-stage colour control failed')
        check(status['model'] == 'ordinary_pair_gpu_status_v1' and status['configured'] is True and status['enabled'] is True and
              status['stopped'] is False and status['active'] is False, 'pair collector is unconfigured, stopped, reset or still active')
        equal_fields(status, dict(pairs=2, refusal=0), 'exact completed two-pair collector state required')
        check(status['headset_ready'] is False and len(captures) == len(pixels) == 2, 'exact two captures required without headset promotion')
        outputs, images, native, queues, swaps, fences = set(), set(), set(), set(), set(), set()
        last_dispatch, last_submission = 0, 0
        for n, (capture, pair_pixels) in enumerate(zip(captures, pixels)):
            camera, sdk, _, _, _, colour = jobs[n]
            check(all(type(colour[k]) is int and colour[k] == 0 for k in ('aborted','consumer_errors')), 'explicit native unwind/consumer counters required for actual GPU use')
            check(capture['model'] == 'ordinary_pair_gpu_capture_v1' and capture['actual_game_run'] is True and
                  all(capture[k] is False for k in ('pip_enabled','gpu_pair_verified','runtime_submission','headset_ready')),
                  'actual pair capture required; fixture/sole-main/runtime claims are invalid')
            check(type(capture['readback_result']) is int and capture['readback_result'] == 0 and capture['pixels_saved'] == [True,True] and
                  all(type(v) is bool for v in capture['pixels_saved']), 'both actual retained images must be read back and saved')
            pair, origin = capture['retention'], camera['ordinary_pair']
            check(pair['model'] == 'ordinary_pair_gpu_retention_v1' and pair['cpu_verified'] is True and pair['gpu_retention_verified'] is True and
                  all(pair[k] is False for k in ('native_gpu_histories_verified','gpu_pair_verified','runtime_submission')) and
                  pair['retirement'] == 'process_lifetime', 'retention is not independent native history/runtime or continuous retirement proof')
            equal_fields(pair, dict(dispatch=camera['dispatch'], render_id=origin['render_id'], revision=origin['revision'], token=origin['token'],
                         list=camera['list'], renderer=camera['renderer'], producer_list=origin['producer_list'], producer_revision=origin['producer_revision'], refusal=0),
                         'retained images do not join the exact actual producing dispatch/cameras')
            check(uint(pair['dispatch'], True) and last_dispatch < pair['dispatch'] <= (2**64-1)//2, 'distinct ordered bounded jobs required')
            last_dispatch = pair['dispatch']
            expected_sdk = colour_audit.bloom_audit.sdk_frame(sdk)
            equal_fields(pair['sdk'], expected_sdk, 'retained job SDK identity differs from accepted actual native/private-SDK transaction')
            eyes = pair['eyes']
            check(len(eyes) == len(pair_pixels) == 2, 'left and right eye required exactly once')
            for slot, (eye, pixel) in enumerate(zip(eyes, pair_pixels)):
                tag = pair['dispatch']*2+slot
                equal_fields(eye, dict(slot=slot, tag=tag, begin_serial=colour['events'][slot*3+1]['serial'],
                             end_serial=colour['events'][slot*3+2]['serial'], thread=colour['events'][slot*3+1]['thread']),
                             'eye tag, native entry/exit serial or thread does not join actual source scope')
                check(eye['begun'] is True and eye['ended'] is True and eye['end_serial'] == eye['begin_serial']+1 and uint(eye['buffer_index']) and
                      all(uint(eye[k], True) for k in ('swapchain','native_buffer','native_buffer_identity','private_output','fence')),
                      'actual native GetBuffer lease, eye scope and fence required')
                swaps.add(eye['swapchain']); native.add(eye['native_buffer_identity'])
                check(eye['private_output'] not in outputs, 'private output reused between eyes or producing jobs')
                outputs.add(eye['private_output'])
                copy, redirect = eye['copy'], eye['redirect']
                equal_fields(copy, dict(tag=tag, identity=eye['private_output'], thread=eye['thread'], refusal=0, hazards=0,
                                       width=colour['display_width'], height=colour['display_height']), 'copy provenance/shape/refusal differs')
                check(all(copy[k] is True for k in ('appended','submitted','completed')) and
                      all(uint(copy[k], True) for k in ('source','generation','writes','submission','fence_value','commands','queue','image_identity')) and
                      0 < copy['width'] <= 4096 and 0 < copy['height'] <= 2160 and copy['submission'] >= last_submission,
                      'actual writer/recording/ordered queue and GPU completion required')
                last_submission = copy['submission']; queues.add(copy['queue'])
                check(copy['image_identity'] not in images and eye['fence'] not in fences, 'retained image or per-copy fence reused')
                images.add(copy['image_identity']); fences.add(eye['fence']); backing(copy)
                equal_fields(redirect, dict(tag=tag, source=eye['native_buffer_identity'], target=eye['private_output'], commands=copy['commands'],
                             generation=copy['generation'], width=copy['width'], height=copy['height'], format=copy['format'], refusal=0, restores=1, source_flags=1),
                             'native restoration/source/private target/recording differs from actual retained copy')
                check(all(redirect[k] is True for k in ('restored','restore_required','private_backing','source_heap_known')) and
                      redirect['active'] is False and uint(redirect['bindings'], True) and uint(redirect['source_heap_flags']) and
                      redirect['source_heap_flags'] & 8, 'exact original native binding and actual display source required')
                check(type(pixel['width']) is int and type(pixel['height']) is int and type(pixel['bytes']) is int and
                      (pixel['width'],pixel['height'],pixel['bytes']) == (copy['width'],copy['height'],copy['width']*copy['height']*3) and
                      pixel['nonzero'] is True and type(pixel['sha256']) is str and len(pixel['sha256']) == 64 and
                      all(c in '0123456789abcdef' for c in pixel['sha256']), 'actual nonempty exact RGB PPM bytes required')
                inventory.append(dict(dispatch=pair['dispatch'], slot=slot, tag=tag, source=copy['identity'], image=copy['image_identity'],
                                      commands=copy['commands'], generation=copy['generation'], submission=copy['submission'], queue=copy['queue'],
                                      fence=eye['fence'], fence_value=copy['fence_value'], pixel_sha256=pixel['sha256']))
            check(all(eyes[0][k] == eyes[1][k] for k in ('swapchain','buffer_index','native_buffer','native_buffer_identity','thread')) and
                  eyes[0]['end_serial'] < eyes[1]['begin_serial'], 'both eyes must use the same current native output in scope order')
            check(type(pair['same_submission']) is bool and pair['same_submission'] == (eyes[0]['copy']['submission'] == eyes[1]['copy']['submission']) and
                  eyes[0]['copy']['format'] == eyes[1]['copy']['format'], 'pair queue batch or format differs')
            check(pair_pixels[0]['sha256'] != pair_pixels[1]['sha256'], 'identical eye bytes do not demonstrate independent stereo in this control')
        check(len(queues) == len(swaps) == 1 and outputs.isdisjoint(images) and outputs.isdisjoint(native) and images.isdisjoint(native),
              'configured queue/swapchain changed or native/private/copied resources alias')
    except (KeyError, TypeError, ValueError, IndexError) as error:
        errors.append(str(error))
    return dict(ordinary_pair_retention_pass=not errors, errors=errors, source_controls=controls, inventory=inventory,
                native_gpu_histories_verified=False, all_final_writers_verified=False, colour_parity_verified=False,
                geometric_disparity_verified=False, continuous_retirement_verified=False, gpu_pair_verified=False,
                runtime_submission=False, headset_ready=False)


def ppm(path):
    with path.open('rb') as stream:
        check(stream.readline() == b'P6\n', 'exact RGB PPM header required')
        width, height = map(int, stream.readline().split())
        check(0 < width <= 4096 and 0 < height <= 2160 and stream.readline() == b'255\n', 'bounded RGB PPM shape/encoding required')
        # One extra byte makes overlong files fail without an unbounded read.
        data = stream.read(width*height*3+1)
        check(len(data) == width*height*3, 'truncated or overlong RGB PPM')
        return dict(width=width, height=height, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), nonzero=any(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    try:
        root = args.directory
        jobs = [tuple(json.loads((root/f'{name}_{n}.json').read_text(encoding='utf-8-sig')) for name in
                      ('second','upscaler','temporal','creation','bloom','pair_colour')) for n in range(4)]
        check(sorted(p.name for p in root.glob('pair_retention_[0-9]*.json')) == ['pair_retention_0.json','pair_retention_1.json'],
              'exactly two bounded pair receipts required')
        captures = [json.loads((root/f'pair_retention_{n}.json').read_text(encoding='utf-8-sig')) for n in range(2)]
        pixels = [[ppm(root/f'pair_retention_{n}_{eye}.ppm') for eye in ('left','right')] for n in range(2)]
        status = json.loads((root/'pair_retention_status.json').read_text(encoding='utf-8-sig'))
        result = analyze(jobs,captures,pixels,status)
    except (OSError,ValueError) as error:
        result = analyze([],[],[],{})
        result['errors'] = [str(error)]
    report = json.dumps(result,indent=2,allow_nan=False)+'\n'
    try:
        (args.directory/'pair_retention_audit.json').write_text(report,encoding='utf-8')
    except OSError as error:
        result['ordinary_pair_retention_pass'] = False
        result['errors'].append(str(error))
        report = json.dumps(result,indent=2,allow_nan=False)+'\n'
    print(report,end='')
    return 0 if result['ordinary_pair_retention_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
