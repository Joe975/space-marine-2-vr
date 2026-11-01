"""Measure sampled camera drift in a QPC-aligned gameplay capture; not actor determinism."""
import argparse
from datetime import datetime, timedelta
import json
import math
from pathlib import Path
import re

STAMP = r"^\[(\d\d):(\d\d):(\d\d\.\d+)\]"
CAMERA = re.compile(STAMP+r".*NativeCameraSource: caller=0x18033bf .*center=\[([^]]+)\] age_ms=([\d.]+)")
VIEW = re.compile(STAMP+r".*EngineCameraProbe: caller=0x18033bf calls=.*row0=\[([^]]+)\] row1=\[([^]]+)\] row2=\[([^]]+)\]")


def analyze(log, capture, clock, offset_hours=13):
    origin = datetime.fromisoformat(clock["utc"].replace("Z", "+00:00"))+timedelta(hours=offset_hours)
    origin_ms = ((origin.hour*60+origin.minute)*60+origin.second)*1000+origin.microsecond/1000
    cameras, views = [], []
    def qpc(match):
        tod = ((int(match[1])*60+int(match[2]))*60+float(match[3]))*1000
        delta = (tod-origin_ms+43200000)%86400000-43200000
        return clock["qpc_ms"]+delta
    for line in log:
        for pattern, samples, count in ((CAMERA, cameras, 3), (VIEW, views, 9)):
            match = pattern.search(line)
            if not match:
                continue
            time = qpc(match)
            if not capture["qpc_start_ms"] <= time <= capture["qpc_end_ms"]:
                continue
            values = ([float(v) for v in match[4].split(",")] if count == 3 else
                      [float(v) for field in match.group(4,5,6) for v in field.split(",")])
            if len(values) != count or not all(math.isfinite(v) for v in values):
                raise ValueError("invalid camera sample")
            if count == 3 and float(match[5]) > 100:
                continue
            samples.append((time, values))
    if len(cameras) < 3 or len(views) < 3:
        raise ValueError("need at least three fresh position and orientation observations")
    gaps = [capture["qpc_start_ms"], *(t for t,_ in cameras), capture["qpc_end_ms"]]
    max_gap = max(b-a for a,b in zip(gaps,gaps[1:]))
    view_gaps = [capture["qpc_start_ms"], *(t for t,_ in views), capture["qpc_end_ms"]]
    max_view_gap = max(b-a for a,b in zip(view_gaps,view_gaps[1:]))
    displacement = max(math.dist(cameras[0][1], p) for _,p in cameras)
    def angle(a,b):
        denominator = math.sqrt(sum(x*x for x in a)*sum(x*x for x in b))
        if denominator < .5:
            raise ValueError("invalid orientation basis")
        return math.degrees(math.acos(max(-1,min(1,sum(x*y for x,y in zip(a,b))/denominator))))
    rotation = max(angle(views[0][1][i:i+3],v[i:i+3]) for _,v in views for i in (0,3,6))
    return {"position_samples": len(cameras), "orientation_samples": len(views),
            "first_native_center": cameras[0][1], "first_view_basis": views[0][1],
            "max_displacement_game_units": displacement, "max_basis_rotation_degrees": rotation,
            "max_position_sample_gap_ms": max_gap,
            "max_orientation_sample_gap_ms": max_view_gap,
            "sampled_camera_stable": displacement <= .05 and rotation <= .5 and max_gap <= 8000 and max_view_gap <= 8000,
            "limits": "Five-second camera samples do not prove identical actors, health, simulation or intervening head motion."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("log", "capture", "clock", "out"):
        parser.add_argument("--"+key, type=Path, required=True)
    parser.add_argument("--offset-hours", type=float, default=13)
    args = parser.parse_args()
    with args.log.open(encoding="utf-8-sig") as source:
        report = analyze(source, json.loads(args.capture.read_text(encoding="utf-8-sig")),
                         json.loads(args.clock.read_text(encoding="utf-8-sig")), args.offset_hours)
    args.out.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(report))
