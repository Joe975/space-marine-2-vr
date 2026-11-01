"""Pistol/bolter shots (native RT edges) and whether the character attacked after them without
a melee button: the 19 Oct 'left pistol firing can also attack' check for GunStrike on RT."""
import json, math, sys, bisect
path = sys.argv[1]
rows, edges, melee = [], [], []
prev_rt = 0
with open(path, encoding="utf-8", errors="replace") as fh:
    for line in fh:
        try: r = json.loads(line)
        except Exception: continue
        if r["type"] == "event":
            if r["event"] == "button" and r.get("down") and r.get("name") in ("RB_melee", "up_power_melee", "LB_parry_block"): melee.append(r["t"])
            if r["event"] == "swing": melee.append(r["t"])
            continue
        if r["type"] != "sample": continue
        i = r.get("input") or {}; c = r.get("character") or {}
        rt = (i.get("pad") or {}).get("rt") or 0
        if rt > 128 and prev_rt <= 128: edges.append(r["t"])
        prev_rt = rt
        hand = next((b["native"] for b in c.get("body") or [] if b["name"] == "HANDR" and b.get("native")), None)
        if hand is None and c.get("right_native"): hand = c["right_native"]
        if hand and c.get("root") and c.get("root_z_axis"):
            z = c["root_z_axis"]; x = (z[2], 0, -z[0]); u = c.get("units_per_metre") or 1
            d = [hand[k] - c["root"][k] for k in range(3)]
            rows.append((r["t"], (sum(d[k] * x[k] for k in range(3)) / u, d[1] / u, sum(d[k] * z[k] for k in range(3)) / u)))
T = [x[0] for x in rows]
def peak(t0, t1):
    k = bisect.bisect_left(T, t0); best = 0
    for a, b in zip(rows[k:], rows[k + 1:]):
        if b[0] > t1: break
        dt = b[0] - a[0]
        if 0 < dt <= 0.1: best = max(best, math.dist(a[1], b[1]) / dt)
    return best
clean = [t for t in edges if all(abs(t - m) > 1.0 for m in melee)]
peaks = [(t, peak(t + 0.05, t + 0.9)) for t in clean]
attacks = [(t, p) for t, p in peaks if p > 12]
print(f"{path[-60:]}: RT edges {len(edges)}, without melee within 1 s {len(clean)}, followed by a right-hand attack-speed motion {len(attacks)}")
for t, p in attacks: print(f"   {t:8.2f} peak {p:.1f} m/s")
