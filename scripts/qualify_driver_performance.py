"""Qualify a fixed 50cm-hand driver hold over the exact PresentMon QPC window."""
import datetime
import json
import math


def qualify(summary, clock, statuses, records, request_id):
    start, end = summary["qpc_start_ms"], summary["qpc_end_ms"]
    if end <= start:
        raise ValueError("invalid capture window")
    surrounding = [s for s in statuses if start-1000 <= s["qpc_ms"] <= end+1000]
    if (not surrounding or surrounding[0]["qpc_ms"] > start or surrounding[-1]["qpc_ms"] < end-300
            or max(b["qpc_ms"]-a["qpc_ms"] for a, b in zip(surrounding, surrounding[1:])) > 1000):
        raise ValueError("driver status coverage gap")
    if any(s["id"] != request_id or s["state"] != "running" or s["error"] for s in surrounding):
        raise ValueError("driver inactive or cancelled during capture")
    terminal = [s for s in statuses if s["id"] == request_id and s["state"] == "completed"]
    if not terminal or terminal[-1]["qpc_ms"] < end:
        raise ValueError("driver did not complete after capture")
    header = next(r for r in records if r.get("type") == "header")
    # Header records local wall time; the runner captures UTC and QPC together.
    origin = datetime.datetime.fromisoformat(header["wall_clock_local"]).astimezone()
    now = datetime.datetime.fromisoformat(clock["utc"])
    offset = (now-origin).total_seconds()-clock["qpc_ms"]/1000
    first, last = start/1000+offset, end/1000+offset
    marks = [r for r in records if r.get("event") == "test_driver" and r.get("detail", "").startswith(request_id+" ")]
    running = next((r for r in marks if r["detail"].split()[1] == "running"), None)
    finished = next((r for r in marks if r["detail"].split()[1] not in ("preroll", "running")), None)
    if (not running or running["t"] > first or not finished or finished["t"] < last
            or finished["detail"].split()[1] != "completed"):
        raise ValueError("native driver markers do not cover capture")
    samples = [r for r in records if r.get("type") == "sample" and first <= r["t"] <= last]
    if (len(samples) < (end-start)/1000*20 or samples[0]["t"] > first+.1 or samples[-1]["t"] < last-.1
            or max(b["t"]-a["t"] for a, b in zip(samples, samples[1:])) > .15):
        raise ValueError("game trace coverage gap")
    vectors = {key: [] for key in ("head", "head_forward", "camera", "camera_forward", "root", "root_forward")}
    hand_error = 0
    actor = samples[0]["character"]["actor"]
    for r in samples:
        inp, cam, char, game = (r[k] for k in ("input", "camera", "character", "game"))
        if (any(not math.isfinite(age) or not 0 <= age <= 100 for age in (inp["age_ms"], cam["age_ms"], char["age_ms"]))
                or not cam["anchored"] or char["actor"] != actor or not actor
                or not game["available"] or game["cinematic"] or game["paused"]):
            raise ValueError("stale or non-gameplay observation")
        if any(inp["pad"].get(k, 0) != 0 for k in ("buttons", "lt", "rt", "lx", "ly")):
            raise ValueError("non-neutral driver controls")
        head = inp["head"]
        if not char["driven"]:
            raise ValueError("hands are not driven")
        for values in (head, inp["head_forward"], inp["left"], inp["right"], cam["eye_centre"], cam["eye_forward"], char["root"], char["root_z_axis"]):
            if len(values) != 3 or not all(math.isfinite(x) for x in values):
                raise ValueError("invalid observed vector")
        fx, _, fz = inp["head_forward"]
        length = math.hypot(fx, fz)
        if length < .1:
            raise ValueError("invalid head orientation")
        fx, fz = fx/length, fz/length
        for side, x in (("left", -.18), ("right", .18)):
            dx, dy, dz = [inp[side][i]-head[i] for i in range(3)]
            actual = (-dx*fz+dz*fx, dy, dx*fx+dz*fz)
            hand_error = max(hand_error, *(abs(a-b) for a, b in zip(actual, (x, -.1, .5))))
        vectors["head"].append(head)
        vectors["head_forward"].append(inp["head_forward"])
        vectors["camera"].append(cam["eye_centre"])
        vectors["camera_forward"].append(cam["eye_forward"])
        vectors["root_forward"].append(char["root_z_axis"])
        if not math.isfinite(char["units_per_metre"]) or char["units_per_metre"] <= 0:
            raise ValueError("invalid game scale")
        vectors["root"].append([x/char["units_per_metre"] for x in char["root"]])
    extents = {key: math.sqrt(sum((max(v[i] for v in values)-min(v[i] for v in values))**2
                                for i in range(3))) for key, values in vectors.items()}
    if hand_error > .025 or any(x > .025 for x in extents.values()):
        raise ValueError("actual player pose differs from stationary 50cm hold")
    return {"samples": len(samples), "trace_start_s": first, "trace_end_s": last,
            "hand_max_error_metres": hand_error,
            "position_extent_metres": {key: extents[key] for key in ("head", "camera", "root")},
            "direction_extent": {key: extents[key] for key in ("head_forward", "camera_forward", "root_forward")},
            "camera_initial": vectors["camera"][0], "root_initial_metres": vectors["root"][0],
            "cached_native_optics": any(s.get("cached_native_optics", False) for s in surrounding)}


def qualify_allocation(rows, summary, mode):
    expected = {"committed": 0, "pooled": 1}[mode]
    values = [int(float(r["value"])) for r in rows if r["event"] == "inline_copy_allocate_cpu"
              and summary["qpc_start_ms"] <= float(r["qpc_start_ms"]) <= summary["qpc_end_ms"]]
    if len(values) < summary["submitted_pair_count"]*1.8 or set(values) != {expected}:
        raise ValueError("allocation policy missing, mixed or different from requested mode")
    return {"mode": mode, "strategy_code": expected, "observations": len(values)}


def load_records(path):
    records = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            if index != len(lines)-1:
                raise ValueError("malformed game trace record")
            # An active recorder may have one partial trailing line.
    return records
