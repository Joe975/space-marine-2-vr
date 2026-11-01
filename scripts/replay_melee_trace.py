"""Replay a playtest trace through the production sword rules, old and new.

Exports the trace's head and hand poses, triggers, grip, loadout and armed state, runs the
native `melee_replay` tool (build/mod/melee_replay.exe, built from scripts/melee_replay.cpp
with the same VrSwingMelee / VrHolster / VrGuard code the mod runs) with the 20 Oct rules
(`--old`) and the current ones, and lists every fast right-hand motion made with the sword
out and whether each set of rules turned it into an attack.

The replay runs at the trace's sample rate (about 22-30 Hz), not the input poll rate, so
speeds read lower than live and the counts are a conservative estimate.

Usage: replay_melee_trace.py TRACE|SCENARIO.json [--tool EXE] [--json] [--assume-secondary]
"""
from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TOOL = ROOT / "build" / "mod" / "melee_replay.exe"
MOTION_WRIST, MOTION_TIP = 1.2, 2.0  # m/s at the trace rate: what counts as a fast motion


def _vec(v):
    return v if isinstance(v, list) and len(v) == 3 and all(isinstance(x, (int, float)) for x in v) else None


def export(lines, assume_secondary=False, infer_loadout=False):
    """Trace lines to replay rows: (t, text) with the tool's column layout.

    Traces before 21 Oct have no `loadout`. assume_secondary treats every such sample as sword
    out; infer_loadout reads it from the swing detector running (only with the pistol and sword)
    and the trigger routing (the right trigger routed to fire is the bolter; the left trigger
    routed to fire, or a chainsword rev, is the pistol and sword), carried forward until a
    weapon switch. Without either they count as unknown.
    """
    rows, inferred = [], -1
    for raw in lines:
        if infer_loadout and '"down_switch_weapon","down":true' in raw:
            inferred = -1
        if '"sample"' not in raw:
            continue
        try:
            r = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if r.get("type") != "sample":
            continue
        i = r.get("input") or {}
        head, hf = _vec(i.get("head")), _vec(i.get("head_forward"))
        if not head or not hf:
            continue
        rc = i.get("raw_controls") or {}
        loadout = i.get("loadout")
        if "loadout" in i:
            code = 1 if loadout in ("secondary", True) else 0 if loadout in ("primary", False) else -1
        elif assume_secondary:
            code = 1
        elif infer_loadout:
            routed, rt, lt = i.get("controls") or {}, rc.get("right_trigger", 0) or 0, rc.get("left_trigger", 0) or 0
            # The swing detector only runs with the pistol/chainsword loadout, so its speed is
            # the strongest evidence; then the trigger routing.
            if (i.get("swing_speed") or 0) > 0 or (routed.get("chainsword_rev") or 0) > 0.05 or (lt > 0.3 and rt < 0.05 and (routed.get("fire") or 0) >= lt - 0.05):
                inferred = 1
            elif rt > 0.3 and (routed.get("fire") or 0) >= rt - 0.05:
                inferred = 0
            code = inferred
        else:
            code = -1
        secondary = code

        def hand(side):
            p, f = _vec(i.get(side)), _vec(i.get(side + "_forward"))
            return (1, p + f) if p and f else (0, [0.0] * 6)
        rv, right = hand("right")
        lv, left = hand("left")
        cols = [f"{r['t']:.4f}", str(int(i.get("epoch") or 0)), str(int(bool(i.get("armed")))), str(secondary),
                f"{rc.get('right_trigger', 0) or 0:.4f}", f"{rc.get('right_grip', (i.get('controls') or {}).get('right_grip', 0)) or 0:.4f}", f"{rc.get('left_trigger', 0) or 0:.4f}",
                *[f"{x:.5f}" for x in head + hf], str(rv), *[f"{x:.5f}" for x in right], str(lv), *[f"{x:.5f}" for x in left],
                str(int(bool(i.get("support_aim")))), f"{rc.get('left_grip', (i.get('controls') or {}).get('left_grip', 0)) or 0:.4f}"]
        rows.append({"t": r["t"], "text": " ".join(cols), "secondary": secondary == 1, "loadout": secondary, "head": head, "right": right if rv else None,
                     "rt": rc.get("right_trigger", 0) or 0, "lt": rc.get("left_trigger", 0) or 0})
    return rows


def motions(rows, loadout=1):
    """Fast right-hand motions (head-relative wrist or 0.45 m tip) made with one loadout out
    (1 pistol/chainsword, 0 bolter)."""
    out, cur = [], None
    for a, b in zip(rows, rows[1:]):
        dt = b["t"] - a["t"]
        if not (0 < dt <= 0.1 and a["right"] and b["right"] and a.get("loadout", int(a["secondary"])) == loadout and b.get("loadout", int(b["secondary"])) == loadout):
            continue
        wa = [a["right"][k] - a["head"][k] for k in range(3)]
        wb = [b["right"][k] - b["head"][k] for k in range(3)]
        ta = [wa[k] + a["right"][3 + k] * 0.45 for k in range(3)]
        tb = [wb[k] + b["right"][3 + k] * 0.45 for k in range(3)]
        ws, ts = math.dist(wa, wb) / dt, math.dist(ta, tb) / dt
        if ws < MOTION_WRIST and ts < MOTION_TIP:
            continue
        if cur and b["t"] - cur["end"] <= 0.15:
            cur.update(end=b["t"], wrist=max(cur["wrist"], ws), tip=max(cur["tip"], ts))
        else:
            cur = {"start": a["t"], "end": b["t"], "wrist": ws, "tip": ts}
            out.append(cur)
    return out


def parse(text):
    events, summary = [], {}
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "summary":
            summary = dict(p.split("=") for p in parts[1:])
        else:
            events.append({"kind": parts[0], "t": float(parts[1]), "detail": parts[2:]})
    return events, summary


def run(tool, samples, old, extra=()):
    """extra: further melee_replay flags, e.g. ("--guard-belt-latch",) or ("--set", "guard.min_angle_degrees=50")."""
    result = subprocess.run([str(tool), str(samples)] + (["--old"] if old else []) + list(extra), capture_output=True, text=True, check=True)
    return parse(result.stdout)


def compare(rows, old, new):
    report = {"old": old[1], "new": new[1], "motions": []}
    for m in sorted(motions(rows, 1) + [dict(x, gun=True) for x in motions(rows, 0)], key=lambda x: x["start"]):
        lo, hi = m["start"] - 0.15, m["end"] + 0.15

        def outcome(events):
            hits = [e for e in events if lo <= e["t"] <= hi and e["kind"] in ("swing", "veto", "flick")]
            return ", ".join(f"{e['kind']}{' ' + e['detail'][0] if e['kind'] == 'swing' else ''}" for e in hits) or "nothing"
        report["motions"].append({"start": round(m["start"], 3), "end": round(m["end"], 3), "wrist": round(m["wrist"], 1), "tip": round(m["tip"], 1),
                                  "loadout": "bolter" if m.get("gun") else "sword", "old": outcome(old[0]), "new": outcome(new[0])})
    report["old_swings"] = [e["t"] for e in old[0] if e["kind"] == "swing"]
    report["new_swings"] = [e["t"] for e in new[0] if e["kind"] == "swing"]
    report["new_gun_swings"] = [e["t"] for e in new[0] if e["kind"] == "swing" and e["detail"][-1:] == ["gun"]]
    report["bolter_seconds"] = round(sum(b["t"] - a["t"] for a, b in zip(rows, rows[1:]) if a.get("loadout") == 0 and 0 < b["t"] - a["t"] <= 0.1), 1)
    return report


def markdown(report):
    out = ["| motion (s) | loadout | wrist / tip m/s | 20 Oct rules | new rules |", "| --- | --- | --- | --- | --- |"]
    for m in report["motions"]:
        out.append(f"| {m['start']:.2f}-{m['end']:.2f} | {m.get('loadout', 'sword')} | {m['wrist']:.1f} / {m['tip']:.1f} | {m['old']} | {m['new']} |")
    out.append("")
    for name in ("old", "new"):
        s = report[name]
        out.append(f"{name}: swings {s.get('swings')} (with the bolter {s.get('gun', 0)}), power {s.get('power')}, vetoes {s.get('vetoes')}, flicks {s.get('flicks')}, "
                   f"wind-ups {s.get('windups')}, guard starts {s.get('guards')}, left pushes {s.get('pushes', 0)}, holster selections {s.get('selections', 0)}")
    if "bolter_seconds" in report:
        out.append(f"bolter out: {report['bolter_seconds']} s; new swings with the bolter at {report.get('new_gun_swings')}")
    return "\n".join(out)


def scenario_lines(script, hz=90, loadout="secondary"):
    """A test-driver scenario (config/test_driver/*.json) as synthetic trace sample lines: a
    level head at 1.7 m facing -Z, wrists interpolated between keyframes, the sword out. Lets a
    scenario's expected outcome be checked offline before it is run live."""
    frames, head = script["frames"], [0.0, 1.7, 0.0]
    left0, right0 = [-.25, -.35, -.45, 0, 0, 0], [.25, -.35, -.45, 0, 0, 0]

    def pose_at(ms):
        for a, b in zip(frames, frames[1:]):
            if a["ms"] <= ms <= b["ms"]:
                u = (ms - a["ms"]) / max(b["ms"] - a["ms"], 1)
                lerp = lambda k, d: [x + (y - x) * u for x, y in zip(a.get(k, d), b.get(k, d))]
                return lerp("left", left0), lerp("right", right0), a
        last = frames[-1]
        return last.get("left", left0), last.get("right", right0), last

    def hand(p):
        yaw = math.radians(p[3])
        return [head[0] + p[0], head[1] + p[1], head[2] + p[2]], [-math.sin(yaw), 0.0, -math.cos(yaw)]
    out, ms = [], 0.0
    while ms <= script["duration_ms"]:
        left, right, frame = pose_at(ms)
        (lp, lf), (rp, rf) = hand(left), hand(right)
        out.append(json.dumps({"t": round(ms / 1000, 4), "type": "sample", "input": {
            "epoch": 1, "armed": True, "loadout": loadout, "head": head, "head_forward": [0, 0, -1],
            "right": rp, "right_forward": rf, "left": lp, "left_forward": lf,
            "raw_controls": {"right_trigger": frame.get("fire", 0), "right_grip": frame.get("right_grip", 0), "left_trigger": frame.get("aim", 0),
                             "left_grip": frame.get("left_grip", 0)}}}))
        ms += 1000 / hz
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace", type=Path)
    parser.add_argument("--tool", type=Path, default=DEFAULT_TOOL)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--assume-secondary", action="store_true", help="for traces without a loadout field")
    parser.add_argument("--infer-loadout", action="store_true", help="for traces without a loadout field: read it from the trigger routing")
    parser.add_argument("--scenario-loadout", default="secondary", choices=("secondary", "primary"), help="loadout a driver scenario assumes")
    parser.add_argument("--samples-out", type=Path, help="also keep the exported samples (melee_replay input) here")
    args = parser.parse_args(argv)
    if args.trace.suffix == ".json":  # A test-driver scenario rather than a trace.
        rows = export(scenario_lines(json.loads(args.trace.read_text(encoding="utf-8")), loadout=args.scenario_loadout))
    else:
        with args.trace.open(encoding="utf-8", errors="replace") as f:
            rows = export(f, args.assume_secondary, args.infer_loadout)
    with tempfile.TemporaryDirectory() as tmp:
        samples = Path(tmp) / "samples.txt"
        samples.write_text("\n".join(r["text"] for r in rows) + "\n", encoding="ascii")
        if args.samples_out:
            args.samples_out.write_text(samples.read_text(encoding="ascii"), encoding="ascii")
        report = compare(rows, run(args.tool, samples, True), run(args.tool, samples, False))
    print(json.dumps(report, indent=1) if args.json else markdown(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
