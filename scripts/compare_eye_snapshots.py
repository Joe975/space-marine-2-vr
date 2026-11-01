"""Compare an -OutputSize eye snapshot with a 4K snapshot of the same pose (docs/EYE_SIZE.md).

Usage: compare_eye_snapshots.py ABS_REFERENCE.ppm ABS_TEST.ppm [--pair-json-ref A --pair-json-test B]

Both eyes use the same headset projection whatever the output size, so a correct test image is
the reference resampled to the test size (an anamorphic resize). Joe's zoomed 21 Oct run instead
showed the left part of a reference-width image: a crop. The script scores the test image against
both candidates (mean absolute difference of grey levels, 0-255, at a common low resolution) and
passes only when the resize is clearly the better match. With pair.json files it also requires the
two snapshots to share the same eye pose.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

COMPARE = (243, 270)  # common low resolution, eye aspect


def grey(image):
    return np.asarray(image.convert('L').resize(COMPARE, Image.BILINEAR), dtype=np.float32)


def candidates(reference, test_size):
    """The correct (resized) and the zoomed (left crop) renderings of the reference at test_size."""
    resized = reference.resize(test_size, Image.BILINEAR)
    crop_width = min(reference.width, test_size[0] * reference.height // test_size[1])
    cropped = reference.crop((0, 0, crop_width, reference.height)).resize(test_size, Image.BILINEAR)
    return resized, cropped


def score(reference, test):
    resized, cropped = candidates(reference, test.size)
    t = grey(test)
    return float(np.abs(t - grey(resized)).mean()), float(np.abs(t - grey(cropped)).mean())


def same_pose(a, b, metres=0.02):
    ea, eb = json.loads(Path(a).read_text())['eyes'], json.loads(Path(b).read_text())['eyes']
    return len(ea) == len(eb) and all(
        np.allclose(x['world'][12:15], y['world'][12:15], atol=metres) and np.allclose(x['world'][:12], y['world'][:12], atol=0.01)
        and np.allclose(x['projection'], y['projection'], atol=1e-3) for x, y in zip(ea, eb))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('reference', type=Path)
    parser.add_argument('test', type=Path)
    parser.add_argument('--pair-json-ref', type=Path)
    parser.add_argument('--pair-json-test', type=Path)
    parser.add_argument('--margin', type=float, default=2.0, help='resize must beat crop by this many grey levels')
    args = parser.parse_args(argv)
    for p in (args.reference, args.test):
        if not p.is_absolute() or not p.is_file():
            parser.error(f'{p} must be an absolute path to an existing image')
    ok = True
    if args.pair_json_ref and args.pair_json_test:
        pose = same_pose(args.pair_json_ref, args.pair_json_test)
        print(f"{'PASS' if pose else 'FAIL'}  same eye pose and projection")
        ok &= pose
    reference, test = Image.open(args.reference), Image.open(args.test)
    resized, cropped = score(reference, test)
    whole = resized + args.margin < cropped
    print(f"{'PASS' if whole else 'FAIL'}  whole eye: difference to resized reference {resized:.1f}, to left crop {cropped:.1f} "
          f"({reference.width}x{reference.height} -> {test.width}x{test.height})")
    ok &= whole
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
