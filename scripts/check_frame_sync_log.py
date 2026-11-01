"""Check a passive FrameSyncTrace capture; this does not authorize headset submission.

Run with the repo venv: python check_frame_sync_log.py <log> [--out <json>]
Reports standard-marker ordering and candidate bindings independently of extra Present calls.
Partial frames at either end of the bounded trace are reported, not treated as complete evidence.
"""
import argparse
import collections
import json
from pathlib import Path
import re

FIELDS = {"seq", "t", "thread", "kind", "marker", "frame", "simulation", "render",
          "presenter", "render_id", "present_id", "eye", "result"}


def analyze(text, policy="standard"):
    # Repeated/interactive bounded captures can have intentional gaps. Judge the last window.
    text = text.rsplit("FrameSyncCapture:",1)[-1]
    events, malformed = [], 0
    for line in text.splitlines():
        if "FrameSyncTrace:" not in line:
            continue
        fields = dict(re.findall(r"(\w+)=(-?\d+)\b", line.split("FrameSyncTrace:", 1)[1]))
        if set(fields) != FIELDS:
            malformed += 1
            continue
        events.append({key: int(value) for key, value in fields.items()})
    sequence_errors = sum(b["seq"] != a["seq"] + 1 or b["t"] < a["t"]
                          for a, b in zip(events, events[1:]))
    frames = collections.defaultdict(list)
    constants = collections.Counter()
    bound_cameras = collections.Counter()
    cameras, presentations, out_of_band, duplicates, rejected = 0, 0, 0, 0, 0
    epoch = 0
    for event in events:
        if event["kind"] == 4:
            epoch += 1
        elif event["kind"] == 1:
            cameras += 1
            if event["frame"] >= 0:
                bound_cameras[epoch,event["frame"]] += 1
        elif event["kind"] == 2:
            constants[epoch, event["frame"]] += 1
        elif event["kind"] == 3:
            presentations += 1
        elif event["kind"] == 0:
            rejected += event["result"] != 0
            if event["marker"] > 5:
                out_of_band += 1
            else:
                mapped = (event["frame"]+1) & 0xffffffff if policy == "sm2" and event["marker"] == 0 else event["frame"]
                frames[epoch, mapped].append(event)
    complete, partial, phase_errors, binding_errors = [], 0, 0, 0
    for key, markers in frames.items():
        unique = {}
        for event in markers:
            if event["marker"] in unique:
                duplicates += 1
            else:
                unique[event["marker"]] = event
        if set(unique) != set(range(6)):
            partial += 1
            continue
        order = [event["marker"] for event in unique.values()]
        # SM2's simulation-end may arrive on another thread after render/present; only start and
        # render/present ordering constrain candidate image identity.
        ordered = order == list(range(6)) if policy == "standard" else [phase for phase in order if phase != 1] == [0,2,3,4,5] and order.index(0) < order.index(1)
        phase_errors += not ordered
        render, present = unique[2], unique[4]
        bindings = (render["render"] == key[1] and present["presenter"] == key[1]
                    and render["render_id"] > 0 and present["present_id"] == render["render_id"]
                    and render["eye"] in (0, 1))
        binding_errors += not bindings
        complete.append({"epoch": key[0], "frame": key[1], "ordered": ordered,
                         "bindings_match": bindings, "render_id": render["render_id"],
                         "eye": render["eye"], "constants": constants[key], "bound_cameras": bound_cameras[key]})
    paired = sum(frame["constants"] > 0 for frame in complete)
    bound = sum(frame["bound_cameras"] > 0 for frame in complete)
    valid = (len(complete) >= 2 and paired >= 2 and cameras >= 2 and not
             (malformed or sequence_errors or phase_errors or binding_errors or rejected))
    return {"valid": valid, "events": len(events), "complete_frames": len(complete), "partial_frames": partial,
            "frames_with_constants": paired, "camera_events": cameras, "present_callbacks": presentations,
            "frames_with_bound_camera": bound, "bound_camera_events": sum(bound_cameras.values()),
            "out_of_band_markers": out_of_band, "duplicate_markers": duplicates, "rejected_markers": rejected,
            "malformed": malformed, "sequence_errors": sequence_errors, "phase_errors": phase_errors,
            "binding_errors": binding_errors, "frames": complete, "policy": policy,
            "limitation": "Marker/CPU camera evidence only; GPU image identity and runtime submission remain unverified."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--policy", choices=("sm2", "standard"), default="sm2")
    args = parser.parse_args()
    result = analyze(args.log.read_text(encoding="utf-8", errors="replace"), args.policy)
    encoded = json.dumps(result, indent=2)
    if args.out:
        args.out.write_text(encoded + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "frames"}, indent=2))
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
