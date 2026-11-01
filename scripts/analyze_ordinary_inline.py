"""Independent ordinary compositor retention audit; does not certify stereo."""
import argparse
import json
from pathlib import Path
import runpy

boundary_audit = runpy.run_path(str(Path(__file__).with_name('analyze_ordinary_boundaries.py')))['analyze']


def analyze(capture, pixels):
    errors = []
    try:
        if (capture['model'] != 'ordinary_inline_copy_v1' or capture['pip_enabled'] is not False
                or capture['gpu_pair_verified'] is not False or capture['runtime_submission'] is not False):
            raise ValueError('wrong ordinary inline-copy model')
        cpu, copy = capture['cpu_boundary'], capture['copy']
        result = boundary_audit(cpu)
        if not result['ordinary_cpu_boundary_pass']:
            raise ValueError('ordinary CPU boundary refused: ' + str(result['errors']))
        if copy.get('refusal') != 0 or copy.get('hazards') != 0:
            raise ValueError(f"native copy refused: reason={copy.get('refusal')} hazards={copy.get('hazards')}")
        for name in ('tag', 'generation', 'writes', 'submission', 'fence_value', 'source', 'identity', 'commands', 'queue', 'thread'):
            if type(copy[name]) is not int or not 0 < copy[name] < (1 << 64):
                raise ValueError('missing actual recording/queue/fence field: ' + name)
        if copy['tag'] != cpu['dispatch']:
            raise ValueError('copy belongs to a different producing dispatch')
        for name in ('appended', 'submitted', 'completed'):
            if copy[name] is not True:
                raise ValueError('copy is not recorded, submitted and GPU completed')
        for name in ('refusal', 'hazards'):
            if type(copy[name]) is not int or copy[name] != 0:
                raise ValueError('refused or hazardous copy')
        if (type(copy['source_state']) is not int or copy['source_state'] not in (0, 4)
                or type(copy['source_flags']) is not int or not copy['source_flags'] & 1 or copy['source_flags'] & (4 | 32)
                or type(copy['format']) is not int or copy['format'] not in (28, 29, 87, 91)):
            raise ValueError('unproven source state or incompatible backbuffer description')
        for name in ('excluded_aliases', 'excluded_uavs'):
            if type(copy[name]) is not int or copy[name] < 0:
                raise ValueError('malformed exclusion counter')
        for name in ('dimension', 'array', 'mips', 'samples', 'quality', 'layout', 'alignment', 'heap_flags', 'heap_type', 'heap_cpu', 'heap_pool', 'creation_node', 'visible_node'):
            if type(copy[name]) is not int or not 0 <= copy[name] < (1 << 64):
                raise ValueError('malformed source description/backing: ' + name)
        if ((copy['dimension'], copy['array'], copy['mips'], copy['samples'], copy['quality'], copy['layout']) != (3, 1, 1, 1, 0, 0)
                or copy['alignment'] not in (0, 65536)):
            raise ValueError('unsupported source/subresources')
        private = (copy['heap_known'] is True and type(copy['heap_flags']) is int and copy['heap_flags'] & 8
                   and not copy['heap_flags'] & ~0x8c and copy['heap_type'] == 1 and copy['heap_cpu'] == 0
                   and copy['heap_pool'] == 0 and copy['creation_node'] == 1 and copy['visible_node'] == 1
                   and copy['source_flags'] == 1)
        if copy['private_display'] is not bool(private) or ((copy['excluded_aliases'] or copy['excluded_uavs']) and not private):
            raise ValueError('alias/UAV exclusion lacks actual private display backing')
        if (type(copy['width']) is not int or type(copy['height']) is not int
                or copy['width'] != cpu['display_width'] or copy['height'] != cpu['display_height']):
            raise ValueError('retained source is not full display resolution')
        if (type(capture['readback_result']) is not int or capture['readback_result'] != 0
                or capture['pixels_saved'] is not True or pixels['width'] != copy['width']
                or pixels['height'] != copy['height'] or pixels['bytes'] != copy['width'] * copy['height'] * 3):
            raise ValueError('actual retained image is missing or malformed')
    except (KeyError, TypeError, ValueError, IndexError) as error:
        errors.append(str(error))
    return dict(ordinary_preoverwrite_copy_pass=not errors, errors=errors, gpu_pair_verified=False,
                simulation_snapshot_verified=False, all_final_writers_verified=False,
                colour_parity_verified=False, runtime_submission=False)


def ppm(path):
    with path.open('rb') as stream:
        if stream.readline() != b'P6\n':
            raise ValueError('expected exact native RGB PPM')
        width, height = map(int, stream.readline().split())
        if stream.readline() != b'255\n':
            raise ValueError('unexpected pixel encoding')
        return dict(width=width, height=height, bytes=len(stream.read()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    controls = []
    for index in range(4):
        try:
            capture = json.loads((args.directory/f'inline_{index}.json').read_text())
            try:
                pixels = ppm(args.directory/f'inline_{index}.ppm')
            except (OSError, ValueError):
                pixels = None
            result = analyze(capture, pixels)
        except (OSError, ValueError) as error:
            result = dict(ordinary_preoverwrite_copy_pass=False, errors=[str(error)])
        controls.append(result)
    passed = all(result['ordinary_preoverwrite_copy_pass'] for result in controls)
    report = dict(all_controls_pass=passed, controls=controls, gpu_pair_verified=False, runtime_submission=False)
    (args.directory/'inline_audit.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
