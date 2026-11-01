"""Per sent swing (sm2vr_trace.jsonl): did the game attack? Native response is the animated
right hand (HANDR, root frame) peaking above 12 m/s within 0.9 s of the pulse (a real native
attack peaks at 20-50 m/s). Also the state at the swing: right trigger, rev active, camera mode, takeover,
sprint, loadout, epoch and time since the previous swing. Prints a table and a summary of
response rate per state."""
import json, math, sys, bisect, collections
path = sys.argv[1]
rows, swings, sprint = [], [], []
with open(path, encoding="utf-8", errors="replace") as f:
    for line in f:
        try: r = json.loads(line)
        except Exception: continue
        if r["type"] == "event":
            e = r["event"]
            if e == "swing": swings.append(r)
            elif e in ("sprint_camera_start", "sprint_camera_end"): sprint.append((r["t"], e.endswith("start")))
            continue
        if r["type"] != "sample": continue
        c = r.get("character") or {}
        i = r.get("input") or {}
        hand = next((b["native"] for b in c.get("body") or [] if b["name"] == "HANDR" and b.get("native")), None)
        rel = None
        if hand and c.get("root") and c.get("root_z_axis"):
            z = c["root_z_axis"]; x = (z[2], 0, -z[0]); u = c.get("units_per_metre") or 1
            d = [hand[k] - c["root"][k] for k in range(3)]
            rel = (sum(d[k] * x[k] for k in range(3)) / u, d[1] / u, sum(d[k] * z[k] for k in range(3)) / u)
        rev = r.get("chainsword_rev") or {}
        rows.append(dict(t=r["t"], rel=rel, rt=(i.get("raw_controls") or {}).get("right_trigger"), rev=rev.get("active"),
                         rate=rev.get("play_rate"), cam=(r.get("camera") or {}).get("mode"), take=c.get("animation_takeover"),
                         load=i.get("loadout"), root=c.get("root"), u=c.get("units_per_metre") or 1, epoch=i.get("epoch"),
                         touch=(i.get("controls") or {}).get("fire_touch")))
T = [r["t"] for r in rows]
def at(t):
    k = max(0, bisect.bisect_right(T, t) - 1); return rows[k]
def peak(t0, t1):
    k = bisect.bisect_left(T, t0); best = 0
    for a, b in zip(rows[k:], rows[k + 1:]):
        if b["t"] > t1: break
        dt = b["t"] - a["t"]
        if a["rel"] and b["rel"] and 0 < dt <= 0.1: best = max(best, math.dist(a["rel"], b["rel"]) / dt)
    return best
def sprinting(t):
    s = False
    for ts, on in sprint:
        if ts > t: break
        s = on
    return s
summary = collections.defaultdict(lambda: [0, 0])
prev = None
for s in swings:
    t = s["t"]; st = at(t)
    after = peak(t + 0.05, t + 0.9)
    resp = after > 12
    key = ("power" if s.get("power") else "light", "rev" if st["rev"] else "norev")
    summary[key][0] += 1; summary[key][1] += resp
    summary[("rt>0.15" if (st["rt"] or 0) > 0.15 else "rt<=0.15",)][0] += 1
    summary[("rt>0.15" if (st["rt"] or 0) > 0.15 else "rt<=0.15",)][1] += resp
    gap = t - prev if prev else 0; prev = t
    print(f"{t:8.2f} {'P' if s.get('power') else 'L'} resp={'Y' if resp else '-'} peak={after:5.1f} rt={st['rt'] or 0:4.2f} rev={int(bool(st['rev']))} rate={st['rate']} cam={st['cam']} take={int(bool(st['take']))} sprint={int(sprinting(t))} load={st['load']} ep={st['epoch']} gap={gap:5.2f}")
print("summary (sent, native response):")
for k, v in sorted(summary.items()): print(" ", k, v)
