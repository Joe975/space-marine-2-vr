"""Fail-closed input-routing gates for holster_support_ownership.json.

Requires primary confirmed before queueing. This does not infer native weapon
switch success from a requested D-pad pulse; inspect native loadout separately.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import vr_test_driver


def incomplete_prefix(line, error):
    """Recognize decoder failures at a valid JSON prefix, never arbitrary damage."""
    value = line.rstrip(" \t")
    if not value.startswith("{") or line.endswith(("\n", "\r")):
        return False
    if error.msg == "Unterminated string starting at" or error.pos == len(value):
        return True
    tail = value[error.pos:]
    if error.msg == "Expecting value" and tail in ("t", "tr", "tru", "f", "fa", "fal", "fals", "n", "nu", "nul", "-"):
        return True
    # raw_decode accepts the complete part of a cut number, then encounters its
    # unfinished fractional/exponent suffix where it expected a delimiter.
    return (error.msg == "Expecting ',' delimiter" and error.pos > 0
            and value[error.pos-1].isdigit() and tail in (".", "e", "E", "e+", "e-", "E+", "E-"))


def read_trace(text, allow_live_tail=False):
    records = []
    lines = text.splitlines(keepends=True)
    discarded = 0
    for index, line in enumerate(lines):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            if allow_live_tail and index == len(lines)-1 and incomplete_prefix(line, error):
                discarded += 1
                continue
            raise ValueError(f"invalid trace record at line {index+1}: {error.msg}") from error
        if not isinstance(record, dict):
            raise ValueError(f"trace record at line {index+1} is not an object")
        records.append(record)
    return records, {"complete_records": len(records), "incomplete_final_records": discarded,
                     "allow_live_tail": allow_live_tail}


def check(records, request_id):
    errors = []
    base = vr_test_driver.report_trace((json.dumps(r) for r in records), request_id)
    if base.get("error") or base.get("terminal") != "completed":
        return {"routing_pass": False, "errors": [base.get("error", "driver did not complete")],
                "native_switch_verified": False}
    marks = base["marks"]
    start = next(r["t"] for r in marks if r["detail"].split()[1] == "running")
    end = next(r["t"] for r in marks if r["detail"].split()[1] == "completed")
    if not 15.8 <= end-start <= 16.3:
        errors.append("unexpected scenario duration")
    samples = [r for r in records if r.get("type") == "sample" and start <= r["t"] <= end]
    actors = set()
    for r in samples:
        i, c = r.get("input"), r.get("character")
        if not i or not c or i.get("age_ms", 101) > 100 or c.get("age_ms", 101) > 100 or not i.get("armed"):
            errors.append("stale or unarmed observations")
            break
        actors.add(c["actor"])
    if len(actors) != 1:
        errors.append("actor changed or absent")
    phases = (("support", 1.2, 1.8), ("support_release_held", 2.7, 3.3),
              ("rev", 7.2, 7.8), ("trigger_release_held", 8.7, 9.3),
              ("outstretched", 13.2, 13.8))
    counts = {}
    for name, low, high in phases:
        window = [r for r in samples if low <= r["t"]-start <= high]
        counts[name] = len(window)
        times = [start+low, *(r["t"] for r in window), start+high]
        if len(window) < 8 or max(b-a for a, b in zip(times, times[1:])) > .15:
            errors.append(name+": insufficient continuous observations")
        for r in window:
            i = r.get("input") or {}
            pad = i.get("pad") or {}
            raw = i.get("raw_controls") or {}
            if raw.get("right_grip", 0) < .9:
                errors.append(name+": requested right squeeze not observed")
                break
            if pad.get("buttons") != 0:
                errors.append(name+": unintended native button")
                break
            if name == "support" and (not i.get("support_aim") or pad.get("lt") != 255):
                errors.append("support did not own native aim")
                break
            if name == "rev" and (pad.get("rt") != 0 or (i.get("controls") or {}).get("chainsword_rev", 0) < .7):
                errors.append("rev loadout/routing not observed")
                break
            if name == "outstretched" and i.get("holster_zone") != 0:
                errors.append("distant right hand classified as holster")
                break
    selections = [(r["t"]-start, r.get("slot")) for r in records
                  if r.get("event") == "holster_select" and start <= r["t"] <= end]
    if len(selections) != 2 or not (4.4 <= selections[0][0] <= 4.7 and selections[0][1] == "secondary"
                                           and 10.4 <= selections[1][0] <= 10.7 and selections[1][1] == "primary"):
        errors.append("expected exactly the two deliberate holster selections")
    for low, high in ((4.5, 4.6), (10.5, 10.6)):
        if not any((r.get("input") or {}).get("pad", {}).get("buttons", 0) & 2
                   for r in samples if low <= r["t"]-start <= high):
            errors.append("missing native weapon-cycle pulse")
    return {"routing_pass": not errors, "errors": sorted(set(errors)), "phases": counts,
            "selections": selections, "native_switch_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--id", required=True)
    parser.add_argument("--allow-live-tail", action="store_true",
                        help="allow one truncated final JSON prefix without a line terminator; report its exclusion")
    args = parser.parse_args()
    try:
        records, receipt = read_trace(args.trace.read_text(encoding="utf-8-sig"), args.allow_live_tail)
    except ValueError as error:
        print(json.dumps({"routing_pass": False, "errors": [str(error)], "native_switch_verified": False}, indent=2))
        return 1
    result = check(records, args.id)
    result["trace_read"] = receipt
    print(json.dumps(result, indent=2))
    return 0 if result["routing_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
