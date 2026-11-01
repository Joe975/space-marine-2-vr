"""Queue bounded controller/pose scripts for a -TestDriver game launch.

JSON keyframes are complete states (omitted inputs release), with stepwise actions
and linearly interpolated hand/head poses. Pose = [x,y,z,yaw,pitch,roll], metres and
degrees, XR -Z forward. Hands are relative to the current level HMD. Scripted head
is relative to the initial level HMD and needs -ScriptedHead at launch.
"""
from __future__ import annotations
import argparse
import json
import math
import os
from pathlib import Path
import uuid

ROOT = Path(__file__).resolve().parents[1]
BUTTONS = {"dodge": 1, "interact": 2, "sprint": 4, "menu": 8, "recenter": 16,
           "finisher": 32, "fire_touch": 64, "fire_touch_valid": 128}
DEFAULT_LEFT = [-.25, -.35, -.45, 0, 0, 0]
DEFAULT_RIGHT = [.25, -.35, -.45, 0, 0, 0]


def vector(value, size, low, high):
    if not isinstance(value, list) or len(value) != size:
        raise ValueError(f"expected {size} numbers")
    if any(isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x)
           or not low <= x <= high for x in value):
        raise ValueError(f"numbers must be finite in [{low}, {high}]")
    return value


def pose(value):
    vector(value, 6, -180, 180)
    vector(value[:3], 3, -2, 2)
    return value


def compile_script(script, request_id):
    if not isinstance(script, dict) or set(script) - {"mode", "duration_ms", "frames", "description"}:
        raise ValueError("unknown script fields")
    if not request_id or len(request_id) > 64 or any(not (c.isascii() and (c.isalnum() or c in "_-")) for c in request_id):
        raise ValueError("invalid request id")
    mode = script.get("mode", "live_head")
    duration = script.get("duration_ms")
    frames = script.get("frames")
    if mode not in ("live_head", "scripted_head") or type(duration) is not int or not 0 < duration <= 120000:
        raise ValueError("invalid mode or duration (maximum 120 seconds)")
    if not isinstance(frames, list) or not 1 <= len(frames) <= 2048:
        raise ValueError("expected 1..2048 frames")
    lines = [f"SM2VR_TEST 1 {request_id} {mode} {duration} {len(frames)}"]
    previous = -1
    for f in frames:
        if not isinstance(f, dict) or set(f) - {"ms", "move", "turn", "buttons", "fire", "aim", "right_grip", "left_grip", "utility", "left", "right", "head"}:
            raise ValueError("unknown keyframe field")
        ms = f.get("ms")
        if type(ms) is not int or not previous < ms <= duration or (previous == -1 and ms != 0):
            raise ValueError("frames must start at 0 and increase within duration")
        previous = ms
        buttons = f.get("buttons", [])
        if not isinstance(buttons, list) or any(not isinstance(b, str) or b not in BUTTONS for b in buttons) or len(set(buttons)) != len(buttons):
            raise ValueError("unknown or duplicate button")
        move = vector(f.get("move", [0, 0]), 2, -1, 1)
        turn = vector([f.get("turn", 0)], 1, -1, 1)
        analog = vector([f.get(k, 0) for k in ("fire", "aim", "right_grip", "left_grip")], 4, 0, 1)
        utility = vector(f.get("utility", [0, 0, 0]), 3, -1, 1)
        if utility[2] < 0:
            raise ValueError("utility press must be nonnegative")
        head = pose(f.get("head", [0]*6))
        if mode == "live_head" and any(head):
            raise ValueError("head override requires scripted_head")
        values = [ms, sum(BUTTONS[b] for b in buttons), *move, *turn, *analog, *utility,
                  *pose(f.get("left", DEFAULT_LEFT)), *pose(f.get("right", DEFAULT_RIGHT)), *head]
        lines.append(" ".join(str(v) for v in values))
    text = "\n".join(lines) + "\n"
    if len(text.encode("ascii")) > 262144:
        raise ValueError("request exceeds 256 KiB")
    return text


def mailbox(explicit=None):
    path = Path(explicit) if explicit else Path(json.loads((ROOT / "build/test_driver/current.json").read_text(encoding="utf-8-sig"))["directory"])
    if not path.is_absolute() or not path.is_dir():
        raise ValueError("mailbox must be an existing absolute directory from -TestDriver")
    return path


def write_request(directory, text):
    temporary = directory / (uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(text, encoding="ascii")
        os.replace(temporary, directory / "request.txt")
    finally:
        temporary.unlink(missing_ok=True)


def report_trace(lines, request_id):
    records = []
    for line in lines:
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass  # An active trace may end with one incomplete write.
    marks = [r for r in records if r.get("event") == "test_driver" and r.get("detail", "").startswith(request_id + " ")]
    running = next((r for r in marks if r["detail"].split()[1] == "running"), None)
    terminal = next((r for r in marks if r["detail"].split()[1] not in ("preroll", "running")), None)
    if not running:
        return {"id": request_id, "marks": marks, "error": "no running marker"}
    start, end = running["t"], terminal["t"] if terminal else float("inf")
    samples = [r for r in records if r.get("type") == "sample" and start <= r["t"] <= end
               and r.get("character") and r["character"]["age_ms"] <= 100]
    events = [r for r in records if r.get("type") == "event" and start <= r["t"] <= end]
    result = {"id": request_id, "marks": marks, "samples": len(samples), "events": events,
              "terminal": terminal["detail"].split()[1] if terminal else "pending"}
    if len(samples) < 2:
        result["error"] = "not enough fresh character observations"
        return result
    a, b = samples[0], samples[-1]
    if a["character"]["actor"] != b["character"]["actor"]:
        result["error"] = "actor changed; displacement is not comparable"
        return result
    def displacement(a, b):
        ca, cb = a["character"], b["character"]
        forward = ca["root_z_axis"]
        length = math.hypot(forward[0], forward[2])
        if length < 1e-6:
            return None
        fx, fz = forward[0]/length, forward[2]/length
        delta = [(cb["root"][i]-ca["root"][i])/ca["units_per_metre"] for i in range(3)]
        return {"right_metres": delta[0]*fz-delta[2]*fx, "forward_metres": delta[0]*fx+delta[2]*fz,
                "up_metres": delta[1]}
    result["root_displacement_initial_body_axes"] = displacement(a, b)
    result["root_displacement_initial_view_axes"] = view_displacement(a, b)
    result["camera_offset_before"] = a.get("metrics", {}).get("head_offset_metres")
    result["camera_offset_after"] = b.get("metrics", {}).get("head_offset_metres")
    result["dodge_windows"] = []
    for event in events:
        if event.get("event") != "button" or event.get("name") != "A_dodge_confirm" or not event.get("down"):
            continue
        before = [s for s in samples if s["t"] <= event["t"]]
        after = [s for s in samples if s["t"] >= event["t"]+1.2]
        if before and after:
            result["dodge_windows"].append({"t": event["t"], "actual": displacement(before[-1], after[0]),
                                           "actual_view": view_displacement(before[-1], after[0])})
    return result


def view_displacement(a, b):
    """Native body-facing target is the world-space VR view yaw, not actor yaw.

    At yaw 180 the view faces world -Z and screen-right is +X. Actor root axes
    can turn during a roll, so they are not a stable reference for stick intent.
    """
    camera = a.get("camera")
    if not camera or camera.get("age_ms", 101) > 100 or not camera.get("anchored"):
        return None
    yaw = math.radians(camera["body_yaw"])
    fx, fz = math.sin(yaw), math.cos(yaw)
    ca, cb = a["character"], b["character"]
    delta = [(cb["root"][i]-ca["root"][i])/ca["units_per_metre"] for i in range(3)]
    return {"right_metres": -delta[0]*fz+delta[2]*fx,
            "forward_metres": delta[0]*fx+delta[2]*fz, "up_metres": delta[1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run")
    run.add_argument("script", type=Path)
    sub.add_parser("stop")
    sub.add_parser("status")
    report = sub.add_parser("report")
    report.add_argument("trace", type=Path)
    report.add_argument("--id", required=True)
    args = parser.parse_args()
    if args.command == "report":
        with args.trace.open(encoding="utf-8") as trace:
            print(json.dumps(report_trace(trace, args.id), indent=2))
        return
    directory = mailbox(args.directory)
    if args.command == "status":
        print((directory / "status.json").read_text())
        return
    request_id = uuid.uuid4().hex
    text = "STOP " + request_id + "\n"
    if args.command == "run":
        script = json.loads(args.script.read_text())
        text = compile_script(script, request_id)
        # Save the exact source and identity for reproducible result correlation.
        (directory / (request_id + ".json")).write_text(json.dumps(script, indent=2) + "\n")
    write_request(directory, text)
    print(json.dumps({"id": request_id, "directory": str(directory), "queued": args.command}))


if __name__ == "__main__":
    main()
