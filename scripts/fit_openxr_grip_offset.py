"""Fit the per-hand OpenXR grip -> raw OpenVR controller offset (docs/OPENXR_MIGRATION_PLAN.md, phase 0).

Inputs are two CSVs with the same layout (qpc,hand,m0..m15 in the mod's row-vector matrix order):
  --witness  scripts/openvr_pose_witness.cpp output (raw OpenVR origins, background client)
  --grips    the mod's SM2VR_OPENXR_GRIP_LOG (OpenXR grip poses before conversion)
For every grip sample the witness pose is interpolated at the same QPC. With row vectors
grip = grip_in_raw * raw, so each pair gives grip_in_raw = grip * inverse(raw); the fit is
their mean. Writes config/openxr_index_grip.calibration (measured=1 only if the gate passes).

Run: .venv/Scripts/python.exe -I scripts/fit_openxr_grip_offset.py --witness W.csv --grips G.csv --out config/openxr_index_grip.calibration
"""
import argparse
import csv
import math
import sys
from pathlib import Path

import numpy as np

GATE_POSITION_MM = 2.0
GATE_ANGLE_DEG = 0.5
GATE_MIN_PAIRS = 200


def load(path):
    out = {0: [], 1: []}
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            hand = int(row["hand"])
            if hand not in out:
                raise ValueError(f"{path}: hand must be 0 or 1")
            matrix = np.array([float(row[f"m{n}"]) for n in range(16)]).reshape(4, 4)
            out[hand].append((int(row["qpc"]), matrix))
    for samples in out.values():
        samples.sort(key=lambda item: item[0])
    return out


def to_quaternion(rotation):
    """Row-vector 3x3 rotation -> unit quaternion (w, x, y, z) of the column-vector matrix."""
    c = rotation.T
    trace = c[0, 0] + c[1, 1] + c[2, 2]
    if trace > 0:
        s = 2 * math.sqrt(trace + 1)
        q = np.array([0.25 * s, (c[2, 1] - c[1, 2]) / s, (c[0, 2] - c[2, 0]) / s, (c[1, 0] - c[0, 1]) / s])
    else:
        i = int(np.argmax([c[0, 0], c[1, 1], c[2, 2]]))
        j, k = (i + 1) % 3, (i + 2) % 3
        s = 2 * math.sqrt(1 + c[i, i] - c[j, j] - c[k, k])
        q = np.zeros(4)
        q[0] = (c[k, j] - c[j, k]) / s
        q[1 + i] = 0.25 * s
        q[1 + j] = (c[j, i] + c[i, j]) / s
        q[1 + k] = (c[k, i] + c[i, k]) / s
    return q / np.linalg.norm(q)


def from_quaternion(q):
    w, x, y, z = q / np.linalg.norm(q)
    c = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    return c.T


def slerp(a, b, t):
    if np.dot(a, b) < 0:
        b = -b
    dot = min(1.0, float(np.dot(a, b)))
    if dot > 0.9995:
        q = a + t * (b - a)
        return q / np.linalg.norm(q)
    theta = math.acos(dot)
    return (math.sin((1 - t) * theta) * a + math.sin(t * theta) * b) / math.sin(theta)


def pose(rotation, translation):
    m = np.eye(4)
    m[:3, :3] = rotation
    m[3, :3] = translation
    return m


def interpolate(samples, qpc, max_gap):
    times = [t for t, _ in samples]
    index = int(np.searchsorted(times, qpc))
    if index == 0 or index >= len(samples):
        return None
    (t0, a), (t1, b) = samples[index - 1], samples[index]
    if t1 - t0 > max_gap:
        return None
    t = (qpc - t0) / (t1 - t0)
    rotation = from_quaternion(slerp(to_quaternion(a[:3, :3]), to_quaternion(b[:3, :3]), t))
    return pose(rotation, (1 - t) * a[3, :3] + t * b[3, :3])


def compose_degrees(degrees):
    """Same as openxr_hands::GripOffset::rotation / EngineArmDriver::grip (row vectors, X then Y then Z)."""
    out = np.eye(3)
    for i, value in enumerate(degrees):
        a = math.radians(value)
        r = np.eye(3)
        j, k = (i + 1) % 3, (i + 2) % 3
        r[j, j] = r[k, k] = math.cos(a)
        r[j, k] = math.sin(a)
        r[k, j] = -math.sin(a)
        out = out @ r
    return out


def decompose_degrees(rotation):
    # rotation = Rx * Ry * Rz: [0][2] = -sin y, [0][0] = cy cz, [0][1] = cy sz, [1][2] = sx cy, [2][2] = cx cy.
    y = math.asin(max(-1.0, min(1.0, -rotation[0, 2])))
    return [math.degrees(math.atan2(rotation[1, 2], rotation[2, 2])), math.degrees(y),
            math.degrees(math.atan2(rotation[0, 1], rotation[0, 0]))]


def fit_hand(witness, grips, max_gap):
    offsets = []
    pairs = []
    for qpc, grip in grips:
        raw = interpolate(witness, qpc, max_gap)
        if raw is None:
            continue
        offsets.append(grip @ np.linalg.inv(raw))
        pairs.append((grip, raw))
    if not offsets:
        return None
    quaternions = np.array([to_quaternion(o[:3, :3]) for o in offsets])
    # Mean rotation: principal eigenvector of the summed outer products (sign-invariant).
    values, vectors = np.linalg.eigh(quaternions.T @ quaternions)
    mean = pose(from_quaternion(vectors[:, int(np.argmax(values))]), np.mean([o[3, :3] for o in offsets], axis=0))
    position, angle = [], []
    for grip, raw in pairs:
        predicted = mean @ raw
        position.append(1000 * float(np.linalg.norm(predicted[3, :3] - grip[3, :3])))
        delta = predicted[:3, :3] @ grip[:3, :3].T
        angle.append(math.degrees(math.acos(max(-1.0, min(1.0, (np.trace(delta) - 1) / 2)))))
    return {
        "offset": mean,
        "pairs": len(pairs),
        "position_mm": (float(np.median(position)), float(np.percentile(position, 95))),
        "angle_deg": (float(np.median(angle)), float(np.percentile(angle, 95))),
    }


def passes(result):
    return (result is not None and result["pairs"] >= GATE_MIN_PAIRS and
            result["position_mm"][0] < GATE_POSITION_MM and result["angle_deg"][0] < GATE_ANGLE_DEG)


def calibration_text(results, measured, source):
    lines = ["# OpenXR /input/grip/pose -> raw OpenVR controller origin, per hand (OpenXR backend only).",
             "# Values are the grip frame expressed in the raw device frame: metres, XYZ degrees.",
             f"# Fitted by scripts/fit_openxr_grip_offset.py from {source}."]
    for hand, name in ((0, "left"), (1, "right")):
        r = results[hand]
        lines.append(f"# {name}: {r['pairs']} pairs, position median/p95 {r['position_mm'][0]:.2f}/{r['position_mm'][1]:.2f} mm, "
                     f"angle {r['angle_deg'][0]:.3f}/{r['angle_deg'][1]:.3f} deg")
    for hand, name in ((0, "left"), (1, "right")):
        offset = results[hand]["offset"]
        lines.append(f"{name}_grip_in_raw_metres=" + " ".join(f"{v:.6f}" for v in offset[3, :3]))
        lines.append(f"{name}_grip_in_raw_degrees=" + " ".join(f"{v:.4f}" for v in decompose_degrees(offset[:3, :3])))
    lines.append(f"measured={1 if measured else 0}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--witness", required=True, type=Path)
    parser.add_argument("--grips", required=True, type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--qpc-frequency", type=float, default=10_000_000, help="QPC ticks per second (the witness prints it)")
    parser.add_argument("--max-gap-ms", type=float, default=25, help="witness polls at the ~15.6 ms Windows timer tick")
    args = parser.parse_args(argv)
    witness, grips = load(args.witness), load(args.grips)
    max_gap = args.max_gap_ms * args.qpc_frequency / 1000
    results = {hand: fit_hand(witness[hand], grips[hand], max_gap) for hand in (0, 1)}
    for hand, result in results.items():
        if result is None:
            print(f"hand {hand}: no overlapping samples", file=sys.stderr)
            return 1
        print(f"hand {hand}: {result['pairs']} pairs, position mm median/p95 {result['position_mm']}, angle deg {result['angle_deg']}")
    measured = all(passes(r) for r in results.values())
    print("gate:", "PASS" if measured else "FAIL (written with measured=0)")
    if args.out:
        args.out.write_text(calibration_text(results, measured, f"{args.witness.name} + {args.grips.name}"))
    return 0 if measured else 1


if __name__ == "__main__":
    sys.exit(main())
