"""Measure a selected menu button in retained VR eye images (stdlib only).

Finds a closed, bright rectangular border in an explicit search region. Fails
closed when absent/ambiguous; a loading screen or gameplay is not a menu pass.
This reads the mod's GPU exports, not Windows screenshots. It cannot judge comfort.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib


def read_ppm(path):
    with Path(path).open("rb") as f:
        if f.readline() != b"P6\n":
            raise ValueError("expected diagnostic RGB PPM")
        width, height = map(int, f.readline().split())
        if not 1 <= width <= 8192 or not 1 <= height <= 8192 or f.readline() != b"255\n":
            raise ValueError("invalid image dimensions/format")
        pixels = f.read(width*height*3+1)
        if len(pixels) != width*height*3:
            raise ValueError("truncated or oversized image")
    return width, height, pixels


def png(path, width, height, pixels):
    # Lossless viewable copy of the app's RGB render artifact.
    def chunk(kind, data):
        return struct.pack(">I", len(data))+kind+data+struct.pack(">I", zlib.crc32(kind+data))
    rows = b"".join(b"\0"+pixels[y*width*3:(y+1)*width*3] for y in range(height))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))+
                          chunk(b"IDAT", zlib.compress(rows, 3))+chunk(b"IEND", b""))


def selected_panel(image, search, min_width, max_width):
    width, height, pixels = image
    x0, y0, x1, y1 = search
    if not 0 <= x0 < x1 <= width or not 0 <= y0 < y1 <= height:
        raise ValueError("search region outside image")
    def bright(x, y):
        r, g, b = pixels[(y*width+x)*3:(y*width+x)*3+3]
        # A one-pixel gold border can dim during the real GPU's linear resampling.
        return r >= 75 and g >= 50 and b >= 25 and max(r, g, b)-min(r, g, b) <= 160
    lines = []
    for y in range(y0, y1):
        runs = []
        for x in range(x0, x1):
            if bright(x, y):
                if runs and x-runs[-1][1] <= 10:
                    runs[-1][1] = x
                    runs[-1][2] += 1
                else:
                    runs.append([x, x, 1])
        for left, right, hits in runs:
            if min_width <= right-left+1 <= max_width and hits/(right-left+1) >= .8:
                lines.append((left, right, y))
    candidates = []
    for n, (left, right, top) in enumerate(lines):
        for other_left, other_right, bottom in lines[n+1:]:
            if not 18 <= bottom-top <= 150 or abs(left-other_left) > 3 or abs(right-other_right) > 3:
                continue
            hits = sum(any(bright(x, y) for x in range(max(x0, edge-3), min(x1, edge+4)))
                       for edge in (left, right) for y in range(top, bottom+1))
            if hits/(2*(bottom-top+1)) < 0.8:
                continue
            box = [left, top, right+1, bottom+1]
            if not any(max(abs(a-b) for a, b in zip(box, previous)) <= 5 for previous in candidates):
                candidates.append(box)
    if len(candidates) != 1:
        raise ValueError(f"expected one selected panel, found {len(candidates)}")
    return candidates[0]


def ray_angles(eye, box):
    projection = eye["projection"]
    if len(projection) != 16 or any(not math.isfinite(v) for v in projection) or projection[0] <= 0 or projection[5] <= 0:
        raise ValueError("invalid projection")
    u = (box[0]+box[2])/(2*eye["width"])
    v = (box[1]+box[3])/(2*eye["height"])
    x = (2*u-1-projection[8])/projection[0]
    y = (1-2*v-projection[9])/projection[5]
    result = {"centre_uv": [u, v], "texture_offset_percent": [(u-.5)*100, (v-.5)*100],
              "eye_ray_degrees": [math.degrees(math.atan2(x, 1)), math.degrees(math.atan2(y, math.sqrt(1+x*x)))]}
    if "eye_to_head" in eye:
        distance = eye.get("menu_distance_metres", 2)
        result["head_plane_x_metres"] = x*distance+eye["eye_to_head"][12]
        result["head_plane_horizontal_degrees"] = math.degrees(math.atan2(result["head_plane_x_metres"], distance))
    return result


def analyze(directory, search, min_width, max_width, tolerance):
    root = Path(directory)
    metadata = json.loads((root/"pair.json").read_text())
    eyes = metadata["eyes"]
    if len(eyes) != 2 or [e["eye"] for e in eyes] != [0, 1] or eyes[1]["render_id"] != eyes[0]["render_id"]+1:
        raise ValueError("not an adjacent left/right pair")
    results = []
    for i, name in enumerate(("left", "right")):
        path = root/(name+".ppm")
        image = read_ppm(path)
        if list(image[:2]) != [eyes[i]["width"], eyes[i]["height"]]:
            raise ValueError("metadata/image dimensions differ")
        png(root/(name+".png"), *image)
        try:
            box = selected_panel(image, search, min_width, max_width)
        except ValueError as error:
            raise ValueError(f"{name}: {error}") from error
        results.append({"eye": i, "box": box, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), **ray_angles(eyes[i], box)})
    panel_mode = all(e.get("screen_menu", False) for e in eyes)
    texture_centred = all(abs(e["texture_offset_percent"][0]) <= tolerance for e in results)
    plane_error = abs(results[0]["head_plane_x_metres"]-results[1]["head_plane_x_metres"]) if all("head_plane_x_metres" in e for e in results) else None
    panel_centred = panel_mode and plane_error is not None and plane_error < .02 and all(abs(e["head_plane_horizontal_degrees"]) <= 2 for e in results)
    return {"eyes": results, "texture_centred": texture_centred, "menu_panel": panel_mode,
            "plane_convergence_error_metres": plane_error, "panel_centred": panel_centred,
            "accepted": panel_centred if plane_error is not None else texture_centred,
            "horizontal_tolerance_percent": tolerance, "scope": "selected panel geometry; no headset comfort or menu identity claim"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--search", type=int, nargs=4, required=True)
    parser.add_argument("--width", type=int, nargs=2, default=[200, 1000])
    parser.add_argument("--tolerance", type=float, default=3)
    args = parser.parse_args()
    report = analyze(args.directory, args.search, *args.width, args.tolerance)
    (args.directory/"analysis.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report["accepted"] else 1)
