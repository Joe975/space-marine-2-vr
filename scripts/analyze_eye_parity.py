"""Measure slot-dependent shading between the two submitted eye images of a snapshot.

Usage:
  python scripts/analyze_eye_parity.py SNAPSHOT_DIR [SNAPSHOT_DIR ...] [--json OUT] [--heatmaps]

Each SNAPSHOT_DIR holds left.ppm, right.ppm and pair.json as written by -EyeSnapshots.
The metric is only meaningful when both slots rendered the same pose: launch with
-EyeSourceControl and write `left` (or `right`) to eye_source.txt, so slot 1 sits 0.5 mm from
slot 0 with the same projection. Then any remaining image difference comes from per-slot
native state (shadow cascades, light lists, histories), not stereo parallax. Native-pose
pairs are still reported but flagged `parallax`, and their numbers mix geometry with shading.

Both images are block-averaged (default 8x8 px) to suppress TAA/DLSS jitter and the 0.5 mm
offset, then compared on 8-bit luma. `signed_mean` > 0 means slot 1 (right image) is brighter.
--heatmaps writes parity_diff.png beside the images: the shading excess (difference beyond the
other eye's 3x3 block neighbourhood, so edge jitter drops out), red where slot 1 is brighter,
blue where slot 0 is brighter, saturating at 32 luma levels.
"""
import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
# 0.5 mm collapse offset plus float noise; a native IPD is ~60 mm.
COLLAPSED_TRANSLATION = 0.005
COLLAPSED_ROTATION = 1e-3
COLLAPSED_PROJECTION = 1e-4
# Fraction of blocks whose luma lies outside the other eye's local range by more than the
# threshold before a same-pose pair is called a mismatch (0.05% of a 360x360 block grid ~ 65 blocks; the
# 20 Oct HZB console row, five small objects, scored 0.07-0.16%).
MISMATCH_FRACTION = 0.0005


def load_rgb(path):
    with Image.open(path) as image:
        return np.asarray(image.convert('RGB'), dtype=np.float32)


def luma(rgb):
    return rgb @ LUMA


def block_mean(plane, block):
    rows, cols = plane.shape[0] // block, plane.shape[1] // block
    if rows == 0 or cols == 0:
        raise ValueError(f'image {plane.shape} smaller than block {block}')
    cropped = plane[:rows * block, :cols * block]
    return cropped.reshape(rows, block, cols, block).mean(axis=(1, 3))


def neighbourhood_range(plane):
    """Per-block min/max over the 3x3 block neighbourhood (edge-replicated)."""
    padded = np.pad(plane, 1, mode='edge')
    rows, cols = plane.shape
    shifts = np.stack([padded[y:y + rows, x:x + cols] for y in range(3) for x in range(3)])
    return shifts.min(axis=0), shifts.max(axis=0)


def shading_excess(left, right):
    """How far each block lies outside the other image's local range, symmetrically.

    A sub-block edge shift (TAA jitter, the 0.5 mm collapse offset, near-body parallax) stays
    inside the neighbour's range and scores ~0; a changed shadow or light over a flat area does not.
    """
    lo, hi = neighbourhood_range(left)
    right_excess = np.maximum(np.maximum(lo - right, right - hi), 0)
    lo, hi = neighbourhood_range(right)
    left_excess = np.maximum(np.maximum(lo - left, left - hi), 0)
    return np.maximum(right_excess, left_excess)


def pose_relation(pair):
    """Return translation/rotation/projection deltas between the two eyes of pair.json."""
    eyes = sorted(pair['eyes'], key=lambda eye: eye['eye'])
    if len(eyes) != 2:
        raise ValueError('pair.json needs exactly two eyes')
    a, b = (np.array(eye['world'], dtype=np.float64).reshape(4, 4) for eye in eyes)
    pa, pb = (np.array(eye['projection'], dtype=np.float64) for eye in eyes)
    translation = float(np.linalg.norm(a[3, :3] - b[3, :3]))
    rotation = float(np.abs(a[:3, :3] - b[:3, :3]).max())
    projection = float(np.abs(pa - pb).max())
    collapsed = (translation <= COLLAPSED_TRANSLATION and rotation <= COLLAPSED_ROTATION
                 and projection <= COLLAPSED_PROJECTION)
    return {'translation': translation, 'rotation': rotation, 'projection': projection,
            'collapsed': collapsed, 'render_ids': [eye.get('render_id') for eye in eyes]}


def projection_terms(eye):
    """(sx, ox, sy, oy) of a row-vector D3D projection: ndc = s * (view x|y / view z) + o."""
    m = eye['projection']
    return m[0], m[8], m[5], m[9]


def align_far(right_rgb, pair):
    """Resample the slot-1 image into slot 0's pixel grid for points at infinity.

    With equal eye rotations, a direction lands at ndc = s * t + o in each eye, so far content
    (sky, sun, light shafts, distant walls) maps by a per-axis affine transform. Near geometry
    still shows the 6 cm baseline. Returns (aligned image, valid mask) or None if the eyes' rotations
    differ (canted displays would need a full homography).
    """
    eyes = sorted(pair['eyes'], key=lambda eye: eye['eye'])
    a, b = (np.array(eye['world'], dtype=np.float64).reshape(4, 4) for eye in eyes)
    if np.abs(a[:3, :3] - b[:3, :3]).max() > COLLAPSED_ROTATION:
        return None
    lsx, lox, lsy, loy = projection_terms(eyes[0])
    rsx, rox, rsy, roy = projection_terms(eyes[1])
    height, width = right_rgb.shape[:2]
    ndc_x = (np.arange(width) + 0.5) / width * 2 - 1
    ndc_y = 1 - (np.arange(height) + 0.5) / height * 2
    right_x = rsx * (ndc_x - lox) / lsx + rox
    right_y = rsy * (ndc_y - loy) / lsy + roy
    columns = np.floor((right_x + 1) / 2 * width).astype(np.int64)
    rows = np.floor((1 - right_y) / 2 * height).astype(np.int64)
    valid_columns = (columns >= 0) & (columns < width)
    valid_rows = (rows >= 0) & (rows < height)
    aligned = right_rgb[np.clip(rows, 0, height - 1)][:, np.clip(columns, 0, width - 1)]
    return aligned, valid_rows[:, None] & valid_columns[None, :]


def compare(left_rgb, right_rgb, block=8, threshold=8.0, grid=4, valid=None):
    """Compare two same-pose images. Returns summary metrics and the block difference map.

    valid (optional, per pixel) excludes blocks that are not fully covered, e.g. after align_far.
    """
    if left_rgb.shape != right_rgb.shape:
        raise ValueError(f'image sizes differ: {left_rgb.shape} vs {right_rgb.shape}')
    left = block_mean(luma(left_rgb), block)
    right = block_mean(luma(right_rgb), block)
    covered = np.ones(left.shape, dtype=bool) if valid is None else block_mean(valid.astype(np.float32), block) >= 1.0
    right = np.where(covered, right, left)  # uncovered blocks compare equal
    diff = right - left
    magnitude = np.abs(diff)
    excess = shading_excess(left, right)
    rows, cols = diff.shape
    tiles = []
    for ty in range(grid):
        for tx in range(grid):
            tile = diff[ty * rows // grid:(ty + 1) * rows // grid, tx * cols // grid:(tx + 1) * cols // grid]
            tile_excess = excess[ty * rows // grid:(ty + 1) * rows // grid, tx * cols // grid:(tx + 1) * cols // grid]
            tiles.append({'tile': [tx, ty], 'mean_abs': round(float(np.abs(tile).mean()), 3),
                          'shading_over': round(float((tile_excess > threshold).mean()), 4),
                          'signed_mean': round(float(tile.mean()), 3),
                          'over_threshold': round(float((np.abs(tile) > threshold).mean()), 4)})
    tiles.sort(key=lambda tile: (-tile['shading_over'], -tile['mean_abs']))
    summary = {
        'block': block,
        'threshold': threshold,
        'left_mean': round(float(left.mean()), 3),
        'right_mean': round(float(right.mean()), 3),
        'mean_abs': round(float(magnitude.mean()), 3),
        'signed_mean': round(float(diff.mean()), 3),
        'p95_abs': round(float(np.percentile(magnitude, 95)), 3),
        'p99_abs': round(float(np.percentile(magnitude, 99)), 3),
        'over_threshold': round(float((magnitude > threshold).mean()), 4),
        'shading_mean': round(float(excess.mean()), 3),
        'shading_over': round(float((excess > threshold).sum() / max(covered.sum(), 1)), 4),
        'coverage': round(float(covered.mean()), 4),
        'worst_tiles': tiles[:3],
    }
    return summary, np.sign(diff) * excess


def heatmap(diff, saturation=32.0):
    scaled = np.clip(diff / saturation, -1.0, 1.0)
    image = np.full(diff.shape + (3,), 255.0, dtype=np.float32)
    positive, negative = np.clip(scaled, 0, 1), np.clip(-scaled, 0, 1)
    image[..., 1] -= 255.0 * (positive + negative)
    image[..., 2] -= 255.0 * positive  # red: slot 1 brighter
    image[..., 0] -= 255.0 * negative  # blue: slot 0 brighter
    return Image.fromarray(np.clip(image, 0, 255).astype(np.uint8), 'RGB')


def analyze_folder(folder, block=8, threshold=8.0, write_heatmap=False):
    folder = Path(folder)
    pair = json.loads((folder / 'pair.json').read_text())
    relation = pose_relation(pair)
    left_rgb, right_rgb = load_rgb(folder / 'left.ppm'), load_rgb(folder / 'right.ppm')
    summary, diff = compare(left_rgb, right_rgb, block, threshold)
    result = {'snapshot': folder.name, 'dispatch': pair.get('dispatch'), 'pose': relation, **summary,
              'verdict': verdict(relation, summary)}
    if not relation['collapsed']:
        aligned = align_far(right_rgb, pair)
        if aligned is not None:
            # Native-pose pair: score far content only. Sky, sun shafts and distant walls should
            # agree; near geometry keeps its stereo parallax and shows up as edges.
            far, diff = compare(left_rgb, aligned[0], block, threshold, valid=aligned[1])
            result['far'] = far
    if write_heatmap:
        heatmap(diff).save(folder / 'parity_diff.png')
    return result


def contact_sheet(results, folders, out, width=360):
    """One row per snapshot: slot 0, slot 1, shading excess heatmap, labelled with the verdict."""
    from PIL import ImageDraw
    rows = []
    for result, folder in zip(results, folders):
        folder = Path(folder)
        tiles = [Image.open(folder / name).convert('RGB').resize((width, width)) for name in ('left.ppm', 'right.ppm')]
        diff_path = folder / 'parity_diff.png'
        tiles.append(Image.open(diff_path).convert('RGB').resize((width, width), Image.NEAREST) if diff_path.is_file()
                     else Image.new('RGB', (width, width), 'white'))
        row = Image.new('RGB', (3 * width, width + 18), 'white')
        for index, tile in enumerate(tiles):
            row.paste(tile, (index * width, 18))
        far = f"  far (heatmap) shading_over={result['far']['shading_over']:.4f}" if 'far' in result else ''
        ImageDraw.Draw(row).text((4, 3), f"{result['snapshot']}  {result['verdict']}  shading_over={result['shading_over']:.4f}  "
                                         f"mean_abs={result['mean_abs']:.2f}{far}", fill='black')
        rows.append(row)
    sheet = Image.new('RGB', (3 * width, sum(row.height for row in rows)), 'white')
    y = 0
    for row in rows:
        sheet.paste(row, (0, y))
        y += row.height
    sheet.save(out)


def verdict(relation, summary):
    if not relation['collapsed']:
        return 'parallax'
    return 'match' if summary['shading_over'] < MISMATCH_FRACTION else 'mismatch'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('folders', nargs='+')
    parser.add_argument('--block', type=int, default=8)
    parser.add_argument('--threshold', type=float, default=8.0)
    parser.add_argument('--json')
    parser.add_argument('--heatmaps', action='store_true')
    parser.add_argument('--sheet', help='write a contact sheet PNG (implies --heatmaps)')
    args = parser.parse_args(argv)
    results = [analyze_folder(folder, args.block, args.threshold, args.heatmaps or bool(args.sheet)) for folder in args.folders]
    if args.sheet:
        contact_sheet(results, args.folders, args.sheet)
    print(f"{'snapshot':28} {'pose':9} {'mean_abs':>8} {'signed':>8} {'p99':>7} {'>thr':>7} {'shade>thr':>9}  verdict")
    for result in results:
        pose = 'same' if result['pose']['collapsed'] else f"{result['pose']['translation'] * 1000:.0f}mm"
        print(f"{result['snapshot']:28} {pose:9} {result['mean_abs']:8.3f} {result['signed_mean']:8.3f} "
              f"{result['p99_abs']:7.2f} {result['over_threshold']:7.4f} {result['shading_over']:9.4f}  {result['verdict']}")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2))
    return 0 if all(math.isfinite(result['mean_abs']) for result in results) else 1


if __name__ == '__main__':
    sys.exit(main())
