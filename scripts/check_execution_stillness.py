"""Replay the viewer play's stillness rule on executions in a playtest trace (docs/VIEWER_MODE.md).

Usage:
  python scripts/check_execution_stillness.py TRACE.jsonl [--units 0.9] [--speed 0.35] [--still 0.5]

For each right-stick click (RS_finisher) followed by a takeover within 1.2 s (an execution), reports
when the rule "root, HEAD and both HAND bones under --speed m/s for --still s" first held after the
press (looked for from 0.8 s on, up to the player's next stick push), when the Marine stopped (the
last faster interval before that), and when the player pushed the stick. The rule should hold
0.5-0.65 s after the Marine stops; executions the player cut short with the stick show no rule time.
Read the rule times against the stop times by eye: a rule time well before the execution's visible
end means the rule fired mid-execution. Speeds use the trace's character samples (native animation)
over at least 50 ms, as vr_viewer::Motion does.
"""
import argparse
import json
import math
import sys

INTERVAL = 0.05  # seconds; vr_viewer::Motion::INTERVAL
GAP = 0.3        # seconds; vr_viewer::Motion::GAP


def speeds(samples, units):
    """[(t, fastest m/s)] from [(t, [root, head, left, right])], each over at least INTERVAL."""
    out, ref = [], None
    for t, points in samples:
        if ref is None or t - ref[0] > GAP:
            ref = (t, points)
            continue
        if t - ref[0] < INTERVAL:
            continue
        pairs = [(a, b) for a, b in zip(points, ref[1]) if a is not None and b is not None]
        if pairs:
            out.append((t, max(math.dist(a, b) for a, b in pairs) / units / (t - ref[0])))
        ref = (t, points)
    return out


def rule_time(rates, start, speed, still):
    """First time after start when every rate has stayed at or under speed for still seconds."""
    quiet = start
    for t, v in rates:
        if t < start:
            continue
        if v > speed:
            quiet = t
        elif t - quiet >= still:
            return t
    return None


def read(path):
    samples, presses, modes, sticks = [], [], [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                d = json.loads(line)
            except ValueError:
                continue
            t = d.get("t")
            if d.get("type") == "event":
                if d.get("event") == "button" and d.get("name") == "RS_finisher" and d.get("down"):
                    presses.append(t)
                elif d.get("event") == "camera_mode":
                    modes.append((t, d.get("mode")))
                continue
            c = d.get("character")
            if d.get("type") != "sample" or not c or not c.get("root"):
                continue
            samples.append((t, [c.get("root"), c.get("head"), c.get("left_native"), c.get("right_native")]))
            pad = (d.get("input") or {}).get("pad") or {}
            sticks.append((t, math.hypot(pad.get("lx", 0), pad.get("ly", 0)) / 32767))
    return samples, presses, modes, sticks


def report(samples, presses, modes, sticks, units, speed, still):
    rows = []
    for press in presses:
        if not any(press <= t <= press + 1.2 and m == "takeover" for t, m in modes):
            continue
        stick = next((t for t, v in sticks if t > press + 0.3 and v > 0.3), None)
        end = stick if stick is not None else press + 15
        rates = speeds([s for s in samples if press <= s[0] <= end], units)
        rule = rule_time(rates, press + 0.8, speed, still)
        moving = [t for t, v in rates if v > speed and (rule is None or t <= rule)]
        stopped = moving[-1] if moving and rule is not None else None
        rows.append({"press": press, "stopped": stopped and stopped - press, "rule": rule and rule - press,
                     "stick": stick and stick - press})
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace")
    parser.add_argument("--units", type=float, default=0.9, help="world units per metre (life size)")
    parser.add_argument("--speed", type=float, default=0.35)
    parser.add_argument("--still", type=float, default=0.5)
    args = parser.parse_args(argv)
    rows = report(*read(args.trace), args.units, args.speed, args.still)
    fmt = lambda v: "-" if v is None else f"+{v:.2f}"
    print("finisher   stopped  rule    stick")
    for r in rows:
        print(f"{r['press']:9.2f}  {fmt(r['stopped']):7}  {fmt(r['rule']):6}  {fmt(r['stick'])}")
    print(f"{len(rows)} executions, {sum(r['rule'] is not None for r in rows)} settled by the rule before the stick moved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
