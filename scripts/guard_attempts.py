"""Guard attempts: episodes where the left hand is inside the guard position zone (trace
guard.in_zone) with the sword loadout, not two-handed. Per episode: duration, closest
forward distance, max controller angle, whether a guard started (held), and the reason it
did not (angle short, entered too far out, or aim latched by the left trigger)."""
import json, sys, collections
path = sys.argv[1]
# Usage: guard_attempts.py TRACE [MIN_ANGLE]; 62 was the rule in the 21 Oct evening session (58 since).
MIN_ANGLE, MAX_ENTRY = float(sys.argv[2]) if len(sys.argv) > 2 else 62, 0.30
eps, cur = [], None
lb = []
with open(path, encoding="utf-8", errors="replace") as fh:
    for line in fh:
        try:
            r = json.loads(line)
        except ValueError:
            continue  # a trace line cut short (seen at 8192 characters, 29 Oct)
        if r["type"] == "event":
            if r["event"] == "button" and r.get("name", "").startswith("LB"): lb.append((r["t"], r.get("down")))
            continue
        if r["type"] != "sample": continue
        i = r.get("input") or {}; g = i.get("guard") or {}
        if not g.get("enabled") or not g.get("hand"):
            if cur: eps.append(cur); cur = None
            continue
        inz = g.get("in_zone")
        if inz:
            lt = (i.get("raw_controls") or {}).get("left_trigger") or 0
            s = dict(t=r["t"], lat=g["hand"][0], h=g["hand"][1], f=g["hand"][2], ang=g.get("angle_degrees") or 0, held=g.get("held"), lt=lt, two=i.get("two_handed"), load=i.get("loadout"))
            if cur is None: cur = dict(samples=[s])
            else: cur["samples"].append(s)
        elif cur:
            eps.append(cur); cur = None
if cur: eps.append(cur)
stats = collections.Counter()
near = []
for e in eps:
    s = e["samples"]; first = s[0]
    dur = s[-1]["t"] - first["t"]
    held = any(x["held"] for x in s)
    maxang = max(x["ang"] for x in s)
    close_samples = [x for x in s if x["f"] <= MAX_ENTRY]
    lt = any(x["lt"] > 0.05 for x in s)
    two = any(x["two"] for x in s)
    if held: why = "guard"
    elif two: why = "two-handed"
    elif lt: why = "left trigger (aim latch)"
    elif not close_samples: why = "entered too far out"
    elif max(x["ang"] for x in close_samples) < MIN_ANGLE: why = "angle short"
    else: why = "other (latched/brief)"
    stats[why] += 1
    if why == "angle short":
        near.append((first["t"], dur, max(x["ang"] for x in close_samples), min(x["f"] for x in s), first))
    e.update(why=why, dur=dur, maxang=maxang)
print("episodes", len(eps), dict(stats))
print("LB presses", sum(1 for t, d in lb if d))
bins = collections.Counter(int(a // 5) * 5 for _, _, a, _, _ in near)
print("angle short, max angle while close (5-degree bins):", dict(sorted(bins.items())))
print("near misses (max angle >= 50 while close):")
for t, dur, a, f, s in near:
    if a >= 50: print(f"  {t:8.2f} dur {dur:4.2f}s max angle {a:4.1f} closest forward {f:+.2f} entry h{s['h']:+.2f} l{s['lat']:+.2f} f{s['f']:+.2f}")
print("guards:")
for e in eps:
    if e["why"] == "guard":
        s = e["samples"][0]
        print(f"  {s['t']:8.2f} dur {e['dur']:4.2f}s max angle {e['maxang']:4.1f} entry h{s['h']:+.2f} l{s['lat']:+.2f} f{s['f']:+.2f}")
print("aim-latched episodes (left trigger pulled in the zone), max angle while close and trigger released:")
for e in eps:
    if e["why"] != "left trigger (aim latch)": continue
    s = e["samples"]
    rel = [x for x in s if x["lt"] <= 0.05 and x["f"] <= MAX_ENTRY]
    ma = max((x["ang"] for x in rel), default=0)
    lt_time = sum(1 for x in s if x["lt"] > 0.05) / 30
    print(f"  {s[0]['t']:8.2f} dur {e['dur']:4.2f}s trigger-pulled {lt_time:4.2f}s max angle released+close {ma:5.1f} {'<- would guard' if ma >= MIN_ANGLE else ''}")
