"""One table for an effects benchmark campaign (scripts/bench_effects.py runs, one launch per pack).

    .venv/Scripts/python.exe -I scripts/summarize_effects_bench.py ABS_DUMP_DIR [ABS_DUMP_DIR ...] [--out ABS.md]

For every dump folder (one launch; rows are labelled by its folder name): reads its bench_effects_<label>.json windows, runs analyze_campaign_perf.py
over them (writing bench_perf.json there), and prints a markdown table of fresh pairs/s, frame time,
GPU busy per frame (PresentMon) and render-thread CPU per window. The delta columns compare each
window with the mean of the same launch's baseline windows, so launch-to-launch drift cancels.
"""
from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def row(label: str, window: dict) -> dict:
    m = window['metrics']
    gpu = m.get('gpu') or {}
    return {'pack': label, 'step': window['label'].split(':', 1)[-1], 'mode': window['mode'],
            'pairs': m.get('fresh_pairs_per_s'), 'frame_ms': (m.get('frame_ms') or {}).get('median'),
            'gpu_ms': gpu.get('gpu_busy_ms_median'), 'gpu_frac': gpu.get('gpu_busy_fraction'),
            'render_cpu': (m.get('threads') or {}).get('render_cpu_fraction')}


def with_deltas(rows: list[dict]) -> list[dict]:
    """Add pairs/GPU deltas against the mean of the same pack's 'baseline*' rows."""
    out = []
    for pack in dict.fromkeys(r['pack'] for r in rows):
        mine = [r for r in rows if r['pack'] == pack]
        base = [r for r in mine if r['step'].startswith('baseline') and r['pairs'] is not None]
        bp = statistics.fmean(r['pairs'] for r in base) if base else None
        bg = statistics.fmean(r['gpu_ms'] for r in base if r['gpu_ms'] is not None) if any(r['gpu_ms'] is not None for r in base) else None
        for r in mine:
            out.append({**r, 'd_pairs': None if bp is None or r['pairs'] is None else r['pairs'] - bp,
                        'd_gpu_ms': None if bg is None or r['gpu_ms'] is None else r['gpu_ms'] - bg})
    return out


def fmt(value, spec: str, signed: bool = False) -> str:
    if value is None:
        return '-'
    return format(value, ('+' if signed else '') + spec)


def table(rows: list[dict]) -> str:
    lines = ['| pack | window | pairs/s | vs base | frame ms | GPU busy ms | vs base | GPU busy | render CPU |',
             '|---|---|---:|---:|---:|---:|---:|---:|---:|']
    for r in rows:
        lines.append(f"| {r['pack']} | {r['step']}{' (mono)' if r['mode'] == 'mono' else ''} | {fmt(r['pairs'], '.1f')} | "
                     f"{fmt(r['d_pairs'], '.1f', True)} | {fmt(r['frame_ms'], '.1f')} | {fmt(r['gpu_ms'], '.1f')} | "
                     f"{fmt(r['d_gpu_ms'], '.1f', True)} | {fmt(r['gpu_frac'] and 100 * r['gpu_frac'], '.0f')}% | "
                     f"{fmt(r['render_cpu'] and 100 * r['render_cpu'], '.0f')}% |")
    return '\n'.join(lines)


def analyze(dump: Path) -> list[dict]:
    rows = []
    for receipt in sorted(dump.glob('bench_effects_*.json')):
        bench = json.loads(receipt.read_text())
        windows = [arg for s in bench['steps'] for arg in ('--window', f"{s['window'][0]}-{s['window'][1]}={s['label']}")]
        out = dump / f"bench_perf_{bench['label']}.json"
        subprocess.run([sys.executable, '-I', str(ROOT / 'scripts/analyze_campaign_perf.py'), str(dump), *windows,
                        '--min-seconds', '20', '--out', str(out)], check=True, stdout=subprocess.DEVNULL)
        # One launch per row group: the dump folder (run tag), so two launches of one pack stay apart.
        rows += [row(dump.name.removeprefix('bench_'), w) for w in json.loads(out.read_text())['windows']]
    return rows


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dumps', nargs='+', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args(argv)
    text = table(with_deltas([r for d in args.dumps for r in analyze(d.resolve())]))
    print(text)
    if args.out:
        args.out.write_text(text + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
