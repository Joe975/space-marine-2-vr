"""Reproject selected pixels through saved ordinary-pair cameras (no runtime access).

Depth is forward Z in the selected source camera, in native world units. A supplied
depth interval is an assumption, never a measured object bound. Row-vector matrices
match VrCameraPose.h; finite endpoints also bound the rational projection between
them provided target W remains positive. This does not identify meshes or prove
occlusion/material correctness.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
from pathlib import Path


def multiply(vector, matrix):
    return [sum(vector[k] * matrix[k * 4 + j] for k in range(4)) for j in range(4)]


def inverse(matrix):
    if len(matrix) != 16 or not all(math.isfinite(x) for x in matrix):
        raise ValueError("invalid matrix")
    rows = [list(matrix[i * 4:i * 4 + 4]) + [float(i == j) for j in range(4)] for i in range(4)]
    for j in range(4):
        pivot = max(range(j, 4), key=lambda i: abs(rows[i][j]))
        if abs(rows[pivot][j]) < 1e-12:
            raise ValueError("singular matrix")
        rows[j], rows[pivot] = rows[pivot], rows[j]
        divisor = rows[j][j]
        rows[j] = [v / divisor for v in rows[j]]
        for i in range(4):
            if i != j:
                factor = rows[i][j]
                rows[i] = [a - factor * b for a, b in zip(rows[i], rows[j])]
    return [v for row in rows for v in row[4:]]


def source_ray(eye, pixel, size):
    """Return view-space direction with forward Z=1; refuse noncentral optics."""
    width, height = size
    if width <= 0 or height <= 0 or not all(math.isfinite(v) for v in pixel):
        raise ValueError("invalid pixel/size")
    clip = [2 * pixel[0] / width - 1, 1 - 2 * pixel[1] / height, 0.5, 1]
    point = multiply(clip, inverse(eye["projection"]))
    # Pair projections are central perspective. The inverse's XYZ direction is
    # independent of clip depth; test that before using view-Z parameterization.
    second = multiply(clip[:2] + [0.25, 1], inverse(eye["projection"]))
    if abs(point[2]) < 1e-12 or abs(second[2]) < 1e-12:
        raise ValueError("not forward perspective")
    ray = [point[i] / point[2] for i in range(3)]
    if any(abs(ray[i] - second[i] / second[2]) > 1e-7 for i in range(3)):
        raise ValueError("noncentral projection unsupported")
    return ray


def reproject(source, target, pixel, size, depth):
    if depth <= 0 or not math.isfinite(depth):
        raise ValueError("positive finite source depth required")
    ray = source_ray(source, pixel, size)
    point = multiply([v * depth for v in ray] + [1], source["world"])
    view = multiply(point, inverse(target["world"]))
    clip = multiply(view, target["projection"])
    if clip[3] <= 1e-9:
        raise ValueError("target point behind eye or crosses near infinity")
    ndc = [clip[i] / clip[3] for i in range(3)]
    width, height = size
    return {"pixel": [(ndc[0] + 1) * width / 2, (1 - ndc[1]) * height / 2],
            "target_w": clip[3], "world": point[:3], "target_view": view[:3]}


def qualify(pair, size, bounds, depths, source_eye=0):
    if len(bounds) != 4 or bounds[0] > bounds[2] or bounds[1] > bounds[3]:
        raise ValueError("ordered x0,y0,x1,y1 bounds required")
    if len(depths) != 2 or not 0 < depths[0] <= depths[1]:
        raise ValueError("ordered positive depth interval required")
    eyes = {eye["eye"]: eye for eye in pair["eyes"]}
    if set(eyes) != {0, 1} or not pair.get("submitted"):
        raise ValueError("submitted two-eye pair required")
    if eyes[0]["render_id"] != eyes[1]["render_id"]:
        raise ValueError("different render IDs")
    samples = [reproject(eyes[source_eye], eyes[1 - source_eye], (x, y), size, depth)
               for x, y, depth in itertools.product((bounds[0], bounds[2]),
                                                   (bounds[1], bounds[3]), depths)]
    xs, ys = zip(*(sample["pixel"] for sample in samples))
    projected = [min(xs), min(ys), max(xs), max(ys)]
    width, height = size
    inside = projected[0] >= 0 and projected[1] >= 0 and projected[2] <= width and projected[3] <= height
    outside = projected[2] < 0 or projected[3] < 0 or projected[0] > width or projected[1] > height
    return {"model": "saved_pair_pixel_overlap_v1", "dispatch": pair["dispatch"],
            "render_id": eyes[0]["render_id"], "source_eye": source_eye,
            "size": list(size), "source_pixel_bounds": list(bounds),
            "assumed_source_depth_interval": list(depths),
            "target_pixel_bounds": projected,
            "classification": "inside" if inside else "outside" if outside else "partial",
            "depth_measured": False, "mesh_identity_verified": False,
            "occlusion_verified": False, "samples": samples}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pair", type=Path, required=True)
    parser.add_argument("--size", nargs=2, type=int, required=True)
    parser.add_argument("--bounds", nargs=4, type=float, required=True)
    parser.add_argument("--depths", nargs=2, type=float, required=True)
    parser.add_argument("--source-eye", type=int, choices=(0, 1), default=0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = qualify(json.loads(args.pair.read_text()), args.size, args.bounds, args.depths, args.source_eye)
    result["pair_path"] = str(args.pair.resolve())
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
