"""Brief pauses of the owned stereo pairs, and what started each (docs/STEREO_PAUSES_2026-10-26.md).

A pause is the log's `OrdinaryHeadsetRetention ... pairs=0` followed by `OrdinaryHeadsetResumed`:
the producer rendered one or more native mono dispatches between two stereo pairs
(`dispatch - previous - 1` of them), so the headset got no new frame for that long.

  python scripts/stereo_pauses.py build/run/<run dir>
  python scripts/stereo_pauses.py build/run/<run dir> --markdown
  python scripts/stereo_pauses.py build/run/<run dir> --fixture tests/fixtures/stereo_pauses_2026-10-26.txt

Causes, first match wins:
  suspended  owned stereo was suspended (a load, a cutscene start, a rebuilt pool).
  foreign    a game PIP view held owned stereo off (ForeignViewHold, a crash guard).
  viewer     photo mode, the viewer or the pause menu came or went (by design leaving them
             settles over three jobs; frame-clock skips are common in photo mode).
  hitch      the game produced no frame for over 100 ms (a trace frame gap, or the player pose
             over 100 ms old): owned readiness refuses the stale player state.
  clock      the frame clock had no unique open job (`clock_skips` rose: OrdinaryClockGap).
  freshness  owned readiness refused with no hitch the trace can see: at 13-37 fps the player
             state is often just over 100 ms old by the time the render job checks it.
  single     one mono dispatch and no readiness refusal: a provider-age retry or a transient
             packet refusal; it costs one frame.
  unknown    none of the above.

Readiness then wants three jobs in a row with the same owner (OwnedStereoReadiness) before
the next pair, so a one-job refusal costs three mono dispatches. The `trigger` column is the
mono dispatches before those two settling jobs (`mono - 2`) for hitch, clock and freshness.
The periodic producer counters come every 120 presents (about 4 s), so a count can include
a refusal elsewhere in that interval; the trace is what places the hitch.
"""
from __future__ import annotations

import argparse
import datetime
import json
import re
import statistics
from pathlib import Path

LINE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\] \[([A-Z])\] \[tid:\d+\] ([A-Za-z0-9_]+):(.*)$")
FIELD = re.compile(r"(\w+)=(\S+)")
TRACE_T = re.compile(r'^\{"t":\s*([0-9.]+),\s*"type":\s*"(\w+)"')
STALE_MS = 100.0  # OwnedStereoReadiness: a pose or owner older than this is refused
PHOTO = 0x100  # EngineCinematicState: photo mode (0x108) and the pause menu (0x908)
RESUMABLE = ("hitch", "clock", "freshness")  # causes the resume window shortens
SETTLE = 2  # jobs refused while readiness counts back up to three after a one-job refusal
PERIODIC = ("OrdinaryOwnedReadiness", "OrdinaryHeadsetProducer", "ForeignViewHold")
# Log lines worth showing next to a pause (the rest is periodic status).
CONTEXT = ("OrdinaryClockGap", "VrScreenMenu", "VrFlatFallback", "OrdinaryHeadsetRestarted", "OrdinaryBloomRenewed",
           "OrdinaryHeadsetSuspended", "OrdinaryHeadsetRecovered", "OrdinaryProviderFailure", "HangWatchdog",
           "ReadinessStall", "CameraHandover", "StereoForeignView", "OrdinaryHeadsetRearm", "OrdinaryHeadsetColdRestart")
TRACE_EVENTS = {"animation_takeover_start", "animation_takeover_end", "camera_mode", "snap_turn", "tracking_reset",
                "game_state", "actor", "sprint_camera_start", "sprint_camera_end", "ride_start", "ride_end"}


def seconds(h, mi, s, ms):
    return int(h) * 3600 + int(mi) * 60 + int(s) + int(ms) / 1000


def clock(t):
    t %= 86400
    return f"{int(t // 3600):02d}:{int(t % 3600 // 60):02d}:{t % 60:06.3f}"


def parse_log(lines):
    """(seconds of day, kind, fields, text) of every mod line."""
    rows, last = [], None
    for line in lines:
        m = LINE.match(line)
        if not m:
            continue
        t = seconds(*m.groups()[:4])
        if last is not None and t < last - 43200:
            t += 86400  # past midnight
        last = t
        rest = m.group(7).strip()
        rows.append((t, m.group(6), dict(FIELD.findall(rest)), rest))
    return rows


def pauses(rows):
    out, start = [], None
    for t, kind, f, _ in rows:
        if kind == "OrdinaryHeadsetRetention" and f.get("pairs") == "0":
            start = t  # the latest drop to no pairs before the resume
        elif kind == "OrdinaryHeadsetResumed" and start is not None:
            dispatch, previous = int(f["dispatch"]), int(f["previous"])
            out.append({"start": start, "end": t, "ms": round((t - start) * 1000), "dispatch": dispatch,
                        "previous": previous, "mono": dispatch - previous - 1})
            start = None
    return out


def counter_deltas(rows, start, end):
    """Change in the periodic producer counters from the last line before `start` to the first after `end`."""
    def bracket(kind):
        before = after = None
        for t, k, f, _ in rows:
            if k != kind:
                continue
            if t <= start:
                before = f
            elif t >= end and after is None:
                after = f
        return before, after
    out = {}
    for kind, names in (("OrdinaryOwnedReadiness", ("blocked", "owner_changes")),
                        ("OrdinaryHeadsetProducer", ("clock_skips", "suspended_jobs")),
                        ("ForeignViewHold", ("held_jobs",))):
        a, b = bracket(kind)
        for n in names:
            try:
                out[n] = int(b[n]) - int(a[n]) if a and b else None
            except (KeyError, ValueError):
                out[n] = None
        if kind == "OrdinaryOwnedReadiness":
            out["reason"] = b.get("reason") if b else None
    return out


def trace_windows(lines, windows, before=0.5):
    """Per window [start-before, end] (seconds of day): longest frame gap, oldest player pose and head
    sample, game flags, takeovers and trace events. Streams the trace; parses only nearby lines."""
    base = None
    out = [{"gap_ms": 0.0, "pose_ms": 0.0, "head_ms": 0.0, "flags": set(), "takeover": False, "events": [], "samples": 0,
            "last": None} for _ in windows]
    spans = [(w["start"] - before, w["end"]) for w in windows]
    lo = min((a for a, _ in spans), default=0)
    hi = max((b for _, b in spans), default=0)
    for line in lines:
        m = TRACE_T.match(line)
        if not m:
            continue
        if m.group(2) == "header":
            w = datetime.datetime.fromisoformat(json.loads(line)["wall_clock_local"])
            base = w.hour * 3600 + w.minute * 60 + w.second + w.microsecond / 1e6
            continue
        if base is None:
            continue
        t = float(m.group(1)) + base
        if t < lo - 1 or t > hi + 1:
            continue
        hits = [k for k, (a, b) in enumerate(spans) if a - 0.2 <= t <= b]
        if not hits:
            continue
        item = json.loads(line)
        for k in hits:
            o = out[k]
            a, b = spans[k]
            if item["type"] == "sample":
                if o["last"] is not None and t > a:
                    o["gap_ms"] = max(o["gap_ms"], (t - o["last"]) * 1000)
                o["last"] = t
                if t < a:
                    continue
                o["samples"] += 1
                i, c, g = item.get("input") or {}, item.get("character") or {}, item.get("game") or {}
                o["pose_ms"] = max(o["pose_ms"], c.get("age_ms") or 0)
                o["head_ms"] = max(o["head_ms"], i.get("age_ms") or 0)
                o["flags"].add(g.get("flags", 0))
                o["takeover"] |= bool(c.get("animation_takeover"))
            elif item["type"] == "event" and item.get("event") in TRACE_EVENTS and t >= a:
                o["events"].append(item["event"])
    for o in out:
        o.pop("last")
        o["flags"] = sorted(o["flags"])
    return out


def classify(p):
    c, tr = p["counters"], p.get("trace") or {}
    if (c.get("suspended_jobs") or 0) > 0:
        return "suspended", f"{c['suspended_jobs']} suspended jobs"
    if (c.get("held_jobs") or 0) > 0:
        return "foreign", f"{c['held_jobs']} jobs held for a PIP view"
    if "camera_mode" in tr.get("events", ()) or any(f & PHOTO for f in tr.get("flags", ())):
        return "viewer", f"photo/viewer or pause menu change ({c.get('clock_skips')} clock skips)"
    hitch = max(tr.get("gap_ms", 0), tr.get("pose_ms", 0))
    if hitch > STALE_MS:
        return "hitch", f"no game frame for {hitch:.0f} ms"
    if (c.get("clock_skips") or 0) > 0:
        return "clock", f"{c['clock_skips']} clock skips in the interval"
    if p["mono"] == 1 and not (c.get("blocked") or 0):
        return "single", "one mono dispatch, no readiness refusal"
    if (c.get("blocked") or 0) > 0:
        return "freshness", f"refused {c['blocked']} (sampled reason {c.get('reason')})"
    return "unknown", ""


def analyse(log_lines, trace_lines=()):
    rows = parse_log(log_lines)
    found = pauses(rows)
    for p in found:
        p["counters"] = counter_deltas(rows, p["start"] - 0.15, p["end"])
        p["context"] = [f"{clock(t)} {k} {text[:90]}" for t, k, _, text in rows
                        if p["start"] - 1.0 <= t <= p["end"] + 0.2 and k in CONTEXT]
    traces = trace_windows(trace_lines, found) if trace_lines else [None] * len(found)
    for p, tr in zip(found, traces):
        p["trace"] = tr
        p["cause"], p["detail"] = classify(p)
        p["trigger"] = p["mono"] - SETTLE if p["cause"] in RESUMABLE and p["mono"] > SETTLE else None
    return found


def summary(found):
    out = {}
    for p in found:
        out.setdefault(p["cause"], []).append(p)
    rows = []
    for cause, ps in sorted(out.items(), key=lambda kv: -len(kv[1])):
        ms = [p["ms"] for p in ps]
        mono = [p["mono"] for p in ps]
        rows.append({"cause": cause, "count": len(ps), "median_ms": statistics.median(ms), "max_ms": max(ms),
                     "total_ms": sum(ms), "median_mono": statistics.median(mono), "settle_jobs": sum(SETTLE for p in ps if p["trigger"])})
    return rows


def flags_text(tr):
    return ",".join(f"{f:#x}" for f in tr["flags"]) if tr else ""


def text(found):
    out = []
    for p in found:
        tr = p["trace"] or {}
        out.append(f"{clock(p['start'])} {p['ms']:5d} ms mono={p['mono']:3d} {p['cause']:<9} {p['detail']:<40} "
                   f"gap={tr.get('gap_ms', 0):5.0f} pose={tr.get('pose_ms', 0):6.0f} head={tr.get('head_ms', 0):6.0f} "
                   f"flags={flags_text(tr)} takeover={tr.get('takeover')} {' '.join(sorted(set(tr.get('events', []))))}")
        out.extend("      " + c for c in p["context"])
    out.append("")
    for r in summary(found):
        out.append(f"{r['cause']:<9} {r['count']:3d} pauses, median {r['median_ms']:.0f} ms, longest {r['max_ms']} ms, "
                   f"total {r['total_ms'] / 1000:.1f} s, median {r['median_mono']} mono dispatches, {r['settle_jobs']} of them settling")
    out.append(f"all       {len(found):3d} pauses, median {statistics.median([p['ms'] for p in found]) if found else 0:.0f} ms")
    return "\n".join(out)


def markdown(found):
    out = ["| Cause | Pauses | Median ms | Longest ms | Total s | Median mono dispatches | Settling jobs |", "|---|---|---|---|---|---|---|"]
    for r in summary(found):
        out.append(f"| {r['cause']} | {r['count']} | {r['median_ms']:.0f} | {r['max_ms']} | {r['total_ms'] / 1000:.1f} | {r['median_mono']} | {r['settle_jobs']} |")
    return "\n".join(out)


def fixture(found, source):
    """Replay fixture for tests/test_owned_stereo_readiness.cpp: one line per pause."""
    lines = [f"# {source}: owned stereo pauses, cut by scripts/stereo_pauses.py",
             "# p index cause start_seconds_of_day recorded_ms recorded_mono trigger_jobs",
             "# trigger_jobs: mono dispatches before readiness's two settling jobs (hitch, clock, freshness), else -1."]
    for k, p in enumerate(found):
        lines.append(f"p {k} {p['cause']} {p['start']:.3f} {p['ms']} {p['mono']} {p['trigger'] if p['trigger'] is not None else -1}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", type=Path, help="run directory (sm2vr.log, sm2vr_trace.jsonl)")
    parser.add_argument("--markdown", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--fixture", type=Path, help="write the replay fixture here")
    args = parser.parse_args(argv)
    log = args.run / "sm2vr.log" if args.run.is_dir() else args.run
    trace = log.parent / "sm2vr_trace.jsonl"
    log_lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    if trace.exists():
        with trace.open(encoding="utf-8", errors="replace") as handle:
            found = analyse(log_lines, handle)
    else:
        found = analyse(log_lines)
    if args.fixture:
        args.fixture.write_text(fixture(found, "/".join(log.parts[-3:-1])), encoding="utf-8", newline="\n")
    if args.json:
        print(json.dumps(found, indent=1, default=list))
    elif args.markdown:
        print(markdown(found))
    else:
        print(text(found))


if __name__ == "__main__":
    main()
