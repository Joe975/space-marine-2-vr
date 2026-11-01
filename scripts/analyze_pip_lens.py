"""Validate native final-lens metadata and predicted movement in actual PIP pixels."""
import argparse
import hashlib
import json
import math
from pathlib import Path


def ppm(path):
    raw = Path(path).read_bytes()
    magic, dimensions, maximum, pixels = raw.split(b"\n", 3)
    width, height = map(int, dimensions.split())
    if magic != b"P6" or maximum != b"255" or width < 5 or height < 5 or len(pixels) != width * height * 3:
        raise ValueError("invalid RGB PPM")
    return width, height, pixels


def multiply(a, b):
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]


def inverse_world(world):
    result = [0.0] * 16
    for r in range(3):
        for c in range(3):
            result[r * 4 + c] = world[c * 4 + r]
    for c in range(3):
        result[12 + c] = -sum(world[12 + k] * result[k * 4 + c] for k in range(3))
    result[15] = 1.0
    return result


def geometry_errors(capture, horizontal, vertical):
    errors = []
    dispatch = capture["native_dispatch"]
    before = [v for v in dispatch["before"] if v["pip"]]
    after = [v for v in dispatch["after"] if v["pip"]]
    if len(before) != 1 or len(after) != 1:
        return ["ambiguous PIP view"]
    view = after[0]
    p, inv, combined = (view[k] for k in ("projection_cache", "inverse_projection_cache", "combined_cache"))
    world, rectangle, corners, planes = (view[k] for k in ("world", "rectangle", "corners", "planes"))
    values = p + inv + combined + world + rectangle + corners + planes + [view["cull_radius"]]
    if not all(math.isfinite(x) for x in values) or p[10] in (0, 1):
        return ["invalid numeric camera data"]
    near, far = p[14] / (1 - p[10]), -p[14] / p[10]
    if near <= 0 or far <= near:
        return ["invalid clip planes"]
    if abs(p[8] + 2 * horizontal) > 1e-5 or abs(p[9] + 2 * vertical) > 1e-5:
        errors.append("final principal point does not match requested lens")
    identity = multiply(p, inv)
    if max(abs(x - (1 if i // 4 == i % 4 else 0)) for i, x in enumerate(identity)) > 1e-4:
        errors.append("stale inverse projection")
    if max(abs(a - b) for a, b in zip(multiply(inverse_world(world), p), combined)) > 1e-3:
        errors.append("stale combined view/projection")
    l, b, width, height = rectangle
    expected_corners = [l, b, l + width, b, l + width, b + height, l, b + height]
    if max(abs(a - b) for a, b in zip(corners, expected_corners)) > 1e-5:
        errors.append("corner cache differs from rectangle")
    for x, y in zip(corners[::2], corners[1::2]):
        radius = math.sqrt(sum((world[a] * x + world[4 + a] * y + world[8 + a] * near) ** 2 for a in range(3)))
        if view["cull_radius"] + 1e-7 < radius:
            errors.append("near-corner bound is too small")
            break
    if not all(k in view for k in ("cull_directions", "cull_sides", "cull_normal", "cull_origin", "cull_distance")):
        errors.append("missing compact culling cache")
    else:
        normal, origin = view["cull_normal"], view["cull_origin"]
        if not all(math.isfinite(x) for x in normal + origin + view["cull_directions"] + view["cull_sides"] + [view["cull_distance"]]):
            return errors + ["invalid compact culling data"]
        if max(abs(normal[a] + world[8 + a]) for a in range(3)) > 1e-5 or max(abs(origin[a] - world[12 + a]) for a in range(3)) > 1e-4:
            errors.append("compact culling origin or cap normal differs from camera")
        if abs(view["cull_distance"] + near * sum(world[8 + a] ** 2 for a in range(3))) > 1e-5:
            errors.append("compact near cap differs from true near plane")
        directions, sides = view["cull_directions"], view["cull_sides"]
        for corner in range(4):
            direction = [world[a] * corners[corner * 2] + world[4 + a] * corners[corner * 2 + 1] + world[8 + a] * near for a in range(3)]
            length = math.sqrt(sum(a * a for a in direction))
            if max(abs(directions[corner * 3 + a] - direction[a] / length) for a in range(3)) > 1e-5:
                errors.append("compact corner directions differ from final lens")
            for axis in range(3):
                component_a, component_b, following = (axis + 1) % 3, (axis + 2) % 3, (corner + 1) % 4
                cross = directions[corner * 3 + component_a] * directions[following * 3 + component_b] - directions[corner * 3 + component_b] * directions[following * 3 + component_a]
                if abs(sides[corner * 3 + axis] - cross) > 1e-5:
                    errors.append("compact side normals differ from corner rays")
    # Independent classification over a grid at three depths, including rays
    # outside all four edges and points beyond both clip planes.
    for depth in (near * .5, min(10, far * .5), far * 1.1):
        for u in (-.1, .1, .5, .9, 1.1):
            for v in (-.1, .1, .5, .9, 1.1):
                x, y = (l + width * u) / near * depth, (b + height * v) / near * depth
                point = [world[12 + a] + world[a] * x + world[4 + a] * y + world[8 + a] * depth for a in range(3)]
                inside = True
                for offset in range(0, 24, 4):
                    n = planes[offset:offset + 3]
                    length = math.sqrt(sum(a * a for a in n))
                    if length < 1e-8 or (sum(a * b for a, b in zip(n, point)) - planes[offset + 3]) / length < -.002:
                        inside = False
                expected = near < depth < far and 0 < u < 1 and 0 < v < 1
                if inside != expected:
                    errors.append("native culling disagrees with final lens")
                    break
    mains_before = [v for v in dispatch["before"] if not v["pip"]]
    mains_after = [v for v in dispatch["after"] if not v["pip"]]
    if len(mains_before) != 1 or len(mains_after) != 1 or mains_before[0] != mains_after[0]:
        errors.append("main view changed during native dispatch")
    return sorted(set(errors))


def pixel_shift(reference, candidate, expected, radius=4, region=None, step=6):
    width, height, pixels = reference
    if candidate[:2] != (width, height):
        raise ValueError("different PIP output sizes")
    other = candidate[2]
    # Static upper architecture; avoid most actors, HUD and moving combat effects.
    x0, y0, x1, y1 = region or (240, 90, 1040, 240)
    def gradient(data, x, y):
        def value(px, py):
            i = (py * width + px) * 3
            return data[i] + data[i + 1] + data[i + 2]
        return value(x + 2, y) - value(x - 2, y), value(x, y + 2) - value(x, y - 2)
    points = [(x, y, gradient(pixels, x, y)) for y in range(y0, y1, step) for x in range(x0, x1, step)]
    def score(dx, dy):
        cross = first = second = 0
        for x, y, a in points:
            px, py = x + dx, y + dy
            if not (2 <= px < width - 2 and 2 <= py < height - 2):
                continue
            b = gradient(other, px, py)
            cross += a[0] * b[0] + a[1] * b[1]
            first += a[0] ** 2 + a[1] ** 2
            second += b[0] ** 2 + b[1] ** 2
        return cross / math.sqrt(first * second) if first and second else -1.0
    dx, dy = expected
    candidates = [(score(x, y), x, y) for x in range(dx - radius, dx + radius + 1) for y in range(dy - radius, dy + radius + 1)]
    best, x, y = max(candidates)
    zero = score(0, 0)
    return {"expected": [dx, dy], "measured": [x, y], "gradient_correlation": best, "zero_shift_correlation": zero,
            "pass": abs(x - dx) <= 2 and abs(y - dy) <= 2 and best >= .7 and best >= zero + .2}


def analyze(root, horizontal=.15, vertical=.1):
    root = Path(root)
    captures = [json.loads((root / f"phase_{phase}" / "capture.json").read_text()) for phase in range(4)]
    images = [ppm(root / f"phase_{phase}" / "pip.ppm") for phase in range(4)]
    result = {"runtime_submission": False, "headset_used": False, "same_gpu_producing_frame_verified": False, "phases": []}
    for phase, sign in enumerate((1, 0, -1, 1)):
        errors = geometry_errors(captures[phase], horizontal * sign, vertical * sign)
        if not captures[phase].get("final_lens_enabled") or captures[phase].get("lens_phase") != phase:
            errors.append("wrong lens mode or capture phase")
        item = {"phase": phase, "geometry_errors": errors,
                "pip_sha256": hashlib.sha256((root / f"phase_{phase}" / "pip.ppm").read_bytes()).hexdigest()}
        if phase != 1:
            item["pixels"] = pixel_shift(images[1], images[phase], (round(-horizontal * sign * images[phase][0]), round(vertical * sign * images[phase][1])))
        result["phases"].append(item)
    worlds = [[v for v in c["native_dispatch"]["after"] if v["pip"]][0]["world"] for c in captures]
    result["max_pose_delta"] = max(abs(a - b) for world in worlds for a, b in zip(world, worlds[1]))
    result["pass"] = result["max_pose_delta"] < .0005 and all(not p["geometry_errors"] and p.get("pixels", {"pass": True})["pass"] for p in result["phases"])
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text + "\n")
    print(text)
    raise SystemExit(0 if result["pass"] else 1)
