"""Input disarm windows in a playtest trace (docs/CONTROL_CUTOUT_2026-10-26.md).

For every stretch where the trace shows input disarmed: what started it, how long it lasted,
which held control kept it from re-arming and what finally re-armed it. Until 26 Oct every
disarm waited for the whole pad to rest ("awaiting neutral controls"), so a held stick or
trigger kept the player locked out.

  python scripts/control_cutout.py build/run/<run dir>
  python scripts/control_cutout.py build/run/<run dir> --markdown
  python scripts/control_cutout.py build/run/<run dir> --fixture tests/fixtures/control_cutout_2026-10-26.txt

Triggers: "recenter" (a tracking reset with a body-scale reading: Left A held 1 s), "stall"
(the stereo pairs paused, or no input poll for over 100 ms, just before), "controls
unavailable" (the trace has no controller sample: focus or tracking loss), else "unknown".
Newer traces name the cause themselves (input_armed / input_resumed "cause").

The trace records the stick, triggers, grips, trackpad and gestures but not the face buttons:
a disarmed sample with none of those held was waiting on an untraced button (after a
recenter, Left A still held).
"""
import argparse
import datetime
import json
import math
import re
from pathlib import Path

GAP = 0.1  # seconds: VrControllerInput's freshness limit
LOG_TIME = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\]")


def parse(lines):
    header, samples, events = None, [], []
    for line in lines:
        try:
            item = json.loads(line)
        except (ValueError, TypeError):
            continue  # a run cut off mid-line
        kind = item.get("type")
        if kind == "header":
            header = item
        elif kind == "sample":
            samples.append(item)
        elif kind == "event":
            events.append(item)
    return header, samples, events


def wall(header, t):
    if not header or "wall_clock_local" not in header:
        return f"{t:.3f}"
    base = datetime.datetime.fromisoformat(header["wall_clock_local"])
    return (base + datetime.timedelta(seconds=t)).strftime("%H:%M:%S.%f")[:-3]


def pair_pauses(log_lines, header):
    """(pause, resume) trace times of the stereo pairs stopping briefly (sm2vr.log)."""
    if not header or "wall_clock_local" not in header:
        return []
    base = datetime.datetime.fromisoformat(header["wall_clock_local"])
    pauses, start = [], None
    for line in log_lines:
        m = LOG_TIME.match(line)
        if not m:
            continue
        h, mi, s, ms = map(int, m.groups())
        when = base.replace(hour=h, minute=mi, second=s, microsecond=ms * 1000)
        if when < base - datetime.timedelta(hours=12):
            when += datetime.timedelta(days=1)
        t = (when - base).total_seconds()
        if "OrdinaryHeadsetRetention:" in line and " pairs=0 " in line:
            start = t  # the latest drop to no pairs before the resume
        elif "OrdinaryHeadsetResumed:" in line and start is not None:
            pauses.append((start, t))
            start = None
    return pauses


def grip_melee(log_lines):
    """Whether right-grip melee was on (the right grip only blocks re-arming then)."""
    for line in log_lines:
        if "melee_enabled=" in line:
            return "melee_enabled=true" in line
    return False


def held(sample, melee=False):
    """Names of the traced controls not at rest (VrControllerInput's neutral thresholds)."""
    i = sample.get("input") or {}
    c = i.get("controls")
    if not c:
        return None
    out = []
    mx, my = c["move"][0], c["move"][1]
    if math.hypot(mx, my) > 0.2:
        out.append("move")
    if abs(c["turn"]) > 0.3:
        out.append("turn")
    if c["fire"] > 0.05:
        out.append("fire")
    if c["aim"] > 0.05:
        out.append("aim")
    if melee and c["right_grip"] > 0.35:
        out.append("melee")
    if c["left_grip"] > 0.35:
        out.append("parry")
    if c["trackpad"] > 0.35:
        out.append("grenade")
    if (i.get("guard") or {}).get("held"):
        out.append("guard")
    if (i.get("grenade") or {}).get("holding"):
        out.append("grenade_throw")
    if i.get("swing_melee"):
        out.append("swing")
    return out


def windows(events):
    out, start = [], None
    for e in events:
        if e.get("event") != "input_armed":
            continue
        if not e["armed"]:
            if start is None:
                start = e
        elif start is not None:
            out.append({"start": start["t"], "end": e["t"], "reason": start.get("reason", ""), "cause": start.get("cause", "")})
            start = None
    return out


def classify(w, samples, events, pauses):
    a = w["start"]
    near = [e for e in events if abs(e["t"] - a) <= 0.05]
    names = {e["event"] for e in near}
    if w.get("cause"):
        return w["cause"], "named by the trace"
    if w.get("reason") and w["reason"] != "awaiting neutral controls":
        return "controls unavailable", w["reason"]
    if "tracking_reset" in names and "body_scale" in names:
        return "recenter", "Left A held 1 s: tracking reset with a body-scale reading"
    for p, r in pauses:
        if a - 0.5 <= r <= a + 0.05:
            return "stall", f"stereo pairs paused {(r - p) * 1000:.0f} ms"
    before = [s for s in samples if a - 0.6 <= s["t"] <= a + 0.05]
    gaps = [b["t"] - s["t"] for s, b in zip(before, before[1:])]
    if gaps and max(gaps) > GAP:
        return "stall", f"frame gap {max(gaps) * 1000:.0f} ms"
    ages = [(s.get("input") or {}).get("age_ms") or 0 for s in before]
    if ages and max(ages) > GAP * 1000:
        return "stall", f"input not polled for {max(ages):.0f} ms"
    return "unknown", ""


def analyse(lines, log_lines=()):
    header, samples, events = parse(lines)
    log_lines = list(log_lines)
    pauses = pair_pauses(log_lines, header)
    melee = grip_melee(log_lines)
    rows = []
    for w in windows(events):
        trigger, detail = classify(w, samples, events, pauses)
        inside = [s for s in samples if w["start"] <= s["t"] < w["end"] and s.get("input")]
        counts, untraced = {}, 0
        for s in inside:
            h = held(s, melee)
            if h is None:
                continue
            if not h:
                untraced += 1
            for name in h:
                counts[name] = counts.get(name, 0) + 1
        last = held(inside[-1], melee) if inside else None
        rows.append({
            "start": w["start"], "end": w["end"], "seconds": round(w["end"] - w["start"], 3),
            "start_wall": wall(header, w["start"]), "end_wall": wall(header, w["end"]),
            "trigger": trigger, "detail": detail, "samples": len(inside),
            "held": counts, "untraced": untraced,
            "released": (last if last else ["an untraced button"]) if last is not None else ["controls returned"],
        })
    return {"header": header, "melee": melee, "pauses": len(pauses), "windows": rows}


def blocking(row):
    n = max(row["samples"], 1)
    parts = [f"{name} {100 * k // n}%" for name, k in sorted(row["held"].items(), key=lambda kv: -kv[1])]
    if row["untraced"]:
        parts.append(f"untraced button {100 * row['untraced'] // n}%")
    return ", ".join(parts) or "-"


def text(report):
    out = []
    for r in report["windows"]:
        out.append(f"{r['start_wall']} {r['seconds']:6.3f}s {r['trigger']:<10} {r['detail']:<40} held: {blocking(r)}; re-armed on release of {'+'.join(r['released'])}")
    by = {}
    for r in report["windows"]:
        by.setdefault(r["trigger"], []).append(r["seconds"])
    out.append("summary: " + "; ".join(f"{k} {len(v)} (total {sum(v):.1f} s, longest {max(v):.1f} s)" for k, v in sorted(by.items())))
    return "\n".join(out)


def markdown(report):
    out = ["| Start | End | Seconds | Trigger | Detail | Held while disarmed (share of samples) | Re-armed when released |", "|---|---|---|---|---|---|---|"]
    for r in report["windows"]:
        out.append(f"| {r['start_wall']} | {r['end_wall']} | {r['seconds']:.3f} | {r['trigger']} | {r['detail']} | {blocking(r)} | {' + '.join(r['released'])} |")
    return "\n".join(out)


def fixture(report, samples, source, melee=False, pad=0.2):
    """Replay fixture for tests/test_vr_controller_input.cpp: per window a "w" line, then the
    traced controls of each sample from just before to just after it. untraced=1 marks a
    disarmed sample with no traced control held (an untraced button: Left A after a recenter)."""
    lines = [f"# {source}: input disarm windows, cut by scripts/control_cutout.py",
             "# w index trigger start end recorded_seconds",
             "# s index t move_x move_y turn fire aim right_grip left_grip trackpad guard grenade_holding untraced raw_right raw_left loadout",
             "# fire/aim are as routed to the input (VrCombatRouting); raw_right/raw_left are the triggers as held;",
             "# loadout p(rimary), s(econdary) or u(nknown)."]
    for k, r in enumerate(report["windows"]):
        lines.append(f"w {k} {r['trigger'].replace(' ', '_')} {r['start']:.3f} {r['end']:.3f} {r['seconds']:.3f}")
        for s in samples:
            if not (r["start"] - pad <= s["t"] <= r["end"] + pad):
                continue
            i = s.get("input") or {}
            c = i.get("controls")
            if not c:
                continue
            h = held(s, melee)
            raw = i.get("raw_controls") or {}
            untraced = int(r["start"] <= s["t"] < r["end"] and not h)  # disarmed throughout; the record at the start can be the stale one
            lines.append(f"s {k} {s['t']:.3f} {c['move'][0]:.3f} {c['move'][1]:.3f} {c['turn']:.3f} {c['fire']:.3f} {c['aim']:.3f} "
                         f"{c['right_grip']:.3f} {c['left_grip']:.3f} {c['trackpad']:.3f} {int(bool((i.get('guard') or {}).get('held')))} "
                         f"{int(bool((i.get('grenade') or {}).get('holding')))} {untraced} "
                         f"{raw.get('right_trigger', c['fire']):.3f} {raw.get('left_trigger', c['aim']):.3f} {(i.get('loadout') or 'u')[0]}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", type=Path, help="run directory (sm2vr_trace.jsonl, sm2vr.log) or a trace file")
    parser.add_argument("--markdown", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--fixture", type=Path, help="write the replay fixture here")
    args = parser.parse_args(argv)
    trace = args.run / "sm2vr_trace.jsonl" if args.run.is_dir() else args.run
    log = trace.parent / "sm2vr.log"
    lines = trace.read_text(encoding="utf-8", errors="replace").splitlines()
    log_lines = log.read_text(encoding="utf-8", errors="replace").splitlines() if log.exists() else []
    report = analyse(lines, log_lines)
    if args.fixture:
        _, samples, _ = parse(lines)
        source = "/".join(trace.parts[-3:])
        args.fixture.write_text(fixture(report, samples, source, report["melee"]), encoding="utf-8", newline="\n")
    if args.json:
        print(json.dumps({k: v for k, v in report.items() if k != "header"}, indent=1))
    elif args.markdown:
        print(markdown(report))
    else:
        print(text(report))


if __name__ == "__main__":
    main()
