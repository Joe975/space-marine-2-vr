"""Audit private ordinary compositor output, native restore and completed image."""
import argparse
import json
from pathlib import Path
import runpy

base = runpy.run_path(str(Path(__file__).with_name('analyze_ordinary_inline.py')))


def receipt(copy, redirect):
    errors = []
    try:
        for name in ('tag', 'generation', 'bindings', 'source', 'target', 'commands', 'format', 'width', 'height'):
            if type(redirect[name]) is not int or redirect[name] <= 0:
                raise ValueError('missing actual redirect field: ' + name)
        for a, b in (('tag', 'tag'), ('generation', 'generation'), ('commands', 'commands'), ('target', 'identity'),
                     ('format', 'format'), ('width', 'width'), ('height', 'height')):
            if redirect[a] != copy[b]:
                raise ValueError('redirect/copy mismatch: ' + a)
        if (redirect['source'] == redirect['target'] or redirect['active'] is not False
                or redirect['restored'] is not True or redirect['private_backing'] is not True
                or copy['private_display'] is not True or type(redirect['refusal']) is not int or redirect['refusal'] != 0):
            raise ValueError('private destination or restored native binding is unproven')
        if (type(redirect['restore_required']) is not bool or type(redirect['restores']) is not int
                or redirect['restores'] != int(redirect['restore_required'])):
            raise ValueError('missing or duplicate native binding restoration')
        if (type(redirect['source_flags']) is not int or not redirect['source_flags'] & 1
                or redirect['source_heap_known'] is not True or type(redirect['source_heap_flags']) is not int):
            raise ValueError('missing actual native source description')
    except (KeyError, TypeError, ValueError) as error:
        errors.append(str(error))
    return errors


def analyze(capture, pixels):
    errors = []
    try:
        if (capture['model'] != 'ordinary_private_compositor_v1' or capture['shader_override'] is not False
                or capture['desktop_mirror_verified'] is not False or capture['retirement'] != 'process_lifetime'):
            raise ValueError('wrong bounded private-output control model')
        normalized = dict(capture, model='ordinary_inline_copy_v1')
        inherited = base['analyze'](normalized, pixels)
        if not inherited['ordinary_preoverwrite_copy_pass']:
            errors.extend(inherited['errors'])
        errors.extend(receipt(capture['copy'], capture['redirect']))
    except (KeyError, TypeError, ValueError) as error:
        errors.append(str(error))
    return dict(ordinary_private_output_pass=not errors, errors=errors, gpu_pair_verified=False,
                colour_parity_verified=False, histories_verified=False, desktop_mirror_verified=False,
                general_retirement_verified=False, runtime_submission=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    controls = []
    for index in range(4):
        try:
            capture = json.loads((args.directory/f'inline_{index}.json').read_text())
            try:
                pixels = base['ppm'](args.directory/f'inline_{index}.ppm')
            except (OSError, ValueError):
                pixels = None
            result = analyze(capture, pixels)
        except (OSError, ValueError) as error:
            result = dict(ordinary_private_output_pass=False, errors=[str(error)])
        controls.append(result)
    passed = all(c['ordinary_private_output_pass'] for c in controls)
    report = dict(all_controls_pass=passed, controls=controls, gpu_pair_verified=False, runtime_submission=False)
    (args.directory/'private_output_audit.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
