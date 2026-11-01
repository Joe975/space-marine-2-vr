"""Grenade launch direction versus where the player looked, from one run folder.

Reads build/run/<run>/sm2vr_trace.jsonl (thrown grenade_release events, the gaze in the
native world at that moment, snap turns, grenade_aim) and sm2vr.log (GrenadeRedirect
before/after per redirect, or the older 5 s GrenadeLaunch summary; RigidVelocity first native flight update when
-GrenadeVelocityProbe was on). Angles are horizontal, degrees, signed about +Y.

Usage: python -I scripts/grenade_aim_report.py RUN_DIR [--json OUT]
"""
from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path

REDIRECT = re.compile(r"GrenadeRedirect: serial=(\d+) .*?before=\[([^\]]*)\] after=\[([^\]]*)\]")
LAUNCH = re.compile(r"GrenadeLaunch: redirected=(\d+) refused=(\d+) .*?before=\[([^\]]*)\] after=\[([^\]]*)\]")
VELOCITY = re.compile(r"RigidVelocity: serial=(\d+) .*?caller=([0-9a-f]+) name=(\S+) velocity=\[([^\]]*)\]")
LAUNCH_CALLER = "12aed3e"  # Script RigidBody.SetVelocity launch (NativeGrenadeLaunch.h).


def vector(text: str) -> list[float]:
    return [float(v) for v in text.split(",")]


def horizontal_angle(a, b) -> float | None:
    """Horizontal rotation from b to a about +Y (right-hand rule), degrees; None if vertical."""
    if a is None or b is None or math.hypot(a[0], a[2]) < 1e-6 or math.hypot(b[0], b[2]) < 1e-6:
        return None
    return math.degrees(math.atan2(b[2] * a[0] - b[0] * a[2], a[0] * b[0] + a[2] * b[2]))


def read_trace(lines) -> list[dict]:
    """Thrown releases with the gaze, body yaw and snap count at that moment."""
    throws: list[dict] = []
    camera = None
    snaps = 0
    for line in lines:
        try:
            record = json.loads(line)
        except ValueError:
            continue  # The last line of a killed game can be cut off.
        if record.get("type") == "sample":
            camera = record.get("camera") or camera
            continue
        event = record.get("event")
        if event == "snap_turn":
            snaps = record.get("count", snaps)
        elif event == "grenade_release" and record.get("outcome") == "thrown":
            throws.append({"t": record["t"], "snaps": snaps, "velocity": record.get("velocity"),
                           "gaze": (camera or {}).get("eye_forward"), "body_yaw": (camera or {}).get("body_yaw")})
        elif event == "grenade_aim" and throws:
            throws[-1]["aim"] = {k: record[k] for k in ("yaw_degrees", "pitch_degrees", "hand_yaw_degrees", "hand_pitch_degrees", "hand_speed", "speed_scale") if k in record}
    return throws


def read_log(lines) -> tuple[list[dict], list[dict]]:
    """Distinct redirects (in order) and native grenade velocity samples."""
    redirects: list[dict] = []
    exact: dict[int, dict] = {}
    samples: list[dict] = []
    for line in lines:
        match = REDIRECT.search(line)
        if match:
            exact[int(match.group(1))] = {"redirected": int(match.group(1)), "before": vector(match.group(2)), "after": vector(match.group(3))}
            continue
        match = LAUNCH.search(line)
        if match:
            count = int(match.group(1))
            if not redirects or count > redirects[-1]["redirected"]:
                redirects.append({"redirected": count, "refused": int(match.group(2)),
                                  "before": vector(match.group(3)), "after": vector(match.group(4))})
            continue
        match = VELOCITY.search(line)
        if match and match.group(3).startswith("grenade_"):
            samples.append({"serial": int(match.group(1)), "caller": match.group(2), "name": match.group(3), "velocity": vector(match.group(4))})
    if exact:  # Per-redirect lines are complete; the summaries can skip a redirect.
        refused = redirects[-1]["refused"] if redirects else 0
        redirects = [dict(exact[k], refused=refused) for k in sorted(exact)]
    return redirects, samples


def first_flight(samples: list[dict], index: int, launch: list[float]) -> list[float] | None:
    """The flight update right after the index-th launch sample, if that sample carried the
    native `launch` velocity (the probe records the value native code passed in)."""
    launches = [i for i, sample in enumerate(samples) if sample["caller"] == LAUNCH_CALLER]
    if index >= len(launches):
        return None
    at = launches[index]
    if max(abs(a - b) for a, b in zip(samples[at]["velocity"], launch)) >= 1e-3:
        return None
    following = samples[at + 1] if at + 1 < len(samples) else None
    return following["velocity"] if following and following["caller"] != LAUNCH_CALLER else None


def report(trace_lines, log_lines) -> dict:
    throws = read_trace(trace_lines)
    redirects, samples = read_log(log_lines)
    rows = []
    for i, throw in enumerate(throws):
        row = dict(throw)
        if i < len(redirects):
            r = redirects[i]
            row["native_before"], row["redirected_after"] = r["before"], r["after"]
            row["native_before_from_gaze_degrees"] = horizontal_angle(r["before"], throw["gaze"])
            row["launch_from_gaze_degrees"] = horizontal_angle(r["after"], throw["gaze"])
            native_speed = math.hypot(*r["before"])
            row["launch_speed_ratio"] = math.hypot(*r["after"]) / native_speed if native_speed > 1e-6 else None
            flight = first_flight(samples, i, r["before"])
            if flight:
                row["first_flight"] = flight
                row["flight_from_gaze_degrees"] = horizontal_angle(flight, throw["gaze"])
        rows.append(row)
    return {"throws": rows, "redirects": len(redirects), "refused": redirects[-1]["refused"] if redirects else 0}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("run", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    trace = (args.run / "sm2vr_trace.jsonl").read_text(encoding="utf-8", errors="replace").splitlines()
    log_path = args.run / "sm2vr.log"
    log = log_path.read_text(encoding="utf-8", errors="replace").splitlines() if log_path.exists() else []
    result = report(trace, log)
    for row in result["throws"]:
        fmt = lambda v: "-" if v is None else f"{v:7.1f}"
        print(f"t={row['t']:8.2f} snaps={row['snaps']:2d} body_yaw={fmt(row.get('body_yaw'))} "
              f"native_vs_gaze={fmt(row.get('native_before_from_gaze_degrees'))} launch_vs_gaze={fmt(row.get('launch_from_gaze_degrees'))} "
              f"speed_vs_native={fmt(row.get('launch_speed_ratio'))} "
              f"flight_vs_gaze={fmt(row.get('flight_from_gaze_degrees'))} aim={row.get('aim')}")
    print(f"redirects={result['redirects']} refused={result['refused']}")
    if args.json:
        args.json.write_text(json.dumps(result, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
