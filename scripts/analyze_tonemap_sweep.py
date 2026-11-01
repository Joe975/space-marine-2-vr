"""Validate a repeated native/scene tonemapping comparison; never infer final frame identity."""
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


colour = module('colour', REPO / 'scripts/analyze_pip_colour.py')
lens = module('lens', REPO / 'scripts/analyze_pip_lens.py')


def valid_gpu(gpu):
    try:
        return (gpu['fence_completed'] is True and gpu['simulation_frame_verified'] is False
                and all(type(gpu[key]) is int and gpu[key] > 0 for key in
                        ('frequency', 'serial', 'resource', 'queue', 'commands', 'generation', 'writes', 'lists'))
                and type(gpu['gpu_begin']) is int and type(gpu['gpu_end']) is int
                and 0 <= gpu['gpu_begin'] <= gpu['gpu_end']
                and math.isfinite(gpu['gpu_ms']) and gpu['gpu_ms'] >= 0
                and math.isfinite(gpu['completion_upper_ms']) and gpu['completion_upper_ms'] >= 0
                and math.isclose(gpu['gpu_ms'], (gpu['gpu_end'] - gpu['gpu_begin']) * 1000 / gpu['frequency'], abs_tol=1e-6))
    except (KeyError, TypeError, ValueError):
        return False


def alternating_counts(values):
    # Counters are cumulative; phase 2 carries phase 1's applications.
    return (len(values) == 4 and values[0] == 0 and values[1] > 0
            and values[2] >= values[1] and values[3] > values[2])


def analyze(root, main_grading=False):
    reports, phases, worlds = [], [], []
    for phase in range(4):
        folder = root / f'phase_{phase}'
        capture = json.loads((folder / 'capture.json').read_text())
        report = colour.analyze(colour.read_ppm(folder / 'main.ppm'), colour.read_ppm(folder / 'pip.ppm'))
        reports.append(report)
        (folder / 'analysis.json').write_text(json.dumps(report, indent=2) + '\n')
        errors = lens.geometry_errors(capture, 0, 0)
        if capture['scene_tonemap'] != (phase % 2 == 1) or capture['lens_phase'] != phase:
            errors.append('unexpected phase or tonemap mode')
        gpu = capture['gpu_observed_batch']
        if not valid_gpu(gpu):
            errors.append('invalid completed GPU timing')
        worlds.append(next(v['world'] for v in capture['native_dispatch']['after'] if v['pip']))
        phases.append({'phase': phase, 'scene_tonemap': capture['scene_tonemap'],
                       'applications': capture['scene_tonemap_applications'],
                       'main_grading_applications': capture.get('main_grading_applications', 0),
                       'errors': errors, 'gpu': gpu,
                       'identity_mae_bytes': {c: report['channels'][c]['mae_bytes']['identity'] for c in ('red', 'green', 'blue')},
                       'correlations': {c: report['channels'][c]['correlation'] for c in ('red', 'green', 'blue')}})
    summary = colour.repeatable_improvement(reports)
    summary['max_pose_delta'] = max(abs(a-b) for w in worlds for a, b in zip(w, worlds[0]))
    summary['phases'] = phases
    summary['metadata_pass'] = (not any(p['errors'] for p in phases) and summary['max_pose_delta'] < .0005
                                and alternating_counts([p['applications'] for p in phases])
                                and (not main_grading or alternating_counts([p['main_grading_applications'] for p in phases])))
    summary['main_grading_required'] = main_grading
    summary['pass'] = summary['metadata_pass'] and summary['bounded_colour_improvement_pass']
    summary['runtime_submission'] = False
    summary['simulation_frame_verified'] = False
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Check native/scene/native/scene PIP colour and GPU batch metadata.')
    parser.add_argument('directory', type=Path)
    parser.add_argument('--main-grading', action='store_true', help='Require main grading applications in both scene phases.')
    args = parser.parse_args()
    summary = analyze(args.directory, args.main_grading)
    (args.directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    raise SystemExit(0 if summary['pass'] else 1)
