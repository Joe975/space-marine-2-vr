"""Where a still view changes, per eye (docs/RIGHT_EYE_FLICKER_2026-10-28.md).

Usage: still_view_heatmap.py <abs route dir> <step name> [<step name> ...] --out <abs png>

For each recorded step (e.g. 1R_9camhold) the mirror stream's frames after a 2 s settle are compared
with the previous frame; a pixel counts as changed when its luma moves by more than 4 levels. The
output PNG has one row per step: the right eye's first frame, the right eye's change-count heat map,
and the left eye's heat map (white = changes in every frame), so the changing regions can be looked at.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import viewer_flicker  # noqa: E402

THRESHOLD = 4.0


def heat(eye: np.ndarray) -> np.ndarray:
    luma = viewer_flicker.luma(eye.astype(np.float32))
    changed = np.abs(np.diff(luma, axis=0)) > THRESHOLD
    return changed.sum(axis=0) / max(len(changed), 1)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("route", type=Path)
    p.add_argument("steps", nargs="+")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--scale", type=int, default=3)
    a = p.parse_args(argv)
    if not a.route.is_absolute() or not a.out.is_absolute():
        p.error("paths must be absolute")
    from PIL import Image
    rows = []
    for step in a.steps:
        left, right, times = viewer_flicker.load_mirror(a.route / step)
        keep = times >= times[0] + 2.0
        left, right = left[keep], right[keep]
        hr, hl = heat(right), heat(left)
        print(f"{step}: frames={len(right)} right changed-pixel share={float((hr > 0.05).mean()):.4f} "
              f"left={float((hl > 0.05).mean()):.4f} right max={hr.max():.2f}")
        to_rgb = lambda h: np.repeat((np.clip(h / max(h.max(), 1e-6), 0, 1) * 255).astype(np.uint8)[..., None], 3, axis=2)
        rows.append(np.concatenate([right[0].astype(np.uint8), to_rgb(hr), to_rgb(hl)], axis=1))
    image = np.concatenate(rows, axis=0)
    Image.fromarray(image).resize((image.shape[1] * a.scale, image.shape[0] * a.scale), Image.NEAREST).save(a.out)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
