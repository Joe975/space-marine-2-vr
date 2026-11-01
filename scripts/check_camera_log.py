"""Check passive camera evidence; no game interaction or third-party dependencies."""
import argparse
import json
import math
import re
from pathlib import Path


def fields(line):
    return dict(re.findall(r"(\w+)=(\[[^\]]*\]|\S+)", line))


def vector(value):
    return [float(v) for v in value.strip("[]").split(",")]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def check_log(text, since="00:00:00", require_motion=False):
    """Validate fresh gameplay samples and count matching asynchronous engine snapshots.

    Menu/loading data is excluded by FOV and sample age. Snapshots are sampled separately,
    so movement can cause a legitimate mismatch; require some matches, not all matches.
    """
    samples = []
    current = None
    counters = None
    for line in text.splitlines():
        if not line.startswith("[") or line[1:9] < since:
            continue
        if "StreamlineProbe: state=" in line:
            counters = fields(line)
        elif "StreamlineProbe: frame=" in line:
            current = {"camera": fields(line), "projection": {}, "engine": None}
            samples.append(current)
        elif current is not None:
            match = re.search(r"StreamlineProbe: projection\[(\d)\]=(\[[^\]]+\])", line)
            if match:
                current["projection"][int(match[1])] = vector(match[2])
            elif "EngineCameraProbe: caller=0x18033bf calls=" in line:
                current["engine"] = fields(line)

    if counters is None or int(counters["invalid"]) or int(counters["rejected"]):
        raise ValueError("Missing counters or invalid/rejected Streamline calls")
    fresh = [s for s in samples if float(s["camera"]["age"].rstrip("s")) <= 0.1
             and float(s["camera"]["fov_rad"]) > 0.2]
    if not fresh:
        raise ValueError("No fresh gameplay camera samples")
    matches = 0
    positions, forwards = [], []
    for s in fresh:
        c, p, e = s["camera"], s["projection"], s["engine"]
        pos, right, up, fwd = (vector(c[k]) for k in ("pos", "right", "up", "fwd"))
        if any(len(v) != 3 or not all(math.isfinite(x) for x in v) for v in (pos, right, up, fwd)):
            raise ValueError("Nonfinite or malformed camera vectors")
        axes = (right, up, fwd)
        if any(abs(dot(a, b) - (i == j)) > 1e-4
               for i, a in enumerate(axes) for j, b in enumerate(axes)):
            raise ValueError("Camera basis is not orthonormal")
        near, far, fov, aspect = (float(c[k]) for k in ("near", "far", "fov_rad", "aspect"))
        if not (0 < near < far and 0 < fov < math.pi and aspect > 0
                and c["depth_inverted"] == "1" and c["viewport"] == "0" and c["result"] == "0"):
            raise ValueError("Unexpected main camera projection metadata")
        expected = {0: [1 / (aspect * math.tan(fov / 2)), 0, 0, 0],
                    1: [0, 1 / math.tan(fov / 2), 0, 0],
                    2: [0, 0, near / (near - far), 1],
                    3: [0, 0, -near * far / (near - far), 0]}
        if set(p) != set(expected) or any(len(p[i]) != 4 or
                any(not math.isclose(x, y, rel_tol=1e-4, abs_tol=1e-6)
                    for x, y in zip(p[i], expected[i])) for i in expected):
            raise ValueError("Projection disagrees with FOV/aspect/reversed-depth planes")
        if e is not None and float(e["age"].rstrip("s")) <= 0.1:
            engine_vectors = [vector(e[key]) for key in ("pos", "row0", "row1", "row2")]
            same_vectors = all(len(a) == 3 and all(math.isfinite(x) and
                               math.isclose(x, y, rel_tol=0, abs_tol=1e-4)
                               for x, y in zip(a, b))
                               for a, b in zip(engine_vectors, (pos, right, up, fwd)))
            same_lens = all(math.isclose(float(e[key]), value, rel_tol=1e-5, abs_tol=1e-5)
                            for key, value in (("near", near), ("far", far), ("fov_v", math.degrees(fov))))
            matches += same_vectors and same_lens and e["index"] == "0"
        positions.append(pos)
        forwards.append(fwd)
    if not matches:
        raise ValueError("No matching fresh main-view engine/Streamline sample")
    displacement = max(math.dist(p, positions[0]) for p in positions)
    basis_change = max(math.dist(f, forwards[0]) for f in forwards)
    if require_motion and not (displacement > 0.01 and basis_change > 0.01):
        raise ValueError("Gameplay position and basis did not both change")
    return {"fresh_gameplay_samples": len(fresh), "matching_engine_samples": matches,
            "position_change_from_first": displacement, "forward_change_from_first": basis_change,
            "invalid": int(counters["invalid"]), "rejected": int(counters["rejected"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--since", default="00:00:00", help="HH:MM:SS in this run")
    parser.add_argument("--require-motion", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(check_log(args.log.read_text(encoding="utf-8"), args.since,
                                   args.require_motion), indent=2))
    except (ValueError, KeyError) as error:
        parser.exit(1, f"Camera check failed: {error}\n")


if __name__ == "__main__":
    main()
