"""Audit a bounded ordinary output mirror from exact GPU receipts and pixels."""
import argparse
import json
from pathlib import Path
import runpy

private = runpy.run_path(str(Path(__file__).with_name('analyze_private_compositor.py')))


def receipt(copy, redirect, mirror):
    errors = private['receipt'](copy, redirect)
    try:
        for name in ('tag', 'producer_fence', 'fence_value', 'image', 'destination', 'queue'):
            if type(mirror[name]) is not int or mirror[name] <= 0:
                raise ValueError('invalid mirror identity/fence: ' + name)
        if (mirror['tag'] != copy['tag'] or mirror['producer_fence'] != copy['fence_value']
                or mirror['queue'] != copy['queue'] or mirror['image'] != copy['image_identity']
                or mirror['destination'] != redirect['source'] or mirror['image'] == mirror['destination']):
            raise ValueError('mirror does not join the exact producer image and native destination')
        if (mirror['submitted'] is not True or mirror['completed'] is not True
                or type(mirror['result']) is not int or mirror['result'] != 0
                or copy['submitted'] is not True or copy['completed'] is not True):
            raise ValueError('actual producer and mirror GPU completion required')
    except (KeyError, TypeError, ValueError) as error:
        errors.append(str(error))
    return errors


def analyze(capture, pixels):
    errors = []
    try:
        if capture['model'] != 'ordinary_private_compositor_mirror_v1' or capture['desktop_mirror_verified'] is not True:
            raise ValueError('wrong mirror control or incomplete mirror')
        normalized = dict(capture, model='ordinary_private_compositor_v1', desktop_mirror_verified=False)
        inherited = private['analyze'](normalized, pixels)
        errors.extend(inherited['errors'])
        errors.extend(receipt(capture['copy'], capture['redirect'], capture['mirror']))
    except (KeyError, TypeError, ValueError) as error:
        errors.append(str(error))
    return dict(ordinary_present_mirror_pass=not errors, errors=errors, gpu_pair_verified=False,
                colour_parity_verified=False, histories_verified=False, hud_parity_verified=False,
                general_retirement_verified=False, runtime_submission=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    args=parser.parse_args()
    controls=[]
    for i in range(4):
        try:
            capture=json.loads((args.directory/f'inline_{i}.json').read_text())
            result=analyze(capture,private['base']['ppm'](args.directory/f'inline_{i}.ppm'))
        except (OSError,ValueError) as error:
            result=dict(ordinary_present_mirror_pass=False,errors=[str(error)])
        controls.append(result)
    passed=all(c['ordinary_present_mirror_pass'] for c in controls)
    report=dict(all_controls_pass=passed,controls=controls,gpu_pair_verified=False,runtime_submission=False)
    (args.directory/'present_mirror_audit.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return 0 if passed else 1


if __name__=='__main__':
    raise SystemExit(main())
