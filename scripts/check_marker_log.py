"""Read-only check of native UI marker coordinates against submitted scene projection."""
import argparse
import json
import math
from pathlib import Path
import re

NUMBER = r"[^,\]\s]+"
ROW = re.compile(
    rf"WorldMarkerProjection: caller=(0x[0-9a-f]+) render_id=(\d+) eye=(\d+) "
    rf"ui_uv=\[({NUMBER}),({NUMBER})\] scene_uv=\[({NUMBER}),({NUMBER})\] "
    rf"error=\[({NUMBER}),({NUMBER})\] depth=({NUMBER})", re.IGNORECASE
)


def check(text, maximum_error=0.0001, minimum_samples=4):
    if not math.isfinite(maximum_error) or maximum_error <= 0 or minimum_samples < 1:
        raise ValueError("positive finite error bound and sample count required")
    eyes, callers, identities, errors = set(), set(), set(), []
    failures = []
    for match in ROW.finditer(text):
        caller, render_id, eye = (int(match[i], 16 if i == 1 else 10) for i in (1, 2, 3))
        coordinates = list(map(float, match.groups()[3:]))
        if render_id <= 0 or eye not in (0, 1) or not all(map(math.isfinite, coordinates)):
            failures.append("invalid frame, eye or nonfinite coordinates")
            continue
        u, v, scene_u, scene_v, reported_u, reported_v, depth = coordinates
        actual = abs(u - scene_u), abs(v - scene_v)
        if abs(depth) <= 0.00001:
            failures.append("zero-depth projection sample")
        if max(actual) > maximum_error or max(reported_u, reported_v) > maximum_error:
            failures.append(f"projection differs at render {render_id} eye {eye}: {actual}")
        if reported_u < 0 or reported_v < 0 or any(abs(a-b) > 0.00001 for a, b in zip(actual, (reported_u, reported_v))):
            failures.append("reported error does not match actual coordinates")
        eyes.add(eye)
        callers.add(caller)
        identities.add((caller, render_id, eye))
        errors.extend(actual)
    if len(identities) < minimum_samples:
        failures.append(f"need {minimum_samples} distinct samples; found {len(identities)}")
    if eyes != {0, 1}:
        failures.append("both eyes need measured projection samples")
    if not {0xBF3D8C, 0xBF497A}.issubset(callers):
        failures.append("both 2D and 3D marker paths need measured samples")
    return {"passed": not failures, "samples": len(identities), "eyes": sorted(eyes),
            "callers": [hex(v) for v in sorted(callers)], "max_uv_error": max(errors, default=None),
            "failures": failures}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--max-error", type=float, default=0.0001)
    parser.add_argument("--minimum-samples", type=int, default=4)
    args = parser.parse_args()
    result = check(args.log.read_text(encoding="utf-8", errors="replace"), args.max_error, args.minimum_samples)
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
