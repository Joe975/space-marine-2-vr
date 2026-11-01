"""Reconstruct melee from a playtest trace: which hand moved, what the mod sent, what the game did.

For every `swing` event (the mod's physical-swing detector committing a native attack) this
reports, from the 30 Hz samples and the poll-accurate events of sm2vr_trace.jsonl:

* the hand that led the motion: peak head-relative speeds of both wrists and of both
  0.45 m virtual blade tips (the detector's own endpoints) in the 350 ms before commit;
* the native buttons the mod sent: RB (light) or D-pad up (power), the pulse length, and
  whether a held left-hand guard was released and re-pressed (a new LB edge) around it;
* what the native character did: root travel (the attack lunge) and whether a foot rose
  in front of the body (a kick).

It also lists every kick-shaped foot raise in the session with the inputs that preceded it.
Speeds come from 30 Hz samples, so they underestimate the detector's poll-rate peaks.

Usage: analyze_melee_trace.py TRACE [--json]   (default output: a markdown table)
"""
from __future__ import annotations

import argparse
import bisect
import json
import math
import sys
from pathlib import Path

REACH = 0.45  # VrSwingMelee reach_metres candidate: virtual blade tip along controller -Z.
LEAD_WINDOW = (-0.35, 0.10)  # seconds around the commit used for hand speeds
RESPONSE_WINDOW = 1.5  # seconds after the press searched for native motion
KICK_HEIGHT = 1.2  # metres above the character root (feet rest near 0.27, sprint heels reach ~1.0 behind)
KICK_FORWARD = 0.5  # metres in front of the root along its z axis
DODGE_GRACE = 2.0  # rolls lift the feet over the head; ignore raises this soon after A
BOUNCE_WINDOW = 0.08  # LB pressed again this soon after the attack pulse ended
SHORT_PULSE = 0.12  # the candidate pulse is 140 ms; shorter means something cut it
# Rules Joe chose on 20 Oct (VrSwingMelee / VrGuard on claude/melee), mirrored here so a
# trace recorded under the old rules shows what the new ones would have done.
POWER_TRIGGER = 0.15  # VrSwingMelee::POWER_TRIGGER: a resting finger reads 0.05-0.09
MIN_WRIST_TRAVEL = 0.11  # VrSwingMelee::MIN_WRIST_TRAVEL, metres within the 200 ms history
ACTIVATION_SPEED = 1.5  # speed_metres_per_second candidate
COOLDOWN = 0.45  # cooldown_ms candidate, seconds
OLD_GUARD = {"min_angle": 50.0, "min_forward": 0.15, "max_forward": 0.75, "min_height": -0.55, "max_height": 0.2, "min_lateral": -0.5, "max_lateral": 0.3}
NEW_GUARD = dict(OLD_GUARD, min_angle=62.0, max_entry_forward=0.30)  # entry near the face; a held block may drift to max_forward
GUARD_EXIT_MARGIN, GUARD_ANGLE_SLACK, AIM_RELEASE = 0.05, 10.0, 0.05
LEFT_PUNCH_SPEED = 2.5  # m/s head-relative left wrist at 30 Hz: guard raises stay near 1.5


def _vector(value):
    return value if isinstance(value, list) and len(value) == 3 and all(isinstance(v, (int, float)) for v in value) else None


def compact(record):
    """The fields this analysis needs from one sample line, or None."""
    i, c = record.get("input") or {}, record.get("character") or {}
    row = {"t": record["t"]}
    for key in ("head", "left", "right", "left_forward", "right_forward"):
        row[key] = _vector(i.get(key))
    guard = i.get("guard") or {}
    row["guard_held"] = bool(guard.get("held"))
    row["guard_in_zone"] = bool(guard.get("in_zone"))
    row["guard_hand"] = _vector(guard.get("hand")) if guard.get("enabled") else None  # lateral, height, forward
    row["guard_angle"] = guard.get("angle_degrees")
    row["right_trigger"] = (i.get("raw_controls") or {}).get("right_trigger")
    row["left_trigger"] = (i.get("raw_controls") or {}).get("left_trigger")
    row["holster_zone"] = i.get("holster_zone") or 0
    row["root"], row["root_z"] = _vector(c.get("root")), _vector(c.get("root_z_axis"))
    row["upm"] = c.get("units_per_metre")
    feet = {b.get("name"): _vector(b.get("native")) for b in c.get("body") or [] if b.get("name") in ("FOOT1L", "FOOT1R")}
    row["feet"] = feet
    return row


def read(lines):
    header, samples, events, bad = None, [], [], 0
    for raw in lines:
        raw = raw.strip()
        if not raw:
            continue
        try:
            record = json.loads(raw)
        except json.JSONDecodeError:
            bad += 1
            continue
        kind = record.get("type")
        if kind == "header":
            header = record
        elif kind == "sample":
            samples.append(compact(record))
        elif kind == "event":
            events.append(record)
    return header, samples, events, bad


def endpoints(row, side):
    hand, head, forward = row.get(side), row.get("head"), row.get(side + "_forward")
    if not (hand and head and forward):
        return None
    wrist = [hand[k] - head[k] for k in range(3)]
    return wrist, [wrist[k] + forward[k] * REACH for k in range(3)]


def hand_speeds(samples, times, t0, t1):
    """Peak head-relative wrist and tip speed of each hand between t0 and t1 (m/s)."""
    out = {side: {"wrist": 0.0, "tip": 0.0} for side in ("left", "right")}
    first = max(bisect.bisect_left(times, t0), 1)
    for k in range(first, bisect.bisect_right(times, t1)):
        a, b = samples[k - 1], samples[k]
        dt = b["t"] - a["t"]
        if not 0 < dt <= 0.1:
            continue
        for side in out:
            pa, pb = endpoints(a, side), endpoints(b, side)
            if not (pa and pb):
                continue
            out[side]["wrist"] = max(out[side]["wrist"], math.dist(pa[0], pb[0]) / dt)
            out[side]["tip"] = max(out[side]["tip"], math.dist(pa[1], pb[1]) / dt)
    return out


def lead(speeds):
    """Which hand moved more: compares the faster endpoint (wrist or tip) of each hand."""
    left = max(speeds["left"].values())
    right = max(speeds["right"].values())
    return "left" if left > right else "right"


def local(row, point):
    """Point in the character root frame, metres: (lateral, height, forward)."""
    root, z, upm = row.get("root"), row.get("root_z"), row.get("upm")
    if not (root and z and upm and point):
        return None
    x = (z[2], 0.0, -z[0])
    d = [point[k] - root[k] for k in range(3)]
    return (sum(d[k] * x[k] for k in range(3)) / upm, d[1] / upm, sum(d[k] * z[k] for k in range(3)) / upm)


def buttons(events):
    """Button edges as (t, name, down)."""
    return [(e["t"], e.get("name"), bool(e.get("down"))) for e in events if e.get("event") == "button"]


def next_edge(edges, name, down, after, before=math.inf):
    for t, n, d in edges:
        if n == name and d == down and after <= t < before:
            return t
    return None


def root_travel(samples, times, t0, t1):
    first = bisect.bisect_left(times, t0)
    start = next((s for s in samples[first:] if s["root"] and s["upm"]), None)
    if not start:
        return None
    best = 0.0
    for s in samples[first:bisect.bisect_right(times, t1)]:
        if s["root"] and s["upm"]:
            best = max(best, math.dist(s["root"], start["root"]) / s["upm"])
    return best


def kicks(samples, events):
    """Episodes where a foot rises well above and in front of the root, outside dodge rolls."""
    dodges = [t for t, n, d in buttons(events) if n == "A_dodge_confirm" and d]
    out, current = [], None
    for s in samples:
        best = None
        for name, foot in (s["feet"] or {}).items():
            p = local(s, foot)
            if p and p[1] >= KICK_HEIGHT and p[2] >= KICK_FORWARD and (best is None or p[1] > best[1][1]):
                best = (name, p)
        rolling = any(0 <= s["t"] - t <= DODGE_GRACE for t in dodges)
        if best and not rolling:
            if current and s["t"] - current["end"] <= 0.2:
                current["end"] = s["t"]
                if best[1][1] > current["height"]:
                    current.update(foot=best[0], height=best[1][1], forward=best[1][2])
            else:
                current = {"start": s["t"], "end": s["t"], "foot": best[0], "height": best[1][1], "forward": best[1][2]}
                out.append(current)
    for k in out:
        k["preceding"] = [{"t": round(t - k["start"], 3), "name": n} for t, n, d in buttons(events)
                          if d and -RESPONSE_WINDOW <= t - k["start"] <= 0 and n not in ("RT_fire", "LT_aim")]
    return out


def swings(samples, events):
    times = [s["t"] for s in samples]
    edges = buttons(events)
    guard_starts = [e["t"] for e in events if e.get("event") == "guard_start"]
    kick_list = kicks(samples, events)
    rows = []
    for e in events:
        if e.get("event") != "swing":
            continue
        t = e["t"]
        power = next_edge(edges, "up_power_melee", True, t - 0.005, t + 0.005) is not None
        name = "up_power_melee" if power else "RB_melee"
        released = next_edge(edges, name, False, t + 0.001)
        pulse = released - t if released is not None else None
        k = min(bisect.bisect_left(times, t), len(samples) - 1)
        guard = samples[k]["guard_held"] if samples else False
        lb_released = next_edge(edges, "LB_parry_block", False, t - 0.005, t + 0.005) is not None
        bounce = None
        if released is not None:
            again = next_edge(edges, "LB_parry_block", True, released - 0.005, released + BOUNCE_WINDOW)
            if again is not None and not any(abs(g - again) < 0.005 for g in guard_starts):
                bounce = round(again - t, 3)
        zone = max((s["holster_zone"] for s in samples[k:bisect.bisect_right(times, t + 0.15)]), default=0)
        speeds = hand_speeds(samples, times, t + LEAD_WINDOW[0], t + LEAD_WINDOW[1])
        kick = next((x for x in kick_list if 0 <= x["start"] - t <= RESPONSE_WINDOW), None)
        rows.append({
            "t": t, "detector_speed": e.get("speed"), "native": "power" if power else "light",
            "pulse_ms": round(pulse * 1000) if pulse is not None else None,
            "short_pulse_in_holster_zone": bool(pulse is not None and pulse < SHORT_PULSE and zone),
            "right_trigger": samples[k]["right_trigger"] if samples else None,
            "guard_held": guard, "lb_released_for_swing": lb_released, "lb_repressed_after_ms": None if bounce is None else round(bounce * 1000),
            "speeds": {side: {key: round(v, 2) for key, v in d.items()} for side, d in speeds.items()},
            "lead": lead(speeds),
            "root_travel_metres": None if not samples else root_travel(samples, times, t, t + RESPONSE_WINDOW),
            "kick_after_s": None if not kick else round(kick["start"] - t, 2),
        })
    return rows


def fast_motions(samples, events, side="left", threshold=LEFT_PUNCH_SPEED):
    """Episodes of fast head-relative wrist motion of one hand and what the mod sent meanwhile."""
    out, current = [], None
    for a, b in zip(samples, samples[1:]):
        dt = b["t"] - a["t"]
        pa, pb = endpoints(a, side), endpoints(b, side)
        if not (0 < dt <= 0.1 and pa and pb):
            continue
        speed = math.dist(pa[0], pb[0]) / dt
        if speed < threshold:
            continue
        if current and b["t"] - current["end"] <= 0.15:
            current["end"], current["peak"] = b["t"], max(current["peak"], speed)
        else:
            current = {"start": a["t"], "end": b["t"], "peak": speed}
            out.append(current)
    edges = buttons(events)
    for m in out:
        lo, hi = m["start"] - 0.1, m["end"] + 0.3
        m["peak"] = round(m["peak"], 2)
        m["swings"] = [round(e["t"], 3) for e in events if e.get("event") == "swing" and lo <= e["t"] <= hi]
        m["guard_starts"] = [round(e["t"], 3) for e in events if e.get("event") == "guard_start" and lo <= e["t"] <= hi]
        m["presses"] = sorted({n for t, n, d in edges if d and lo <= t <= hi and n not in ("RT_fire", "LT_aim")})
    return out


def wrist_span(samples, times, t0, t1):
    """Largest head-relative distance between any two right-wrist samples in [t0, t1]."""
    seg = [endpoints(s, "right") for s in samples[bisect.bisect_left(times, t0):bisect.bisect_right(times, t1)]]
    seg = [e[0] for e in seg if e]
    return max((math.dist(a, b) for a in seg for b in seg), default=0.0)


def new_rule_verdicts(samples, rows):
    """What each committed swing would be under the 20 Oct rules (approximate at 30 Hz)."""
    times = [s["t"] for s in samples]
    out, cooldown_from = [], -math.inf
    for r in rows:
        left = max(r["speeds"]["left"].values())
        right = max(r["speeds"]["right"].values())
        # The gated detector waits until the wrist has moved; a swing whose wrist never spans
        # MIN_WRIST_TRAVEL from 200 ms before to 150 ms after the old commit never commits.
        span = wrist_span(samples, times, r["t"] - 0.2, r["t"] + 0.15)
        if r["t"] - cooldown_from < COOLDOWN:
            # The old build's holster-zone reset wiped this cooldown; the fixed detector keeps it.
            verdict = "nothing (cooldown)"
        elif left >= max(ACTIVATION_SPEED, right):
            verdict = "nothing (left hand led)"
            cooldown_from = r["t"]
        elif span < MIN_WRIST_TRAVEL:
            verdict = "nothing (wrist flick)"
        elif (r["right_trigger"] or 0) > POWER_TRIGGER:
            verdict, cooldown_from = "power", r["t"]
        else:
            verdict, cooldown_from = "light", r["t"]
        out.append({"t": r["t"], "was": r["native"], "wrist_span_metres": round(span, 3), "now": verdict})
    return out


def replay_guard(samples, events, settings, aim_latch=True):
    """Replay VrGuard's zone, angle, hysteresis and (new) post-trigger latch on the 30 Hz samples."""
    releases = [t for t, n, d in buttons(events) if n == "RT_fire" and not d]
    starts, pose, aim_latched = [], False, False
    for s in samples:
        hand, angle, aim = s.get("guard_hand"), s.get("guard_angle"), s.get("left_trigger")
        if not hand or angle is None or aim is None:
            pose = aim_latched = False
            continue
        lateral, height, forward = hand
        m = GUARD_EXIT_MARGIN if pose else 0.0
        in_zone = (settings["min_height"] - m <= height <= settings["max_height"] + m and settings["min_forward"] - m <= forward <= settings["max_forward"] + m
                   and settings["min_lateral"] - m <= lateral <= settings["max_lateral"] + m)
        if aim > AIM_RELEASE:
            aim_latched = True
        elif not in_zone or not aim_latch:
            aim_latched = False
        close = pose or forward <= settings.get("max_entry_forward", settings["max_forward"])
        now = in_zone and close and angle >= settings["min_angle"] - (GUARD_ANGLE_SLACK if pose else 0.0) and not aim_latched
        if now and not pose:
            starts.append({"t": s["t"], "forward": round(forward, 3), "angle": round(angle, 1),
                           "after_pistol_release": any(0 <= s["t"] - t <= 0.1 for t in releases)})
        pose = now
    return starts


def analyse(lines):
    header, samples, events, bad = read(lines)
    rows = swings(samples, events)
    report = {
        "header": header, "bad_lines": bad, "swings": rows, "kicks": kicks(samples, events),
        "left_fast_motions": fast_motions(samples, events),
        "counts": {
            "swings": len(rows),
            "light": sum(r["native"] == "light" for r in rows),
            "power": sum(r["native"] == "power" for r in rows),
            "left_led": sum(r["lead"] == "left" for r in rows),
            "guard_held": sum(r["guard_held"] for r in rows),
            "lb_repressed": sum(r["lb_repressed_after_ms"] is not None for r in rows),
            "short_pulse_in_holster_zone": sum(r["short_pulse_in_holster_zone"] for r in rows),
            # Written by builds with left-hand arbitration (claude/melee, 20 Oct).
            "left_hand_vetoes": sum(e.get("event") == "swing_vetoed" for e in events),
            "guard_yields": sum(e.get("event") == "guard_yield" for e in events),
        },
        "new_rules": new_rule_verdicts(samples, rows),
        "guard_replay": {"old": replay_guard(samples, events, OLD_GUARD, aim_latch=False), "new": replay_guard(samples, events, NEW_GUARD)},
        "vetoes": [{"t": e["t"], "speed": e.get("speed"), "other_hand_speed": e.get("other_hand_speed")} for e in events if e.get("event") == "swing_vetoed"],
    }
    return report


def markdown(report):
    out = ["| t (s) | lead | R wrist/tip | L wrist/tip | RT | native | pulse ms | guard | LB after | root m | kick |",
           "| ---: | --- | --- | --- | ---: | --- | ---: | --- | --- | ---: | --- |"]
    for r in report["swings"]:
        s = r["speeds"]
        guard = "held" if r["guard_held"] else ""
        lb = f"re-pressed +{r['lb_repressed_after_ms']}" if r["lb_repressed_after_ms"] is not None else ""
        pulse = f"{r['pulse_ms']}" + (" (holster zone)" if r["short_pulse_in_holster_zone"] else "")
        travel = "" if r["root_travel_metres"] is None else f"{r['root_travel_metres']:.1f}"
        kick = "" if r["kick_after_s"] is None else f"+{r['kick_after_s']} s"
        rt = "" if r["right_trigger"] is None else f"{r['right_trigger']:.2f}"
        out.append(f"| {r['t']:.3f} | {r['lead']} | {s['right']['wrist']:.1f}/{s['right']['tip']:.1f} | {s['left']['wrist']:.1f}/{s['left']['tip']:.1f} "
                   f"| {rt} | {r['native']} | {pulse} | {guard} | {lb} | {travel} | {kick} |")
    out.append("")
    for k in report["kicks"]:
        before = ", ".join(f"{p['name']} {p['t']:+.2f}" for p in k["preceding"]) or "none"
        out.append(f"Kick {k['start']:.3f}-{k['end']:.3f} s: {k['foot']} {k['height']:.2f} m high, {k['forward']:.2f} m forward; preceding inputs: {before}")
    out.append("")
    for m in report.get("left_fast_motions", []):
        out.append(f"Left hand fast {m['start']:.3f}-{m['end']:.3f} s, peak wrist {m['peak']:.1f} m/s: swings {m['swings'] or 'none'}, "
                   f"guard starts {m['guard_starts'] or 'none'}, presses {', '.join(m['presses']) or 'none'}")
    out.append("")
    if report.get("new_rules"):
        tally = {}
        for v in report["new_rules"]:
            tally[v["now"]] = tally.get(v["now"], 0) + 1
        out.append("Under the 20 Oct rules: " + ", ".join(f"{k} {v}" for k, v in sorted(tally.items())))
        out.append("Changed swings: " + ", ".join(f"{v['t']:.3f} {v['was']} -> {v['now']}" for v in report["new_rules"] if v["now"] != v["was"]))
    for name, starts in (report.get("guard_replay") or {}).items():
        out.append(f"Guard replay ({name}): {len(starts)} starts, {sum(s['after_pistol_release'] for s in starts)} right after a pistol trigger release: "
                   + ", ".join(f"{s['t']:.2f} ({s['forward']:.2f} m, {s['angle']:.0f} deg)" for s in starts))
    out.append("")
    out.append("Counts: " + ", ".join(f"{k} {v}" for k, v in report["counts"].items()))
    return "\n".join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    with args.trace.open(encoding="utf-8", errors="replace") as f:
        report = analyse(f)
    print(json.dumps(report, indent=1) if args.json else markdown(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
