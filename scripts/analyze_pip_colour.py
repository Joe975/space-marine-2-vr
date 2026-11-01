"""Compare matched desktop main/PIP captures; diagnose transfer hypotheses without correcting pixels."""
import argparse
import json
import math
from pathlib import Path


def read_ppm(path):
    magic, dims, maximum, data = Path(path).read_bytes().split(b"\n", 3)
    width, height = map(int, dims.split())
    if magic != b"P6" or maximum != b"255" or len(data) != width * height * 3:
        raise ValueError("invalid RGB PPM")
    return width, height, data


def encode(x):
    return 12.92 * x if x <= .0031308 else 1.055 * x ** (1 / 2.4) - .055


def decode(x):
    return x / 12.92 if x <= .04045 else ((x + .055) / 1.055) ** 2.4


def fit(pairs):
    if not pairs:
        raise ValueError("no comparison samples")
    n = len(pairs)
    x = sum(a for a, _ in pairs) / n
    y = sum(b for _, b in pairs) / n
    xx = sum((a - x) ** 2 for a, _ in pairs)
    yy = sum((b - y) ** 2 for _, b in pairs)
    xy = sum((a - x) * (b - y) for a, b in pairs)
    gain = xy / xx if xx else 0
    bias = y - gain * x
    error = lambda transform: sum(abs(transform(a) - b) for a, b in pairs) / n * 255
    return {"samples": n, "pip_mean": x * 255, "main_mean": y * 255,
            "correlation": xy / math.sqrt(xx * yy) if xx and yy else 0,
            "linear_gain": gain, "linear_bias_bytes": bias * 255,
            "mae_bytes": {"identity": error(lambda a: a), "linear_fit": error(lambda a: gain * a + bias),
                          "linear_to_srgb": error(encode), "srgb_to_linear": error(decode)}}


def analyze(main, pip, region=None, step=None):
    mw, mh, rgb = main
    pw, ph, other = pip
    if not pw or not ph or mw % pw or mh % ph or mw // pw != mh // ph:
        raise ValueError("comparison requires equal aspect and integer downsample")
    scale = mw // pw
    # Keep the same angular/normalised architecture region at larger PIP sizes.
    if region is None:
        region = tuple(round(value * pw / 1280) for value in (240, 90, 1040, 240))
    if step is None:
        step = max(1, round(4 * pw / 1280))
    x0, y0, x1, y1 = region
    if not (0 <= x0 < x1 <= pw and 0 <= y0 < y1 <= ph):
        raise ValueError("comparison region outside image")
    pairs = [[] for _ in range(3)]
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            for channel in range(3):
                main_byte = sum(rgb[((y * scale + dy) * mw + x * scale + dx) * 3 + channel]
                                for dy in range(scale) for dx in range(scale)) / (scale * scale)
                pip_byte = other[(y * pw + x) * 3 + channel]
                pairs[channel].append((pip_byte / 255, main_byte / 255))
    return {"channels": dict(zip(("red", "green", "blue"), map(fit, pairs))),
            "region": list(region), "main_downsample": scale,
            "calibrated_colour_parity": False,
            "limits": "Moving effects, resolution, jitter and post-processing may differ; a lower fit error does not identify a shader or establish transfer semantics."}


def repeatable_improvement(reports):
    """Both scene phases must halve each channel's error against both native controls' mean."""
    if len(reports) != 4:
        raise ValueError("four native/scene/native/scene reports required")
    channels = ("red", "green", "blue")
    baseline = {c: sum(reports[i]["channels"][c]["mae_bytes"]["identity"] for i in (0, 2)) / 2 for c in channels}
    ratios = [{c: reports[i]["channels"][c]["mae_bytes"]["identity"] / baseline[c] if baseline[c] > 0 else math.inf
               for c in channels} for i in (1, 3)]
    return {"native_mean_mae_bytes": baseline, "scene_error_ratios": ratios,
            "bounded_colour_improvement_pass": all(math.isfinite(value) and 0 <= value <= .5 for ratio in ratios for value in ratio.values()),
            "full_colour_parity_verified": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(read_ppm(args.directory / "main.ppm"), read_ppm(args.directory / "pip.ppm"))
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text + "\n")
    print(text)
