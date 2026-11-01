"""What differs between the two views' light lists (docs/DECIDE_ONCE_STEREO.md).

Usage: analyze_light_dump.py <abs folder with light_dump_*.bin> [--offsets N]

Each dump (NativeOrdinaryLightVisibility::dump_lists): two int32 counts, both eyes' positions (six
floats), then view 0's and view 1's 0x210-byte light entries. Lights are matched across views by
position, type and range (StereoLightList.h's key). Reports:
- how many of view 1's lights match view 0 by key, by full bytes, and not at all;
- for lights matched by key, which byte offsets differ between the two copies (the per-view fields);
- for view-1-only lights, the nearest view-0 light of the same type and range, and how far it is:
  a near-zero distance means the "same" light carries a view-dependent position (would be doubled).
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

ENTRY, POSITION, TYPE, RANGE = 0x210, 0x30, 0x6C, 0x104


def load(path: Path):
    data = path.read_bytes()
    n0, n1 = struct.unpack_from("<ii", data, 0)
    eyes = struct.unpack_from("<6f", data, 8)
    base = 8 + 24
    views = []
    for n in (n0, n1):
        views.append([data[base + i * ENTRY: base + (i + 1) * ENTRY] for i in range(n)])
        base += n * ENTRY
    return views, eyes


def key(entry: bytes) -> bytes:
    return entry[POSITION:POSITION + 12] + entry[TYPE:TYPE + 4] + entry[RANGE:RANGE + 4]


def position(entry: bytes):
    return struct.unpack_from("<3f", entry, POSITION)


def analyse(views):
    v0, v1 = views
    by_key = {key(e): e for e in v0}
    full = {bytes(e) for e in v0}
    stats = Counter()
    offsets = Counter()
    nearest = []
    for e in v1:
        k = key(e)
        if bytes(e) in full:
            stats["full_match"] += 1
        elif k in by_key:
            stats["key_match_bytes_differ"] += 1
            other = by_key[k]
            for i in range(0, ENTRY, 4):
                if e[i:i + 4] != other[i:i + 4]:
                    offsets[i] += 1
        else:
            stats["view1_only"] += 1
            p = position(e)
            same = [o for o in v0 if o[TYPE:TYPE + 4] == e[TYPE:TYPE + 4] and o[RANGE:RANGE + 4] == e[RANGE:RANGE + 4]]
            if same:
                d = min(sum((a - b) ** 2 for a, b in zip(p, position(o))) ** 0.5 for o in same)
                nearest.append(d)
    return stats, offsets, nearest


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("folder", type=Path)
    p.add_argument("--offsets", type=int, default=12)
    a = p.parse_args(argv)
    if not a.folder.is_absolute():
        p.error("folder must be absolute")
    files = sorted(a.folder.glob("light_dump_*.bin"))
    if not files:
        p.error("no light_dump_*.bin files")
    total = Counter()
    offsets = Counter()
    nearest = []
    for f in files:
        views, _ = load(f)
        s, o, n = analyse(views)
        total.update(s)
        offsets.update(o)
        nearest.extend(n)
        print(f"{f.name}: view0={len(views[0])} view1={len(views[1])} " + " ".join(f"{k}={v}" for k, v in sorted(s.items())))
    print("total: " + " ".join(f"{k}={v}" for k, v in sorted(total.items())))
    print("differing offsets (offset: count): " + ", ".join(f"{k:#x}: {v}" for k, v in offsets.most_common(a.offsets)))
    if nearest:
        nearest.sort()
        q = lambda f: nearest[min(len(nearest) - 1, int(f * len(nearest)))]
        print(f"view1-only nearest same-type view0 light: n={len(nearest)} min={nearest[0]:.3f} p10={q(.1):.3f} median={q(.5):.3f} "
              f"under 0.01: {sum(1 for d in nearest if d < 0.01)}")


if __name__ == "__main__":
    sys.exit(main())
