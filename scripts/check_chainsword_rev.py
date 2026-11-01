"""Check a config/test_driver/chainsword_rev_default.json run against the game trace.

Reads the trace's routed controls, native pad and the native rev block (trace key
"chainsword_rev": installed, active, starts/stops, play_rate, gain) per scripted phase:
a resting finger must not rev, a pull starts exactly one rev whose teeth speed follows the
trigger, easing off keeps it, resting again releases it, and neither the left-trigger pistol
shot nor the rifle's right-trigger shot revs.

  .venv/Scripts/python.exe -I scripts/check_chainsword_rev.py TRACE.jsonl REQUEST_ID
"""
import argparse
import json
import math
from pathlib import Path

# Mirrors EngineChainswordRev.h (DEADZONE, MIN_RATE, MAX_RATE) and EngineChainswordAudio::gain.
DEADZONE, MIN_RATE, MAX_RATE, MIN_GAIN = .05, .35, 2.5, .2
RT_RB, RT_DPAD_UP = 512, 1


def rev_amount(force):
    return min(max((force - DEADZONE) / (1 - DEADZONE), 0.0), 1.0)


def rev_rate(force):
    s = rev_amount(force)
    return MIN_RATE + (MAX_RATE - MIN_RATE) * s * math.sqrt(s)


# name, start s, end s, scripted right trigger, expect revving (None = not checked)
PHASES = (("rest_before", 4.6, 6.9, .08, False), ("pull", 7.6, 9.9, .3, True), ("full", 10.6, 12.9, 1.0, True),
          ("eased", 13.6, 15.9, .12, True), ("rest_after", 16.6, 18.9, .08, False),
          ("pistol", 20.4, 21.4, 0.0, False), ("rifle", 27.0, 28.4, .8, False))


def window(records, request_id):
    marks = [r for r in records if r.get("event") == "test_driver" and r.get("detail", "").startswith(request_id + " ")]
    start = next((r["t"] for r in marks if r["detail"].split()[1] == "running"), None)
    end = next((r["t"] for r in marks if r["detail"].split()[1] == "completed"), None)
    return start, end


def check(records, request_id, min_rows=10):
    start, end = window(records, request_id)
    if start is None or end is None:
        return {"passed": False, "errors": ["driver did not complete"]}
    rows = [(r["t"] - start, r) for r in records if r.get("type") == "sample" and start <= r["t"] <= end
            and r.get("input") and r["input"].get("armed") and r["input"].get("age_ms", 101) <= 100]
    revs = [r["chainsword_rev"] for _, r in rows if r.get("chainsword_rev")]
    if not revs or not revs[0].get("installed"):
        return {"passed": False, "errors": ["native rev hook not installed (launch without -NoChainswordRev)"]}
    errors, summary = [], {}
    for name, low, high, force, revving in PHASES:
        phase = [r for t, r in rows if low <= t <= high and r.get("chainsword_rev")]
        if len(phase) < min_rows:
            errors.append(f"{name}: {len(phase)} fresh observations")
            continue
        native = [r["chainsword_rev"] for r in phase]
        controls = [r["input"].get("controls", {}) for r in phase]
        pads = [r["input"]["pad"] for r in phase]
        loadouts = {r["input"].get("loadout") for r in phase}
        active = sum(n["active"] for n in native)
        summary[name] = {"samples": len(phase), "active": active, "loadout": sorted(map(str, loadouts)),
                         "play_rate": [round(min(n["play_rate"] for n in native), 3), round(max(n["play_rate"] for n in native), 3)],
                         "gain": [round(min(n["gain"] for n in native), 3), round(max(n["gain"] for n in native), 3)]}
        expected_loadout = "primary" if name == "rifle" else "secondary"
        if loadouts != {expected_loadout}:
            errors.append(f"{name}: loadout {sorted(map(str, loadouts))}, expected {expected_loadout}")
        if revving and active != len(phase):
            errors.append(f"{name}: revving in {active}/{len(phase)} samples, expected all")
        if not revving and active:
            errors.append(f"{name}: revving in {active}/{len(phase)} samples, expected none")
        if revving:
            want = rev_rate(force)
            if any(abs(n["play_rate"] - want) > .03 for n in native):
                errors.append(f"{name}: play_rate {summary[name]['play_rate']}, expected {want:.3f}")
            gain = MIN_GAIN + (1 - MIN_GAIN) * rev_amount(force)
            if any(n["gain"] >= 0 and abs(n["gain"] - gain) > .03 for n in native):
                errors.append(f"{name}: gain {summary[name]['gain']}, expected {gain:.3f}")
        if name in ("rest_before", "rest_after", "pull", "full", "eased"):
            if any(abs(c.get("chainsword_rev", 0) - force) > .01 for c in controls):
                errors.append(f"{name}: routed rev request is not the right trigger ({force})")
            if any(p["rt"] or p["lt"] or p["buttons"] & (RT_RB | RT_DPAD_UP) for p in pads):
                errors.append(f"{name}: rev phase emitted a native trigger or melee button")
        if name in ("pistol", "rifle"):
            if not any(p["rt"] >= 200 for p in pads):
                errors.append(f"{name}: no native fire (RT)")
            if any(c.get("chainsword_rev", 0) for c in controls):
                errors.append(f"{name}: firing also requested a rev")
    first, last = revs[0], revs[-1]
    summary["native_starts"] = last["starts"] - first["starts"]
    summary["native_stops"] = last["stops"] - first["stops"]
    if summary["native_starts"] != 1 or summary["native_stops"] != 1:
        errors.append(f"expected one native start and one stop, got {summary['native_starts']}/{summary['native_stops']}")
    return {"passed": not errors, "errors": errors, "phases": summary}


def load(path):
    records = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass  # A killed game can leave a truncated last line.
    return records


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace", type=Path)
    parser.add_argument("request_id")
    args = parser.parse_args()
    result = check(load(args.trace), args.request_id)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
