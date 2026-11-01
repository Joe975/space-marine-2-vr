"""Check committed body-placement positions over one active driver interval.

This is a numeric publication gate, not a visual/comfort/foot-contact acceptance.
Select only the active request with --start/--end. Check terminal driver receipts
and BodyRenderPlacement commit/rejection logs separately: current trace format
does not export placement counters or full body rotation matrices.
"""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path

SHIFTED = ("CENTRE", "FOOT1L", "FOOT1R")
UNCHANGED = ("Trajectory", "cam_root", "foot_targ_L", "foot_targ_R")
WRISTS = ("left_wrist", "right_wrist")


def number(value):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def vector(value):
    if not isinstance(value, (list, tuple)) or len(value) != 3 or not all(number(x) for x in value):
        raise ValueError("missing or non-finite 3D position")
    return value


def read_trace(path, allow_live_tail=False):
    """Only an explicitly allowed incomplete final object may be omitted."""
    def constant(value):
        raise ValueError("non-finite JSON constant: " + value)

    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key: " + key)
            result[key] = value
        return result

    records, ignored = [], 0
    lines = Path(path).read_bytes().splitlines(keepends=True)
    for line_number, raw in enumerate(lines, 1):
        if not raw.strip():
            continue
        try:
            text = raw.decode("utf-8")
            record = json.loads(text, parse_constant=constant, object_pairs_hook=unique)
        except json.JSONDecodeError as error:
            if (allow_live_tail and line_number == len(lines) and not raw.endswith((b"\n", b"\r"))
                    and raw.lstrip().startswith(b"{") and not raw.rstrip().endswith((b"}", b"]"))):
                ignored = 1
                continue
            raise ValueError(f"line {line_number}: malformed JSON ({error.msg})") from error
        except (UnicodeError, ValueError) as error:
            raise ValueError(f"line {line_number}: {error}") from error
        if not isinstance(record, dict) or record.get("type") not in ("header", "event", "sample"):
            raise ValueError(f"line {line_number}: unknown trace record")
        if record["type"] == "sample" and not number(record.get("t")):
            raise ValueError(f"line {line_number}: missing or invalid sample time")
        records.append(record)
    return records, dict(complete_records=len(records), incomplete_live_tail_ignored=ignored)


def qualify(records, start=0.0, end=math.inf, minimum=30, back_metres=.2,
            offset_tolerance=.005, unchanged_tolerance=.002, wrist_tolerance=.005,
            maximum_gap=.25):
    if (not number(start) or start < 0 or not (number(end) or end == math.inf) or end <= start
            or not isinstance(minimum, int) or isinstance(minimum, bool) or minimum < 1
            or not number(back_metres) or not 0 < back_metres <= .4
            or any(not number(v) or v <= 0 for v in (offset_tolerance, unchanged_tolerance, wrist_tolerance, maximum_gap))):
        raise ValueError("invalid window, count, displacement or tolerance")
    gates = {name: dict(samples=0, unavailable_samples=0, violations=0,
                         max_error_metres=None, reasons=set()) for name in SHIFTED + UNCHANGED + WRISTS}
    errors, actors, epochs, reasons = set(), set(), set(), {}
    selected = fresh = 0
    first = last = None
    maximum_observed_gap = 0.0
    telemetry = dict(available_samples=0, unavailable_samples=0, violations=0)

    def ineligible(reason):
        reasons[reason] = reasons.get(reason, 0) + 1
        errors.add("ineligible samples in selected interval")

    def unavailable(name, why):
        gates[name]["unavailable_samples"] += 1
        gates[name]["reasons"].add(why)

    def measure(name, error, tolerance):
        gate = gates[name]
        gate["samples"] += 1
        gate["max_error_metres"] = max(gate["max_error_metres"] or 0, error)
        if error > tolerance:
            gate["violations"] += 1

    for row in records:
        if not isinstance(row, dict) or row.get("type") != "sample":
            continue
        time = row.get("t")
        if not number(time):
            errors.add("invalid sample time")
            continue
        if not start <= time <= end:
            continue
        selected += 1
        if last is not None:
            if time <= last:
                errors.add("selected sample times are not strictly increasing")
            maximum_observed_gap = max(maximum_observed_gap, time-last)
        if first is None:
            first = time
        last = time
        c, inp, game = row.get("character"), row.get("input"), row.get("game")
        if not all(isinstance(v, dict) for v in (c, inp, game)):
            ineligible("missing character/input/game")
            continue
        # driven is set only after the mapped write batch commits. Non-null final
        # matrices alone are not proof: the producer computes them before writing.
        if c.get("driven") is not True or inp.get("armed") is not True:
            ineligible("uncommitted or unarmed publication")
            continue
        if game.get("available") is not True or game.get("paused") is not False or game.get("cinematic") is not False:
            ineligible("not confirmed active gameplay")
            continue
        if any(not number(v) or not 0 <= v <= 100 for v in (c.get("age_ms"), inp.get("age_ms"))):
            ineligible("stale or future character/input")
            continue
        actor, epoch, units = c.get("actor"), inp.get("epoch"), c.get("units_per_metre")
        try:
            actor_id = int(actor, 16) if isinstance(actor, str) else 0
        except ValueError:
            actor_id = 0
        if actor_id <= 0 or not isinstance(epoch, int) or isinstance(epoch, bool) or epoch <= 0:
            ineligible("missing actor or input epoch")
            continue
        if not number(units) or not .01 <= units <= 100:
            ineligible("invalid world units per metre")
            continue
        actors.add(format(actor_id, "x"));epochs.add(epoch);fresh += 1
        try:
            axis = vector(c.get("root_z_axis"));length = math.hypot(axis[0], axis[2])
            if length <= 1e-4:
                raise ValueError("undefined level actor forward")
            expected = [-back_metres*axis[0]/length, 0, -back_metres*axis[2]/length]
        except ValueError as error:
            expected = None
            errors.add(str(error))
        body, duplicates = {}, set()
        bones = c.get("body")
        if isinstance(bones, list):
            for bone in bones:
                if not isinstance(bone, dict) or not isinstance(bone.get("name"), str):
                    errors.add("invalid body bone record")
                    continue
                name = bone["name"]
                if name in body:
                    duplicates.add(name)
                body[name] = bone
        for name in SHIFTED + UNCHANGED:
            try:
                if name in duplicates:
                    raise ValueError("duplicate bone")
                if name not in body:
                    raise ValueError("bone not recorded")
                native, final = vector(body[name].get("native")), vector(body[name].get("final"))
                displacement = [(f-n)/units for f, n in zip(final, native)]
                target = expected if name in SHIFTED else [0, 0, 0]
                if target is None:
                    raise ValueError("actor-forward reference unavailable")
                measure(name, math.dist(displacement, target), offset_tolerance if name in SHIFTED else unchanged_tolerance)
            except ValueError as error:
                unavailable(name, str(error))
        for name in WRISTS:
            side = name.split("_")[0]
            try:
                solved, target = vector(c.get(side+"_solved")), vector(c.get(side+"_target"))
                measure(name, math.dist(solved, target)/units, wrist_tolerance)
            except ValueError as error:
                unavailable(name, str(error))
        # Future schema may export cumulative commit telemetry. Require it to be
        # valid if present; absence is explicit and needs separate log inspection.
        placement = c.get("body_placement")
        if placement is None:
            telemetry["unavailable_samples"] += 1
        else:
            telemetry["available_samples"] += 1
            if (not isinstance(placement, dict) or not number(placement.get("back_metres"))
                    or abs(placement["back_metres"]-back_metres) > 1e-5
                    or not isinstance(placement.get("updates"), int) or isinstance(placement["updates"], bool)
                    or placement["updates"] <= 0
                    or ("stage" in placement and placement["stage"] != "committed")
                    or placement.get("refusal", "") != ""
                    or ("applied_back_metres" in placement and (not number(placement["applied_back_metres"])
                        or abs(placement["applied_back_metres"]-back_metres) > 1e-5))
                    or ("control_status" in placement and placement["control_status"] not in ("valid", "disabled"))):
                telemetry["violations"] += 1
                errors.add("placement commit telemetry contradicts candidate")
    if fresh < minimum:
        errors.add("insufficient fresh committed samples")
    if len(actors) != 1:
        errors.add("selected candidate does not have one actor")
    if len(epochs) != 1:
        errors.add("selected candidate does not have one input epoch")
    if maximum_observed_gap > maximum_gap:
        errors.add("selected trace coverage gap")
    for name, gate in gates.items():
        if gate["violations"]:
            gate["status"] = "fail"
        elif gate["unavailable_samples"] or gate["samples"] < minimum:
            gate["status"] = "unavailable"
        else:
            gate["status"] = "pass"
        gate["reasons"] = sorted(gate["reasons"])
        if gate["status"] != "pass":
            errors.add(name + " gate " + gate["status"])
    telemetry["status"] = ("fail" if telemetry["violations"] else "partial" if telemetry["available_samples"] and telemetry["unavailable_samples"]
                           else "available" if telemetry["available_samples"] else "unavailable")
    telemetry["note"] = ("Placement commit counters are recorded; inspect per-publication stage/refusal and native rejection logs alongside these position gates."
                         if telemetry["available_samples"] else
                         "This trace has no placement counters; confirm BodyRenderPlacement commit/rejection logs separately.")
    return dict(accepted=not errors, scope="committed position invariants only; visual/contact/comfort gates remain",
                errors=sorted(errors), selected_samples=selected, fresh_committed_samples=fresh,
                ineligible_reasons=reasons, actors=sorted(actors), epochs=sorted(epochs),
                first_sample=first, last_sample=last, max_gap_seconds=maximum_observed_gap,
                expected_back_metres=back_metres, gates=gates, placement_telemetry=telemetry)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--start", type=float, default=0)
    parser.add_argument("--end", type=float, default=math.inf)
    parser.add_argument("--minimum", type=int, default=30)
    parser.add_argument("--back-metres", type=float, default=.2)
    parser.add_argument("--allow-live-tail", action="store_true", help="Ignore only an incomplete final object without a newline")
    args = parser.parse_args(argv)
    try:
        records, receipt = read_trace(args.trace, args.allow_live_tail)
        result = qualify(records, args.start, args.end, args.minimum, args.back_metres)
        result["reader"] = receipt
    except (OSError, ValueError) as error:
        result = dict(accepted=False, errors=[str(error)])
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
