"""Where the real floor lands on the Marine, and how the arms compare (docs/BODY_SCALE.md).

Usage: measure_floor_scale.py TRACE_ABSOLUTE [TRACE_ABSOLUTE ...]

Reads standing first-person gameplay from a recorded VR trace (armed, no menu, no animation
takeover, head near its usual height, level gaze) for the rendered eye's height over the Marine's
root and the eyes' scale, then every moment a controller was on the real floor for where that hand
landed. Prints JSON per trace. Distances are game units unless named metres.

floor_above_root: 0 is the real floor on the game's ground. first_person_units_per_metre_for_floor
is the eyes' scale that would put it there for the measured player. reach: the median distance
from shoulder to wrist target over the Marine's arm length, above 1 stretches the arm.
"""
import json
import math
import statistics
import sys
from pathlib import Path

FLOOR_METRES = 0.06  # A controller this close to the real floor is on it.
HEAD_METRES = 1.0  # Standing: the head above this.
MAX_GAZE_PITCH = math.sin(math.radians(30))


def median(values):
    return round(statistics.median(values), 3) if values else None


def bones(character):
    return {b["name"]: b for b in character.get("body") or []}


def samples(lines):
    for line in lines:
        if '"character":{' not in line or '"head_forward":[' not in line:
            continue
        try:
            record = json.loads(line)
        except ValueError:
            continue
        i, c, camera = record.get("input"), record.get("character"), record.get("camera")
        if not i or not c or not camera or not i.get("armed") or i.get("menu"):
            continue
        if c.get("animation_takeover") or camera.get("mode") != "body" or not camera.get("eye_centre"):
            continue
        yield i, c, camera


def measure(lines):
    eye, height, units, floor, upper, lower, breadth, reach = [], [], [], [], [], [], [], []
    span = 0.0
    for i, c, camera in samples(lines):
        root_y = c["root"][1]
        left, right = i.get("left"), i.get("right")
        if left and right:
            span = max(span, math.dist(left, right))
        for hand, target in ((left, c.get("left_target")), (right, c.get("right_target"))):
            if hand and target and hand[1] < FLOOR_METRES:
                floor.append(target[1] - root_y)
        forward = i.get("head_forward")
        if not i.get("head") or not forward or i["head"][1] < HEAD_METRES or abs(forward[1]) > MAX_GAZE_PITCH:
            continue
        eye.append(camera["eye_centre"][1] - root_y)
        height.append(i["head"][1])
        units.append(camera["units_per_metre"])
        body = bones(c)
        for side, target in (("L", c.get("left_target")), ("R", c.get("right_target"))):
            shoulder, elbow, wrist = (body.get(name + side, {}).get("native") for name in ("ARM1", "ARM2", "HAND"))
            if not shoulder or not elbow or not wrist:
                continue
            upper.append(math.dist(shoulder, elbow))
            lower.append(math.dist(elbow, wrist))
            final = body[f"ARM1{side}"].get("final")
            if target and final:
                reach.append(math.dist(final, target))
        if body.get("ARM1L", {}).get("native") and body.get("ARM1R", {}).get("native"):
            breadth.append(math.dist(body["ARM1L"]["native"], body["ARM1R"]["native"]))
    if not eye:
        return {"samples": 0}
    eye_above_root, player, scale = statistics.median(eye), statistics.median(height), statistics.median(units)
    result = {
        "samples": len(eye),
        "eye_above_root": median(eye),
        "player_eye_height_metres": median(height),
        "eyes_units_per_metre": median(units),
        "floor_above_root_predicted": round(eye_above_root - scale * player, 3),
        "floor_above_root": median(floor),
        "floor_samples": len(floor),
        "first_person_units_per_metre_for_floor": round(eye_above_root / player, 3),
        "player_span_metres": round(span, 3),
    }
    if upper:
        arm = statistics.median(upper) + statistics.median(lower)
        result.update({"marine_arm": round(arm, 3), "reach": round(statistics.median(reach) / arm, 3) if reach else None})
        if breadth:
            marine_span = statistics.median(breadth) + 2 * arm
            result.update({"marine_span": round(marine_span, 3), "player_span_game": round(span * scale, 3)})
    return result


def main(paths):
    result = {}
    for path in paths:
        with Path(path).open(encoding="utf-8", errors="replace") as lines:
            result[path] = measure(lines)
    print(json.dumps(result, indent=1))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
