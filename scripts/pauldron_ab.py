"""Pauldron A/B: shoulder geometry at each eye snapshot, and a side-by-side contact sheet.

Usage:
  pauldron_ab.py --a DUMP_A TRACE_A --b DUMP_B TRACE_B --out DIR [--names a_rifle_rest,b_...]

DUMP_* are the runs' ordinary dump directories (each snapshot NAME/left.ppm, pair.json),
TRACE_* their sm2vr_trace.jsonl. For every snapshot name present in both runs it takes the
driven trace sample nearest the snapshot event and reports, in the body's level frame and in
physical metres from the rendered eye centre: the left shoulder helper (shoulder_L, which
carries the pauldron) forward/right/up, its direction (azimuth left of the body's forward,
elevation), and the shoulder twist (shoulder_L->shoulder_R line versus square, positive = left
shoulder leading). Writes measurements.json and contact_sheet.jpg (left eyes, A | B per row).
See docs/PAULDRON.md.
"""
import argparse
import json
import math
from pathlib import Path


def _sub(a, b): return [a[i] - b[i] for i in range(3)]
def _dot(a, b): return sum(a[i] * b[i] for i in range(3))


def body_frame(character):
    """Level forward and right axes of the actor (right from the clavicles, so rig-handedness free)."""
    z = character["root_z_axis"]
    length = math.hypot(z[0], z[2])
    if length < 1e-6:
        return None
    forward = [z[0] / length, 0.0, z[2] / length]
    right = [-forward[2], 0.0, forward[0]]
    bones = {b["name"]: b for b in character.get("body") or []}
    left, rightc = (bones.get("clavicleL") or {}).get("native"), (bones.get("clavicleR") or {}).get("native")
    if not left or not rightc:
        return None
    if _dot(_sub(rightc, left), right) < 0:
        right = [-v for v in right]
    return forward, right


def measure(sample):
    """Shoulder measurements for one trace sample, or None when the sample lacks them."""
    character, camera = sample.get("character") or {}, sample.get("camera") or {}
    eye = camera.get("eye_centre")
    frame = body_frame(character) if character else None
    if not eye or not frame:
        return None
    forward, right = frame
    upm = character.get("units_per_metre") or 1.0
    bones = {b["name"]: b for b in character["body"]}
    out = {"t": sample["t"]}
    for which in ("native", "final"):
        left = (bones.get("shoulder_L") or {}).get(which) or (bones.get("shoulder_L") or {}).get("native")
        rightb = (bones.get("shoulder_R") or {}).get(which) or (bones.get("shoulder_R") or {}).get("native")
        if not left or not rightb:
            return None
        rel = _sub(left, eye)
        f, r, u = _dot(rel, forward) / upm, _dot(rel, right) / upm, rel[1] / upm
        line = _sub(rightb, left)
        out[which] = {
            "shoulder_L_forward_m": round(f, 3), "shoulder_L_right_m": round(r, 3), "shoulder_L_up_m": round(u, 3),
            "shoulder_L_azimuth_deg": round(math.degrees(math.atan2(-r, f)), 1),
            "shoulder_L_elevation_deg": round(math.degrees(math.atan2(u, math.hypot(f, r))), 1),
            "twist_deg": round(math.degrees(math.atan2(-_dot(line, forward), _dot(line, right))), 1),
        }
    traced = (character.get("twist_degrees") or {})
    out["traced_twist_deg"] = {"native": traced.get("native"), "final": traced.get("final")}
    out["lean_deg"] = character.get("lean_degrees")
    return out


def snapshot_samples(trace_path):
    """{snapshot name: nearest driven sample with a body and eye} from one trace."""
    snaps, samples = [], []
    with open(trace_path, encoding="utf-8") as f:
        for line in f:
            if '"eye_snapshot"' in line:
                d = json.loads(line)
                snaps.append((d["t"], d["detail"]))
            elif '"driven":true' in line and '"eye_centre"' in line and '"body":[' in line:
                samples.append(json.loads(line))
    out = {}
    for t, name in snaps:
        if samples:
            out[name] = min(samples, key=lambda s: abs(s["t"] - t))
    return out


def sheet(rows, out_path, width=560):
    from PIL import Image, ImageDraw
    label_h = 26
    tiles = []
    for name, a_path, b_path, note in rows:
        a, b = Image.open(a_path).convert("RGB"), Image.open(b_path).convert("RGB")
        h = round(a.height * width / a.width)
        tiles.append((name, a.resize((width, h)), b.resize((width, h)), note))
    total_h = sum(t[1].height + label_h for t in tiles) + label_h
    canvas = Image.new("RGB", (2 * width + 10, total_h), "black")
    draw = ImageDraw.Draw(canvas)
    draw.text((6, 6), "A: before (-NoUprightTwist, lean only)", fill="white")
    draw.text((width + 16, 6), "B: after (shoulders squared)", fill="white")
    y = label_h
    for name, a, b, note in tiles:
        draw.text((6, y + 6), f"{name}  {note}", fill="white")
        y += label_h
        canvas.paste(a, (0, y))
        canvas.paste(b, (width + 10, y))
        y += a.height
    canvas.save(out_path, quality=88)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", nargs=2, required=True, metavar=("DUMP", "TRACE"))
    ap.add_argument("--b", nargs=2, required=True, metavar=("DUMP", "TRACE"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--names", default="")
    args = ap.parse_args(argv)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    runs = {k: (Path(d), snapshot_samples(t)) for k, (d, t) in (("A", args.a), ("B", args.b))}
    names = [n for n in args.names.split(",") if n] or sorted(set(runs["A"][1]) & set(runs["B"][1]))
    report, rows = {}, []
    for name in names:
        entry = {}
        for key, (dump, samples) in runs.items():
            entry[key] = measure(samples[name]) if name in samples else None
        report[name] = entry
        a_img, b_img = runs["A"][0] / name / "left.ppm", runs["B"][0] / name / "left.ppm"
        if a_img.exists() and b_img.exists():
            def fmt(m):
                return "n/a" if not m else f"twist {m['final']['twist_deg']:+.0f} deg, shoulder {m['final']['shoulder_L_forward_m']:+.2f} m fwd"
            rows.append((name, a_img, b_img, f"A {fmt(entry['A'])} | B {fmt(entry['B'])}"))
    (out / "measurements.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    if rows:
        sheet(rows, out / "contact_sheet.jpg")
    for name, entry in report.items():
        print(name, *(f"{k}: {json.dumps(v['final']) if v else None}" for k, v in entry.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
