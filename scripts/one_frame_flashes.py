"""Find one-frame flashes per eye in a mirror recording and render the worst ones.

Usage: one_frame_flashes.py <abs prefix of a recorded step> [<prefix> ...] --out-dir <abs dir> [--top 6]

A pixel flashes in frame t when it differs from both t-1 and t+1 in the same direction by more than
THRESHOLD luma levels: it changes for exactly one frame and comes back. Moving edges change
monotonically, so a slow head turn mostly does not flash. Frames are read through a memory map,
three at a time, so 1/4-size streams fit. Per step it prints the mean share of flashing pixels per
eye and how many frames flash over 0.2% of the image, and draws the frames where the right eye
flashes most beyond the left: right t-1, right t, right t+1, the right eye's flash map, left t.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import viewer_flicker  # noqa: E402

THRESHOLD = 12.0


def flash(prev: np.ndarray, cur: np.ndarray, nxt: np.ndarray) -> np.ndarray:
    a, b = cur - prev, cur - nxt
    return (np.sign(a) == np.sign(b)) & (np.minimum(np.abs(a), np.abs(b)) > THRESHOLD)


def analyse(prefix: Path, out_dir: Path, top: int, scale: int):
    left, right, times = viewer_flicker.load_mirror(prefix, mapped=True)
    lum = lambda eye, i: viewer_flicker.luma(np.asarray(eye[i], np.float32))
    n = len(right)
    sr, sl = np.zeros(n), np.zeros(n)
    window = {}
    for i in range(n):
        window[i] = (lum(left, i), lum(right, i))
        window.pop(i - 3, None)
        if i >= 2:
            t = i - 1
            sl[t] = flash(window[t - 1][0], window[t][0], window[t + 1][0]).mean()
            sr[t] = flash(window[t - 1][1], window[t][1], window[t + 1][1]).mean()
    inner = slice(1, n - 1)
    print(f"{prefix.name}: frames={n} flash share mean right={sr[inner].mean():.5f} left={sl[inner].mean():.5f}; "
          f"frames over 0.2%: right={int((sr > .002).sum())} left={int((sl > .002).sum())}; "
          f"right-only (right>0.2%, left<0.05%): {int(((sr > .002) & (sl < .0005)).sum())}")
    order = [int(k) for k in np.argsort(-(sr - sl)) if 1 <= k < n - 1][:top]
    rows = []
    for t in order:
        fr = flash(lum(right, t - 1), lum(right, t), lum(right, t + 1))
        heat = np.repeat((fr * 255).astype(np.uint8)[..., None], 3, axis=2)
        rows.append(np.concatenate([right[t - 1], right[t], right[t + 1], heat, left[t]], axis=1).astype(np.uint8))
        print(f"  frame {t} t={times[t] - times[0]:.2f}s right={sr[t]:.4f} left={sl[t]:.4f}")
    if rows:
        from PIL import Image
        image = np.concatenate(rows, axis=0)
        out = out_dir / f"flash_{prefix.name}.png"
        Image.fromarray(image).resize((image.shape[1] // scale, image.shape[0] // scale), Image.BILINEAR).save(out)
        print(f"  wrote {out}")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("prefixes", nargs="+", type=Path)
    p.add_argument("--out-dir", type=Path, required=True)
    p.add_argument("--top", type=int, default=6)
    p.add_argument("--scale", type=int, default=1, help="shrink the drawn image by this factor")
    a = p.parse_args(argv)
    for prefix in a.prefixes:
        analyse(prefix, a.out_dir, a.top, a.scale)


if __name__ == "__main__":
    main()
