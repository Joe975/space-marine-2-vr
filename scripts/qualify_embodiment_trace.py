"""Fail closed on actual publication evidence from the embodiment driver scenario.

Use --start/--end trace times to select one completed driver request. The driver
terminal status must also be checked separately: this script cannot prove focus
ownership or completion merely from a surviving subset of trace samples.
"""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path


def qualify(records, start=0.0, end=math.inf, minimum=30):
    errors = []
    samples = leaning = extended = 0
    worst_lean = worst_wrist = worst_armour = 0.0
    for s in records:
        if s.get("type") != "sample" or not start <= s["t"] <= end:
            continue
        c, i, g = s.get("character"), s.get("input"), s.get("game", {})
        if not c or not i or not c.get("driven") or not i.get("armed") or g.get("paused"):
            continue
        if max(c.get("age_ms", math.inf), i.get("age_ms", math.inf)) > 100:
            continue
        units = c.get("units_per_metre", 0)
        if not math.isfinite(units) or units <= 0:
            errors.append("invalid units")
            continue
        samples += 1
        lean = c.get("lean_degrees", {})
        if lean.get("native", 0) > 30:
            leaning += 1
            final = lean.get("final")
            if final is None or not math.isfinite(final):
                errors.append("missing published lean")
            else:
                worst_lean = max(worst_lean, final)
        body = {b["name"]: b for b in c.get("body", [])}
        for hand, side in (("left", "L"), ("right", "R")):
            try:
                upper, elbow, armour = (body[n] for n in ("ARM1" + side, "ARM2" + side, "forearm_" + side))
                native, solved, target = (c[hand + n] for n in ("_native", "_solved", "_target"))
                vectors = (upper["native"], upper["final"], elbow["native"], armour["native"], armour["final"], native, solved, target)
                if any(len(v) != 3 or any(not math.isfinite(x) for x in v) for v in vectors):
                    raise ValueError("invalid endpoint vector")
                length = math.dist(upper["native"], elbow["native"]) + math.dist(elbow["native"], native)
                if math.dist(upper["final"], target) > length + .005 * units:
                    extended += 1
                worst_wrist = max(worst_wrist, math.dist(solved, target) / units)
                # Preserve the measured native endpoint offset rather than guessing
                # a model or world scale. Palm rotation need not rotate the bracer.
                native_gap = math.dist(armour["native"], native)
                final_gap = math.dist(armour["final"], solved)
                worst_armour = max(worst_armour, abs(final_gap - native_gap) / units)
            except (KeyError, TypeError, ValueError):
                errors.append("missing arm endpoint publication")
    if samples < minimum:
        errors.append("insufficient fresh driven samples")
    if leaning < minimum:
        errors.append("insufficient native lean beyond old 30-degree cap")
    if extended < minimum:
        errors.append("insufficient targets beyond native arm reach")
    if worst_lean > 2:
        errors.append("published lean exceeds 2 degrees")
    if worst_wrist > .005:
        errors.append("wrist residual exceeds 5 mm")
    if worst_armour > .01:
        errors.append("armour endpoint offset changes by more than 10 mm")
    return dict(accepted=not errors, errors=sorted(set(errors)), samples=samples,
                leaning_samples=leaning, extended_hands=extended,
                max_published_lean_degrees=worst_lean,
                max_wrist_error_metres=worst_wrist,
                max_armour_offset_change_metres=worst_armour)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("trace", type=Path)
    p.add_argument("--start", type=float, default=0)
    p.add_argument("--end", type=float, default=math.inf)
    p.add_argument("--minimum", type=int, default=30)
    args = p.parse_args()
    records, malformed = [], 0
    for line in args.trace.read_text(encoding="utf-8").splitlines():
        try:
            records.append(json.loads(line))
        except ValueError:
            malformed += 1
    result = qualify(records, args.start, args.end, args.minimum)
    result["malformed_records"] = malformed
    # Retain the count, while excluding a partially-written writer line. The
    # scenario sample-count gates still have to pass on complete records.
    print(json.dumps(result, indent=2))
    return 0 if result["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
