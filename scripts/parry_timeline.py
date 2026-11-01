"""Each incoming-attack indication in a trace, with the player's parry presses and the outcome.

Usage: parry_timeline.py TRACE [--json]

Needs a trace from a build with EngineParryProbe (-Trace): "parry_sound" notes (indicator, parry and
player-hit sounds the game posts), "attack_event" notes (the attacker's animation event, with its
root position) and the usual LB button events. Per indicator sound:
- the attacker's bearing from the character's facing (degrees; 0 = straight ahead, the game parries
  within +-90) and distance: from the sound objects' Wwise transforms (the indicator is posted on
  the attacker's object, the parry and hit sounds on the player's), or else from an attack_event;
- LB presses from 1.5 s before to 2.5 s after, as offsets from the indicator;
- the outcome within 3 s: parried / parried_riposte / player_hit / guard_break, or none heard.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import struct
from pathlib import Path

OUTCOMES = ("parried", "parried_riposte", "player_hit", "guard_break", "parry_other")


def load(path: Path):
    sounds, attacks, presses, samples = [], [], [], []
    for line in path.open(encoding="utf-8", errors="replace"):
        try:
            d = json.loads(line)
        except ValueError:
            continue  # a cut line
        t = d.get("t")
        if d.get("type") == "event":
            e = d.get("event")
            if e == "parry_sound":
                detail = d.get("detail", "")
                label, name = (detail.split(" ") + ["", ""])[:2]
                obj = re.search(r"object=(\S+)", detail)
                tr = re.search(r" transform=([0-9a-f]{96})", detail)
                at = re.search(r"attacker_transform=([0-9a-f]{96})", detail)
                sounds.append((t, label, name, obj.group(1) if obj else None, decode(tr.group(1)) if tr else None,
                               decode(at.group(1)) if at else None))
            elif e == "attack_event":
                m = re.match(r"(\S+) actor=(\S*) inst=(\S+) pos=\[([-\d.e]+),([-\d.e]+),([-\d.e]+)\]", d.get("detail", ""))
                if m:
                    attacks.append((t, m.group(1), m.group(2), m.group(3), tuple(float(m.group(i)) for i in (4, 5, 6))))
            elif e == "button" and d.get("name", "").startswith("LB") and d.get("down"):
                presses.append(t)
        elif d.get("type") == "sample":
            c = d.get("character") or {}
            if c.get("root") and c.get("root_z_axis"):
                samples.append((t, c["root"], c["root_z_axis"]))
    return sounds, attacks, presses, samples


PLAYER = ("parry_animation", "player_hit", "parried", "parried_riposte", "guard_break", "parry_other")


def decode(hexdigits):
    """A Wwise AkWorldTransform (48 bytes): position (3 doubles) and front and top (3 floats each), in
    either field order; the one whose front and top are unit vectors. (position, front, top) or None."""
    raw = bytes.fromhex(hexdigits)
    for pos_at, vec_at in ((24, 0), (0, 24)):
        pos = struct.unpack_from("<3d", raw, pos_at)
        front, top = struct.unpack_from("<3f", raw, vec_at), struct.unpack_from("<3f", raw, vec_at + 12)
        unit = lambda v: all(math.isfinite(x) for x in v) and abs(math.sqrt(sum(x * x for x in v)) - 1) < 0.05
        if unit(front) and unit(top) and all(math.isfinite(x) and abs(x) < 1e7 for x in pos):
            return pos, front, top
    return None


def wwise_bearing(player, attacker):
    """Angle from the player's front to the attacker in the plane normal to the player's top, and the
    distance in that plane. (degrees, distance) or None."""
    (pp, front, up), ap = player, attacker[0]
    v = [a - b for a, b in zip(ap, pp)]
    flat = lambda x: [xi - sum(a * b for a, b in zip(x, up)) * ui for xi, ui in zip(x, up)]
    vh, fh = flat(v), flat(front)
    if math.hypot(*vh) < 1e-6 or math.hypot(*fh) < 1e-6:
        return None
    cross = [fh[1] * vh[2] - fh[2] * vh[1], fh[2] * vh[0] - fh[0] * vh[2], fh[0] * vh[1] - fh[1] * vh[0]]
    angle = math.degrees(math.atan2(sum(c * u for c, u in zip(cross, up)), sum(a * b for a, b in zip(fh, vh))))
    return angle, math.hypot(*vh)


def bearing(sample, position):
    _, root, forward = sample
    dx, dz = position[0] - root[0], position[2] - root[2]
    if math.hypot(dx, dz) < 1e-6 or math.hypot(forward[0], forward[2]) < 1e-6:
        return None
    a = math.degrees(math.atan2(dx, dz) - math.atan2(forward[0], forward[2]))
    return (a + 180) % 360 - 180


def timeline(sounds, attacks, presses, samples):
    rows = []
    for t, label, name, obj, transform, _ in sounds:
        if not label.startswith("indicator"):
            continue
        attack = next((a for a in reversed(attacks) if t - 0.4 <= a[0] <= t + 0.05), None)
        sample = min(samples, key=lambda s: abs(s[0] - t)) if samples else None
        mine = [x for x in sounds if x[1] in PLAYER and x[4] and abs(x[0] - t) <= 5]
        player = min(mine, key=lambda x: abs(x[0] - t))[4] if mine else None
        # The sound objects share the trace's world frame (29 Oct: the player's object sat 0.7 units above
        # the trace root, its front along root_z_axis), so the body's facing comes from the trace;
        # the player's own object orientation (it rides a bone) is the fallback.
        measured = None
        if transform and sample and bearing(sample, transform[0]) is not None:
            measured = (bearing(sample, transform[0]), math.dist((transform[0][0], transform[0][2]), (sample[1][0], sample[1][2])))
        elif player and transform:
            measured = wwise_bearing(player, transform)
        if measured is None and attack and sample and bearing(sample, attack[4]) is not None:
            measured = (bearing(sample, attack[4]), math.dist((attack[4][0], attack[4][2]), (sample[1][0], sample[1][2])))
        row = {"t": t, "indicator": label, "attacker": attack[2] if attack else obj,
               "bearing": round(measured[0], 1) if measured else None,
               "distance": round(measured[1], 1) if measured else None,
               "presses": [round(p - t, 2) for p in presses if t - 1.5 <= p <= t + 2.5],
               "outcome": next(((round(x[0] - t, 2), x[1], x[2]) for x in sounds if 0 <= x[0] - t <= 3 and x[1] in OUTCOMES), None)}
        # Where the attacker was at the outcome, against the body's facing then.
        outcome = next((x for x in sounds if 0 <= x[0] - t <= 3 and x[1] in OUTCOMES and x[5]), None)
        hit_sample = min(samples, key=lambda q: abs(q[0] - outcome[0])) if outcome and samples else None
        at_hit = bearing(hit_sample, outcome[5][0]) if hit_sample else None
        row["bearing_at_outcome"] = round(at_hit, 1) if at_hit is not None else None
        rows.append(row)
    return rows


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("trace", type=Path)
    p.add_argument("--json", action="store_true")
    a = p.parse_args(argv)
    rows = timeline(*load(a.trace))
    if a.json:
        print(json.dumps(rows, indent=1))
        return
    for r in rows:
        out = f"{r['outcome'][1]} at +{r['outcome'][0]:.2f} s ({r['outcome'][2]})" if r["outcome"] else "no outcome sound"
        print(f"{r['t']:8.2f} {r['indicator']:16s} {r['attacker'] or '?':24s} bearing {r['bearing'] if r['bearing'] is not None else '?':>6} deg "
              f"dist {r['distance'] if r['distance'] is not None else '?':>5}  at outcome {r['bearing_at_outcome'] if r['bearing_at_outcome'] is not None else '?':>6}  "
              f"LB at {r['presses'] or 'none'}  -> {out}")
    print(f"{len(rows)} indications")


if __name__ == "__main__":
    main()
