"""Summarise a playtest trace (sm2vr_trace.jsonl, written when SM2VR_TRACE=1).

The trace has fixed-rate "sample" lines (tracked head/hands, pad output, the character's
head/hand bones and the player-to-character offsets in metres) and "event" lines (every
button edge and gesture outcome). This script answers, without the headset:

* how far the rendered eye sits from the character's head bone, and whether that
  offset drifts over time (least-squares slope per segment; segments split wherever the
  mod legitimately re-anchors: recenter, actor change, camera handover);
* how far the tracked hands are from the animated hands, and the IK residual;
* which actions were sent to the game and what each gesture did.

Usage: analyze_vr_trace.py TRACE [--json] [--events]
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from datetime import datetime, timedelta
from pathlib import Path

# Re-anchoring events: the offset may legitimately jump here, so drift is measured within segments.
SEGMENT_EVENTS = {"tracking_reset", "actor", "camera_mode"}
AXES = ("x", "y", "z")
# Thresholds for warnings (metres, metres per minute).
DRIFT_WARN = 0.02
MIN_DRIFT_SECONDS = 15  # Shorter segments (camera-mode flicker) get no drift slope.
LEAN_WARN = 5  # degrees of spine lean while running
OFFSET_WARN = 0.15  # Eyes normally sit a little in front of the head bone.
RESIDUAL_WARN = 0.02


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
            samples.append(record)
        elif kind == "event":
            events.append(record)
        else:
            bad += 1
    return header, samples, events, bad


def slope_per_minute(times, values):
    """Least-squares slope in units per minute; None for fewer than 3 points or no time span."""
    if len(times) < 3:
        return None
    mean_t, mean_v = statistics.fmean(times), statistics.fmean(values)
    spread = sum((t - mean_t) ** 2 for t in times)
    if spread <= 1e-12:
        return None
    return 60 * sum((t - mean_t) * (v - mean_v) for t, v in zip(times, values)) / spread


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return None
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def describe(values):
    if not values:
        return None
    return {"n": len(values), "mean": statistics.fmean(values), "median": statistics.median(values),
            "p95": percentile(values, 0.95), "max": max(values)}


def horizontal(v):
    return math.hypot(v[0], v[2])


def segments(samples, events):
    cuts = sorted(e["t"] for e in events if e.get("event") in SEGMENT_EVENTS)
    groups, current, next_cut = [], [], 0
    for s in samples:
        while next_cut < len(cuts) and s["t"] >= cuts[next_cut]:
            if current:
                groups.append(current)
            current = []
            next_cut += 1
        current.append(s)
    if current:
        groups.append(current)
    return groups


def head_offsets(group):
    out = []
    for s in group:
        if (s.get("character") or {}).get("animation_takeover"):
            continue  # A pounce/grab/execution moves the head bone on purpose; it is not drift.
        v = (s.get("metrics") or {}).get("head_offset_metres")
        if v is not None and all(isinstance(x, (int, float)) for x in v):
            out.append((s["t"], v))
    return out


def analyse_segment(group):
    points = head_offsets(group)
    result = {"start": group[0]["t"], "end": group[-1]["t"], "samples": len(group), "head_samples": len(points)}
    if not points:
        return result
    times = [t for t, _ in points]
    if result["end"] - result["start"] < MIN_DRIFT_SECONDS:
        result["drift_per_minute"] = None  # A slope over a few seconds is noise, not drift.
    result["head_offset"] = {axis: describe([v[i] for _, v in points]) for i, axis in enumerate(AXES)}
    result["head_offset"]["horizontal"] = describe([horizontal(v) for _, v in points])
    if "drift_per_minute" not in result:
        result["drift_per_minute"] = {axis: slope_per_minute(times, [v[i] for _, v in points]) for i, axis in enumerate(AXES)}
        result["drift_per_minute"]["horizontal"] = slope_per_minute(times, [horizontal(v) for _, v in points])
    tenth = max(1, len(points) // 10)
    first = [statistics.fmean(v[i] for _, v in points[:tenth]) for i in range(3)]
    last = [statistics.fmean(v[i] for _, v in points[-tenth:]) for i in range(3)]
    result["start_to_end"] = [b - a for a, b in zip(first, last)]
    return result


def hands(samples):
    out = {}
    for side in ("left", "right"):
        offsets = [math.sqrt(sum(x * x for x in v)) for s in samples
                   if (v := (s.get("metrics") or {}).get(f"{side}_target_offset_metres")) is not None]
        residuals = [r for s in samples if (r := (s.get("metrics") or {}).get(f"{side}_residual_metres")) is not None]
        out[side] = {"target_to_animated_hand": describe(offsets), "ik_residual": describe(residuals),
                     "residual_over_2cm": (sum(r > RESIDUAL_WARN for r in residuals) / len(residuals)) if residuals else None}
    return out


# Game-side events, reported separately from the player's gestures.
GAME_EVENTS = {"game_state", "animation_takeover_start", "animation_takeover_end", "animset", "weapon_bones", "marker"}


def game_events(events):
    """Inferred animation takeovers (pounce, grab, knockback, execution) and native cinematic/pause changes."""
    ends = [e for e in events if e.get("event") == "animation_takeover_end"]
    starts = [e for e in events if e.get("event") == "animation_takeover_start"]
    states = [e for e in events if e.get("event") == "game_state"]
    return {"takeovers": len(starts), "takeover_seconds": sum(e.get("duration_ms", 0) for e in ends) / 1000,
            "longest_takeover_seconds": max((e.get("duration_ms", 0) for e in ends), default=0) / 1000,
            "takeover_times": [e["t"] for e in starts],
            "cinematics": sum(1 for e in states if e.get("cinematic")), "pauses": sum(1 for e in states if e.get("paused")),
            "markers": [e["t"] for e in events if e.get("event") == "marker"]}


def actions(events):
    presses, gestures = {}, {}
    for e in events:
        name = e.get("event")
        if name == "button":
            if e.get("down"):
                presses[e["name"]] = presses.get(e["name"], 0) + 1
        elif name == "grenade_release":
            key = f"grenade_{e.get('outcome')}"
            gestures[key] = gestures.get(key, 0) + 1
        elif name not in SEGMENT_EVENTS and name not in GAME_EVENTS and name != "input_armed":
            gestures[name] = gestures.get(name, 0) + 1
    return presses, gestures


def coverage(samples):
    def share(predicate):
        return sum(1 for s in samples if predicate(s)) / len(samples) if samples else 0
    return {"input": share(lambda s: s.get("input") is not None), "character": share(lambda s: s.get("character") is not None),
            "camera": share(lambda s: s.get("camera") is not None),
            "head_offset": share(lambda s: (s.get("metrics") or {}).get("head_offset_metres") is not None)}


def facing(samples):
    values = [abs(c["facing_error_degrees"]) for s in samples if (c := s.get("camera")) and isinstance(c.get("facing_error_degrees"), (int, float))]
    return describe(values)


SWITCH_BUTTONS = {"up_primary", "left_secondary", "down_switch_weapon"}
SWITCH_WINDOW = 3.0  # seconds after a weapon switch to measure
FLOAT_WARN = 0.25  # metres between a weapon bone and the right hand


def weapon_rows(sample):
    return (sample.get("character") or {}).get("weapons") or []


def weapon_distances(samples, side="right"):
    """Per weapon bone: distance to a hand (metres) and how often the mod moved it."""
    by_name = {}
    for s in samples:
        for w in weapon_rows(s):
            entry = by_name.setdefault(w["name"], {"distances": [], "moved": 0, "seen": 0})
            entry["seen"] += 1
            entry["moved"] += w.get("final") is not None
            d = w.get(f"to_{side}_hand_metres")
            if d is not None:
                entry["distances"].append(d)
    return {name: {"to_hand": describe(e["distances"]), "moved_by_mod": e["moved"] / e["seen"] if e["seen"] else 0}
            for name, e in sorted(by_name.items())}


def weapon_switches(samples, events):
    """Each weapon switch (holster pulse, switch button or animset change): every weapon bone's
    median distance to the right hand in the window before and after it. The bone a floating
    weapon hangs from is the one whose distance jumps and stays large after the switch."""
    triggers = [e for e in events if (e.get("event") == "button" and e.get("down") and e.get("name") in SWITCH_BUTTONS)
                or e.get("event") == "animset"]
    out = []
    for e in triggers:
        before = [s for s in samples if e["t"] - SWITCH_WINDOW <= s["t"] < e["t"]]
        after = [s for s in samples if e["t"] + 0.5 <= s["t"] <= e["t"] + SWITCH_WINDOW]  # Let the draw animation start.
        b, a = weapon_distances(before), weapon_distances(after)
        bones = {}
        for name in sorted(set(b) | set(a)):
            mb = (b.get(name, {}).get("to_hand") or {}).get("median")
            ma = (a.get(name, {}).get("to_hand") or {}).get("median")
            bones[name] = {"before": mb, "after": ma, "moved_by_mod_after": a.get(name, {}).get("moved_by_mod")}
        out.append({"t": e["t"], "trigger": e.get("name") or f"animset {e.get('animset')}", "bones": bones})
    return out


def weapon_ray(samples):
    rays = [r for s in samples if (r := s.get("weapon_ray"))]
    return {"samples": len(rays), "overridden": (sum(bool(r.get("overridden")) for r in rays) / len(rays)) if rays else None}


SPEED_BANDS = (("standing", 0, 0.2), ("walking", 0.8, 3), ("running", 3, 50))


def lean_by_speed(samples):
    """Spine lean and eye-to-head offsets grouped by the character root's measured speed
    (m/s between consecutive samples), not by the stick, which may not reach the game."""
    groups = {name: [] for name, _, _ in SPEED_BANDS}
    previous = None
    for s in samples:
        c = s.get("character")
        if not c or not c.get("root") or not c.get("units_per_metre"):
            previous = None
            continue
        root, upm = c["root"], c["units_per_metre"]
        if previous and 0 < s["t"] - previous[0] < 0.2:
            speed = math.hypot(root[0] - previous[1][0], root[2] - previous[1][2]) / upm / (s["t"] - previous[0])
            for name, low, high in SPEED_BANDS:
                if low <= speed < high:
                    groups[name].append(s)
        previous = (s["t"], root)
    out = {}
    for name, rows in groups.items():
        def median_of(get):
            values = [v for r in rows if (v := get(r)) is not None]
            return statistics.median(values) if values else None
        out[name] = {
            "samples": len(rows),
            "lean_native_degrees": median_of(lambda r: ((r["character"].get("lean_degrees") or {}).get("native"))),
            "lean_final_degrees": median_of(lambda r: ((r["character"].get("lean_degrees") or {}).get("final"))),
            # Shoulder twist from the bind pose about the spine (docs/PAULDRON.md), unsigned.
            "twist_native_degrees": median_of(lambda r: None if (v := (r["character"].get("twist_degrees") or {}).get("native")) is None else abs(v)),
            "twist_final_degrees": median_of(lambda r: None if (v := (r["character"].get("twist_degrees") or {}).get("final")) is None else abs(v)),
            "eye_forward_of_head_metres": median_of(lambda r: (v := (r.get("metrics") or {}).get("head_offset_metres")) and v[2]),
            "eye_forward_of_published_head_metres": median_of(lambda r: (v := (r.get("metrics") or {}).get("head_offset_final_metres")) and v[2]),
        }
    return out


LS_SPRINT = 0x0040  # XINPUT_GAMEPAD_LEFT_THUMB: the mod holding native sprint.
SPRINT_SPEED = 7.0  # m/s; native sprint 8.4-8.7, full-stick running 5.8 (docs/SPRINT_CAMERA.md)
SPRINT_BONES = ("HEAD", "NECK", "BACKup")
BACKPACK_WARN = 0.10  # metres of eye clearance ahead of BACKup; walking has about 0.22


def sprint_clearance(samples):
    """How far the upper body sits ahead of the rendered eye, by locomotion state.

    Forward is the character root's level z axis, in metres. For each state: each bone's
    native position forward of the root, the eye forward of that bone (negative = the
    bone is in front of the eye), and the sprint camera offset the mod applied."""
    states = {name: [] for name in ("standing", "walking", "running", "sprinting")}
    previous = None
    for s in samples:
        c, cam = s.get("character"), s.get("camera")
        if not c or not c.get("root") or not c.get("units_per_metre") or not c.get("root_z_axis"):
            previous = None
            continue
        root, upm = c["root"], c["units_per_metre"]
        speed = None
        if previous and 0 < s["t"] - previous[0] < 0.2:
            speed = math.hypot(root[0] - previous[1][0], root[2] - previous[1][2]) / upm / (s["t"] - previous[0])
        previous = (s["t"], root)
        eye = (cam or {}).get("eye_centre")
        if speed is None or not eye:
            continue
        pad = ((s.get("input") or {}).get("pad") or {}).get("buttons", 0)
        sprint_pad = bool(pad & LS_SPRINT)
        if sprint_pad and speed >= SPRINT_SPEED:
            state = "sprinting"
        elif sprint_pad or c.get("animation_takeover"):
            continue  # Sprint onset/refusal, or a pounce/execution moving the bones on purpose.
        elif speed < 0.2:
            state = "standing"
        elif 0.8 <= speed < 3:
            state = "walking"
        elif speed >= 3:
            state = "running"
        else:
            continue
        z = c["root_z_axis"]
        length = math.hypot(z[0], z[2])
        if length < 1e-4:
            continue
        def forward(v):
            return (v[0] * z[0] + v[2] * z[2]) / length / upm
        bones = {b["name"]: b["native"] for b in c.get("body") or [] if b.get("native")}
        if c.get("head"):
            bones["HEAD"] = c["head"]
        row = {"offset": ((s.get("input") or {}).get("sprint_camera") or {}).get("offset_metres")}
        for name in SPRINT_BONES:
            if name in bones:
                row[name] = forward([bones[name][i] - root[i] for i in range(3)])
                row["eye_" + name] = forward([eye[i] - bones[name][i] for i in range(3)])
        states[state].append(row)
    out = {}
    for name, rows in states.items():
        def median_of(key):
            values = [r[key] for r in rows if r.get(key) is not None]
            return statistics.median(values) if values else None
        band = {"samples": len(rows), "sprint_offset_metres": median_of("offset")}
        for bone in SPRINT_BONES:
            band[f"{bone}_forward_of_root_metres"] = median_of(bone)
            band[f"eye_forward_of_{bone}_metres"] = median_of("eye_" + bone)
        out[name] = band
    return out


def body_scale(events):
    """Explicit-recenter eye-height readings (docs/BODY_SCALE.md) and the factor they left."""
    readings = [{"t": e["t"], "reading": e.get("reading"), "eye_height_metres": e.get("player_eye_height_metres"),
                 "factor": e.get("factor")} for e in events if e.get("event") == "body_scale"]
    return {"readings": readings, "factor": readings[-1]["factor"] if readings else None}


def room_walking(samples):
    """Walking in the play space (docs/BODY_FOLLOWING.md): the counters at the end of the trace, how far
    the head was from the body in first person, and what returns to first person dropped."""
    cameras = [s["camera"] for s in samples if isinstance(s.get("camera"), dict) and "room_steps" in s["camera"]]
    if not cameras:
        return None
    first_person = [c for c in cameras if c.get("mode") == "body" and c.get("anchored")]
    errors = [c["room_error_metres"] for c in first_person if isinstance(c.get("room_error_metres"), (int, float))]
    last = cameras[-1]
    return {"steps": last["room_steps"] - cameras[0]["room_steps"], "rebases": last["room_rebases"] - cameras[0]["room_rebases"],
            "largest_rebase_metres": max((c["room_rebased_metres"] for c in cameras), default=0),
            "held_metres": last["room_held_metres"] - cameras[0]["room_held_metres"],
            "blocked_share": sum(1 for c in first_person if c.get("room_blocked")) / len(first_person) if first_person else 0,
            "head_from_body_metres": describe(errors)}


def warnings(report):
    out = []
    rejected = [r for r in report["body_scale"]["readings"] if r["reading"] != "accepted"]
    if rejected:
        out.append(f"body scale: {len(rejected)} recenter(s) not measured ("
                   + ", ".join(f"t={r['t']:.1f} {r['reading']}" for r in rejected) + "); the player recentered crouched or seated")
    for i, seg in enumerate(report["segments"]):
        drift = (seg.get("drift_per_minute") or {}).get("horizontal")
        minutes = (seg["end"] - seg["start"]) / 60
        if drift is not None and minutes >= 0.25 and abs(drift) >= DRIFT_WARN:
            out.append(f"segment {i}: eye-to-head-bone offset drifts {drift * 100:+.1f} cm/min horizontally over {minutes:.1f} min")
        median = ((seg.get("head_offset") or {}).get("horizontal") or {}).get("median")
        if median is not None and median >= OFFSET_WARN:
            out.append(f"segment {i}: eye sits {median * 100:.0f} cm (median, horizontal) from the character's head bone")
    for side, data in report["hands"].items():
        share = data["residual_over_2cm"]
        if share is not None and share > 0.1:
            out.append(f"{side} hand: IK misses the tracked wrist by >2 cm in {share * 100:.0f}% of samples (arm reach/calibration)")
    g = report["gestures"]
    if g.get("guard_rejected_angle", 0) > max(3, g.get("guard_start", 0)):
        out.append(f"guard: {g['guard_rejected_angle']} raises rejected as forward-pointing vs {g.get('guard_start', 0)} guards; consider a lower min_angle_degrees")
    if g.get("grenade_cancelled", 0) > g.get("grenade_thrown", 0) and g.get("grenade_cancelled", 0) >= 3:
        out.append(f"grenade: {g['grenade_cancelled']} put back vs {g.get('grenade_thrown', 0)} thrown; release speeds are in the grenade_release events")
    for switch in report["weapon_switches"]:
        for name, bone in switch["bones"].items():
            if bone["after"] is not None and bone["after"] >= FLOAT_WARN and (bone["before"] is None or bone["after"] - bone["before"] >= 0.1):
                was = "n/a" if bone["before"] is None else f"{bone['before'] * 100:.0f} cm"
                unmoved = ", not moved by the mod" if bone["moved_by_mod_after"] == 0 else ""
                out.append(f"weapon switch at t={switch['t']:.1f} s ({switch['trigger']}): {name} is "
                           f"{bone['after'] * 100:.0f} cm from the right hand (was {was}){unmoved}")
    run = report["lean"]["running"]
    if run["samples"] >= 30 and run["lean_native_degrees"] is not None and run["lean_native_degrees"] >= LEAN_WARN:
        final = run["lean_final_degrees"]
        if final is None or final >= LEAN_WARN:
            out.append(f"running leans the spine {run['lean_native_degrees']:.1f} deg (published {'n/a' if final is None else f'{final:.1f}'} deg):"
                       " helmet/shoulders in view; try SM2VR_UPRIGHT_TORSO=1")
    sprint = report["sprint"]["sprinting"]
    clearance = sprint["eye_forward_of_BACKup_metres"]
    if sprint["samples"] >= 30 and clearance is not None and clearance < BACKPACK_WARN:
        out.append(f"sprinting: the eye is only {clearance * 100:.0f} cm ahead of BACKup (walking ~22 cm), so the backpack fills the view;"
                   " check sprint_forward_offset_metres in the calibration")
    if report["coverage"]["head_offset"] < 0.5 and report["samples"]:
        out.append(f"head offset only available in {report['coverage']['head_offset'] * 100:.0f}% of samples (first person with hands enabled?)")
    return out


def analyse(lines):
    header, samples, events, bad = read(lines)
    report = {"header": header, "samples": len(samples), "events": len(events), "malformed": bad,
              "duration_seconds": (samples[-1]["t"] - samples[0]["t"]) if len(samples) > 1 else 0,
              "coverage": coverage(samples), "segments": [analyse_segment(g) for g in segments(samples, events)],
              "hands": hands(samples), "facing_error_degrees": facing(samples)}
    report["presses"], report["gestures"] = actions(events)
    report["weapons"] = weapon_distances(samples)
    report["weapon_switches"] = weapon_switches(samples, events)
    report["weapon_ray"] = weapon_ray(samples)
    report["game"] = game_events(events)
    report["lean"] = lean_by_speed(samples)
    report["sprint"] = sprint_clearance(samples)
    report["body_scale"] = body_scale(events)
    report["room_walking"] = room_walking(samples)
    report["warnings"] = warnings(report)
    return report, events


def wall(header, t):
    try:
        return (datetime.fromisoformat(header["wall_clock_local"]) + timedelta(seconds=t)).strftime("%H:%M:%S")
    except (TypeError, KeyError, ValueError):
        return ""


def fmt(v, scale=100, unit="cm"):
    return "n/a" if v is None else f"{v * scale:+.1f} {unit}"


def text(report, events, show_events):
    lines = [f"samples {report['samples']} over {report['duration_seconds'] / 60:.1f} min, events {report['events']}, malformed {report['malformed']}"]
    c = report["coverage"]
    lines.append(f"coverage: input {c['input']:.0%}, character {c['character']:.0%}, camera {c['camera']:.0%}, head offset {c['head_offset']:.0%}")
    short = 0
    for i, seg in enumerate(report["segments"]):
        if seg["end"] - seg["start"] < MIN_DRIFT_SECONDS:
            short += 1  # Camera-mode flicker; summarised below instead of printing noise.
            continue
        lines.append(f"segment {i}: t={seg['start']:.1f}-{seg['end']:.1f} s, {seg['head_samples']} head samples")
        if "head_offset" in seg:
            ho, dr = seg["head_offset"], seg["drift_per_minute"]
            lines.append("  eye minus head bone (root axes, median): " + ", ".join(f"{a} {fmt(ho[a]['median'])}" for a in AXES)
                         + f"; horizontal median {fmt(ho['horizontal']['median'])}, p95 {fmt(ho['horizontal']['p95'])}")
            lines.append("  drift per minute: " + ", ".join(f"{a} {fmt(dr[a])}" for a in (*AXES, "horizontal")))
    if short:
        lines.append(f"{short} segments shorter than {MIN_DRIFT_SECONDS} s (re-anchoring flicker) not reported; see --json")
    for name, band in report["lean"].items():
        if band["samples"]:
            lines.append(f"{name}: {band['samples']} samples, spine lean native {fmt(band['lean_native_degrees'], 1, 'deg')}, published "
                         f"{fmt(band['lean_final_degrees'], 1, 'deg')}; eye forward of head bone {fmt(band['eye_forward_of_head_metres'])}"
                         f" (published head {fmt(band['eye_forward_of_published_head_metres'])})"
                         + (f"; shoulder twist native {fmt(band['twist_native_degrees'], 1, 'deg')}, published {fmt(band['twist_final_degrees'], 1, 'deg')}"
                            if band.get("twist_native_degrees") is not None else ""))
    for name, band in report["sprint"].items():
        if band["samples"]:
            lines.append(f"{name} clearance: {band['samples']} samples; forward of root HEAD {fmt(band['HEAD_forward_of_root_metres'])}, "
                         f"NECK {fmt(band['NECK_forward_of_root_metres'])}, BACKup {fmt(band['BACKup_forward_of_root_metres'])}; "
                         f"eye forward of BACKup {fmt(band['eye_forward_of_BACKup_metres'])}, of HEAD {fmt(band['eye_forward_of_HEAD_metres'])}; "
                         f"sprint offset {fmt(band['sprint_offset_metres'])}")
    accepted = [r for r in report["body_scale"]["readings"] if r["reading"] == "accepted"]
    if report["body_scale"]["readings"]:
        lines.append(f"body scale: {len(report['body_scale']['readings'])} recenter readings, eye heights "
                     + (", ".join(f"{r['eye_height_metres']:.2f}" for r in accepted) or "none accepted")
                     + f"; final factor {report['body_scale']['factor']:.3f}")
    room = report["room_walking"]
    if room:
        away = room["head_from_body_metres"]
        lines.append(f"play space walking: {room['steps']} steps, head from body median {fmt(away and away['median'])}, p95 {fmt(away and away['p95'])}, "
                     f"max {fmt(away and away['max'])} (Marine metres); blocked {room['blocked_share']:.0%} of first person, held back {room['held_metres']:.2f} m at walls; "
                     f"{room['rebases']} returns to first person, largest dropped {room['largest_rebase_metres']:.2f} m")
    for side, data in report["hands"].items():
        t, r = data["target_to_animated_hand"], data["ik_residual"]
        if t or r:
            lines.append(f"{side} hand: tracked-to-animated median {fmt(t and t['median'])}, IK residual median {fmt(r and r['median'])}, p95 {fmt(r and r['p95'])}")
    if report["facing_error_degrees"]:
        f = report["facing_error_degrees"]
        lines.append(f"body facing error: median {f['median']:.1f} deg, p95 {f['p95']:.1f} deg")
    for name, w in report["weapons"].items():
        d = w["to_hand"]
        lines.append(f"weapon bone {name}: to right hand median {fmt(d and d['median'])}, max {fmt(d and d['max'])}; moved by the mod in {w['moved_by_mod']:.0%} of samples")
    for s in report["weapon_switches"]:
        moved = [f"{n} {fmt(b['before'])} -> {fmt(b['after'])}" for n, b in s["bones"].items()]
        lines.append(f"weapon switch t={s['t']:.1f} s ({s['trigger']}), to right hand before -> after: " + "; ".join(moved))
    if report["weapon_ray"]["samples"]:
        lines.append(f"game aim ray: {report['weapon_ray']['samples']} samples, driven from the wrist in {report['weapon_ray']['overridden']:.0%}")
    g = report["game"]
    if g["takeovers"] or g["cinematics"] or g["markers"]:
        lines.append(f"game: {g['takeovers']} inferred animation takeovers ({g['takeover_seconds']:.1f} s total, longest {g['longest_takeover_seconds']:.1f} s)"
                     f" at t=" + ", ".join(f"{t:.1f}" for t in g["takeover_times"][:20]) + f"; {g['cinematics']} cinematics, {g['pauses']} pauses, markers at "
                     + (", ".join(f"{t:.1f}" for t in g["markers"]) or "none"))
    lines.append("presses: " + (", ".join(f"{k} {v}" for k, v in sorted(report["presses"].items())) or "none"))
    lines.append("gestures: " + (", ".join(f"{k} {v}" for k, v in sorted(report["gestures"].items())) or "none"))
    for w in report["warnings"]:
        lines.append(f"WARNING: {w}")
    if show_events:
        for e in events:
            detail = {k: v for k, v in e.items() if k not in ("t", "type", "event")}
            lines.append(f"{e['t']:9.3f} {wall(report['header'], e['t'])} {e['event']} {json.dumps(detail, separators=(',', ':'))}")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--json", action="store_true", help="machine-readable report")
    parser.add_argument("--events", action="store_true", help="also list every event with wall-clock time")
    args = parser.parse_args(argv)
    with args.trace.open(encoding="utf-8") as handle:
        report, events = analyse(handle)
    print(json.dumps(report, indent=2) if args.json else text(report, events, args.events))
    return 0


if __name__ == "__main__":
    sys.exit(main())
