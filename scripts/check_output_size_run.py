"""Check that a -OutputSize run rendered whole eyes at the requested size (docs/EYE_SIZE.md).

Usage: check_output_size_run.py ABS_SM2VR_LOG ABS_ORDINARY_DUMP_DIR WxH

Joe's 21 Oct campaign run (device mode) had a 1944x2160 swap chain and eye copies, while the
renderer's colour targets stayed 3840x2160 and DLSS kept a 16:9 1280x720 input. The eye was the
left half of a 3840-wide image: everything looked twice as wide ("zoomed in"). This checker fails
such a run. It prints PASS/FAIL lines and exits 1 on any failure.

  swap chain         last logged `Sm2Probe: ... swap chain WxH` equals the request
  eye copies         every headset_handoff pairs[].eyes[].copy is WxH (at least one)
  colour targets     every native colour-boundary render target is WxH (a larger one means a crop)
  upscaler input     the DLSS input from the motion-vector scale (1/width, 1/height) has the
                     output's aspect within 3%
"""
import argparse
import json
import re
import sys
from pathlib import Path

SWAP = re.compile(r'Sm2Probe: [a-z0-9 ]+: swap chain (\d+)x(\d+)')
MVEC = re.compile(r'StreamlineProbe: frame=\d+ viewport=(\d+) .*?mvec=\[(-?[0-9.e-]+),(-?[0-9.e-]+)\]')


def parse_size(text):
    m = re.fullmatch(r'([1-9]\d{2,3})x([1-9]\d{2,3})', text)
    if not m:
        raise argparse.ArgumentTypeError('size must look like 1944x2160')
    return int(m[1]), int(m[2])


def handoff_sizes(dump: Path):
    copies, colour = [], []
    for f in sorted(dump.glob('headset_handoff_*.json')):
        data = json.loads(f.read_text(errors='replace'))
        for pair in data.get('pairs', []) or []:
            for eye in pair.get('eyes', []) or []:
                c = eye.get('copy') or {}
                if 'width' in c:
                    copies.append((c['width'], c['height']))
        for event in (data.get('colour') or {}).get('events', []) or []:
            for rtv in event.get('rtvs', []) or []:
                if 'width' in rtv:
                    colour.append((rtv['width'], rtv['height']))
    return copies, colour


def upscaler_inputs(lines):
    """Input sizes implied by the motion-vector scale of headset frames (viewport != 0 first)."""
    found = {}
    for line in lines:
        m = MVEC.search(line)
        if not m:
            continue
        x, y = abs(float(m[2])), abs(float(m[3]))
        if x <= 0 or y <= 0:
            continue
        found.setdefault(m[1] != '0', set()).add((round(1 / x), round(1 / y)))
    return sorted(found.get(True) or found.get(False) or set())


def check(lines, dump: Path, size):
    results = []
    swaps = [(int(m[1]), int(m[2])) for m in map(SWAP.search, lines) if m]
    results.append(('swap chain', bool(swaps) and swaps[-1] == size, f'last {swaps[-1] if swaps else None}, requested {size}'))
    copies, colour = handoff_sizes(dump)
    results.append(('eye copies', bool(copies) and all(c == size for c in copies), f'{sorted(set(copies))} in {len(copies)} copies'))
    wrong = sorted({c for c in colour if c != size})
    results.append(('colour targets', bool(colour) and not wrong,
                    f'{sorted(set(colour))}' + (f'; {wrong} differ from the output: the eye is a crop of a larger image' if wrong else '')))
    inputs = upscaler_inputs(lines)
    aspect = size[0] / size[1]
    bad = [i for i in inputs if abs(i[0] / i[1] - aspect) > 0.03 * aspect]
    results.append(('upscaler input', bool(inputs) and not bad,
                    f'{inputs} (output aspect {aspect:.3f})' + (f'; {bad} has a different aspect' if bad else '')))
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('log', type=Path)
    parser.add_argument('dump', type=Path)
    parser.add_argument('size', type=parse_size)
    args = parser.parse_args(argv)
    if not (args.log.is_absolute() and args.log.is_file() and args.dump.is_absolute() and args.dump.is_dir()):
        parser.error('log and dump must be absolute paths to an existing file and directory')
    with args.log.open(encoding='utf-8', errors='replace') as handle:
        results = check(list(handle), args.dump, args.size)
    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}")
    return 0 if all(ok for _, ok, _ in results) else 1


if __name__ == '__main__':
    sys.exit(main())
