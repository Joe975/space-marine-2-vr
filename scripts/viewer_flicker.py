"""Lighting flicker in a headset recording, camera mode against first person
(docs/VIEWER_FLICKER_2026-10-27.md).

  python scripts/viewer_flicker.py "F:/Recordings/VR/2026-10-27 19-52-08.mp4" <run dir>
  python scripts/viewer_flicker.py <recording> <run dir> --from 742 --to 827 --events
  python scripts/viewer_flicker.py <prefix>.rgb <run dir>     (a stream from openvr_mirror_capture, both eyes)

The recording is the OBS capture of the SteamVR mirror (one eye, 60 fps). The game renders about
30 pairs a second, so most game frames appear twice; repeats are dropped first. The camera-mode
intervals come from the run's `VrViewer: state=active|off` log lines, placed on the video by the
recording's start time (taken from an OBS file name such as `2026-10-27 19-52-08.mp4`, or
`--recording-start HH:MM:SS`).

Two measures, per unique game frame:
  blip   a cell of an 8x8 grid that jumps by more than 8 luma levels for one game frame and comes
         straight back (a light switching on or off for a frame). Camera motion moves a cell
         monotonically, so it rarely does this. Reported per 100 frames, binned by image motion
         (mean absolute difference to the neighbouring frames), because fast motion makes more.
  flash  the whole frame's red excess (red minus the larger of green and blue) jumps for one or
         two game frames: the unlit red and grey screen rectangles seen in camera mode. Real red
         (blood, the HUD) also counts, so look at the listed times.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

import numpy as np

W, H, GRID = 160, 88, 8
REPEAT = 0.3  # largest grid-cell luma change under which a capture frame repeats the previous game frame
BLIP = 8.0  # luma levels a cell must leave both neighbours by
MOTION_BINS = (0, 2, 4, 6, 9, 13, 1000)
FLASH_MIN, FLASH_RATIO, FLASH_OFFSET = 1.0, 4.0, 0.8
VIEWER = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\] .*VrViewer: state=(\w+) ")
OBS_NAME = re.compile(r"(\d\d)-(\d\d)-(\d\d)\.\w+$")


def seconds(h, m, s, ms=0):
    return int(h) * 3600 + int(m) * 60 + int(s) + int(ms) / 1000


def viewer_intervals(lines, start, end=float("inf")):
    """Camera-mode intervals in recording seconds; `start` is the recording's clock time in seconds."""
    intervals, opened = [], None
    for line in lines:
        m = VIEWER.match(line)
        if not m:
            continue
        t = seconds(*m.groups()[:4]) - start
        state = m.group(5)
        if state == "active" and opened is None:
            opened = t
        elif state == "off" and opened is not None:
            intervals.append((opened, t))
            opened = None
    if opened is not None:
        intervals.append((opened, end))
    return [(a, b) for a, b in intervals if b > 0]


def inside(t, intervals):
    return any(a <= t <= b for a, b in intervals)


def luma(rgb):
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def unique_frames(gray):
    """Indices of capture frames that show a new game frame: some grid cell changed. (A whole-image
    mean would drop a frame where only one cell blips while the view is still.)"""
    c = cells(gray)
    keep = [0]
    for i in range(1, len(gray)):
        if np.abs(c[i] - c[keep[-1]]).max() > REPEAT:
            keep.append(i)
    return np.array(keep)


def cells(gray):
    n, h, w = gray.shape
    return gray[:, : h - h % GRID, : w - w % GRID].reshape(n, GRID, h // GRID, GRID, w // GRID).mean(axis=(2, 4))


def blips(gray):
    """Per unique frame (aligned with gray[1:-1]): blip cells brighter, blip cells darker, motion."""
    c = cells(gray)
    a, b, n = c[:-2], c[1:-1], c[2:]
    hit = (np.abs(b - a) > BLIP) & (np.abs(b - n) > BLIP) & (np.abs(a - n) < BLIP / 3) & (np.sign(b - a) == np.sign(b - n))
    up = (hit & (b > a)).sum(axis=(1, 2))
    down = (hit & (b < a)).sum(axis=(1, 2))
    motion = 0.5 * (np.abs(gray[1:-1] - gray[:-2]).mean(axis=(1, 2)) + np.abs(gray[2:] - gray[1:-1]).mean(axis=(1, 2)))
    return up, down, motion


def blip_cells(gray):
    """Per unique frame (aligned with gray[1:-1]): the boolean 8x8 map of blip cells."""
    c = cells(gray)
    a, b, n = c[:-2], c[1:-1], c[2:]
    return (np.abs(b - a) > BLIP) & (np.abs(b - n) > BLIP) & (np.abs(a - n) < BLIP / 3) & (np.sign(b - a) == np.sign(b - n))


def one_eye_blips(left, right, reach=(1, 1, 2)):
    """Blips that show in one eye only: (left-only frames, right-only frames, frames) over the game frames
    of a stereo stream. A blip cell counts for one eye when the other eye has no blip within `reach`
    (frames, rows, columns): the eyes see the same scene a little shifted, so a real event (a spark, a
    lamp blinking) blips in both, and only a fault in one eye's rendering blips in one."""
    keep = unique_frames(np.concatenate([left, right], axis=2))
    hl, hr = blip_cells(left[keep]), blip_cells(right[keep])
    dt, dy, dx = reach

    def near(h):
        out = np.zeros_like(h)
        n, gy, gx = h.shape
        for t in range(-dt, dt + 1):
            for y in range(-dy, dy + 1):
                for x in range(-dx, dx + 1):
                    out[max(0, t):n + min(0, t), max(0, y):gy + min(0, y), max(0, x):gx + min(0, x)] |= \
                        h[max(0, -t):n - max(0, t), max(0, -y):gy - max(0, y), max(0, -x):gx - max(0, x)]
        return out

    only_left = (hl & ~near(hr)).any(axis=(1, 2))
    only_right = (hr & ~near(hl)).any(axis=(1, 2))
    return int(only_left.sum()), int(only_right.sum()), len(hl)


def one_eye_changes(left, right, times):
    """Capture frames where one eye's image changed and the other's did not: (left-only, right-only,
    seconds). On a still view the eyes should change together or not at all (the mirror stream writes
    a frame only when something changed), so changes in one eye alone measure that eye's instability
    without needing motion."""
    if len(left) < 2:
        return 0, 0, 0.0
    changed = []
    for eye in (left, right):
        c = cells(eye)
        changed.append(np.abs(np.diff(c, axis=0)).max(axis=(1, 2)) > REPEAT)
    only_left = int((changed[0] & ~changed[1]).sum())
    only_right = int((changed[1] & ~changed[0]).sum())
    return only_left, only_right, float(times[-1] - times[0])


def red_excess(rgb):
    return np.clip(rgb[..., 0] - np.maximum(rgb[..., 1], rgb[..., 2]), 0, None).mean(axis=(1, 2))


def flashes(red):
    """Indices where the red excess spikes for at most two frames above its surroundings."""
    found = []
    for t in range(8, len(red) - 8):
        around = np.r_[red[t - 8:t - 2], red[t + 3:t + 9]]
        if red[t] > FLASH_MIN and red[t] > FLASH_RATIO * np.median(around) + FLASH_OFFSET and red[t] == red[t - 2:t + 3].max():
            if not found or t - found[-1] > 4:
                found.append(t)
    return found


def decode(path, start=None, end=None, fps=60.0):
    cmd = ["ffmpeg", "-v", "error"]
    if start is not None:
        cmd += ["-ss", str(start)]
    cmd += ["-i", str(path)]
    if end is not None:
        cmd += ["-t", str(end - (start or 0))]
    cmd += ["-vf", f"scale={W}:{H}:flags=area", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, H, W, 3).astype(np.float32)


def analyse(rgb, intervals, offset=0.0, fps=60.0, times=None):
    """Blip and flash statistics split into camera mode and the rest. `times` gives each captured
    frame's time on the intervals' clock; without it frame i is at offset + i / fps."""
    times = offset + np.arange(len(rgb)) / fps if times is None else np.asarray(times, float)
    gray = luma(rgb)
    keep = unique_frames(gray)
    up, down, motion = blips(gray[keep])
    viewer = np.array([inside(t, intervals) for t in times[keep[1:-1]]], bool)
    red = red_excess(rgb)
    flash_times = [float(times[i]) for i in flashes(red)]
    times = times[keep[1:-1]]
    out = {}
    for name, sel in (("camera mode", viewer), ("first person", ~viewer)):
        hit = (up + down)[sel] > 0
        bins = []
        for lo, hi in zip(MOTION_BINS[:-1], MOTION_BINS[1:]):
            b = (motion[sel] >= lo) & (motion[sel] < hi)
            bins.append((lo, hi, int(b.sum()), int(hit[b].sum())))
        out[name] = {"frames": int(sel.sum()), "blips": int(hit.sum()), "up": int(up[sel].sum()), "down": int(down[sel].sum()),
                     "bins": bins, "events": [(float(t), int(u), int(d)) for t, u, d in zip(times[sel], up[sel], down[sel]) if u + d],
                     "flashes": [t for t in flash_times if inside(t, intervals) == (name == "camera mode")]}
    return out


def report(stats, events=False):
    lines = []
    for name, s in stats.items():
        rate = 100 * s["blips"] / s["frames"] if s["frames"] else 0.0
        lines.append(f"{name}: {s['frames']} game frames, {s['blips']} with a blip ({rate:.2f} per 100; cells up {s['up']}, down {s['down']}), "
                     f"{len(s['flashes'])} red flashes")
        for lo, hi, frames, hits in s["bins"]:
            if frames:
                lines.append(f"  motion {lo:>3}-{hi:<4} frames {frames:5d}  blips {hits:4d}  per 100 {100 * hits / frames:5.2f}")
        if s["flashes"]:
            lines.append("  red flashes at " + ", ".join(f"{t:.2f}s" for t in s["flashes"]))
        if events:
            for t, u, d in s["events"]:
                lines.append(f"  blip {t:8.2f}s  up {u} down {d}")
    return "\n".join(lines)


def load_mirror(prefix, mapped=False):
    """A stream from `openvr_mirror_capture <prefix> stream`: both eyes side by side, and each frame's
    local clock time in seconds. Returns (left eye frames, right eye frames, times). mapped=True maps the
    file instead of reading it (the views keep it open)."""
    prefix = Path(str(prefix).removesuffix(".rgb"))
    info = json.loads(prefix.with_suffix(".json").read_text())
    read = (lambda p: np.memmap(p, np.uint8, mode="r")) if mapped else (lambda p: np.fromfile(p, np.uint8))
    raw = read(prefix.with_suffix(".rgb"))
    size = info["height"] * info["width"] * 3
    frames = raw[: len(raw) - len(raw) % size].reshape(-1, info["height"], info["width"], 3)  # a killed capture ends mid-frame
    times = np.array([seconds(*line.split()[1].replace(".", ":").split(":"))
                      for line in prefix.with_suffix(".times").read_text().splitlines() if len(line.split()) >= 2])
    n = min(len(frames), len(times))
    eye = info["eye_width"]
    return frames[:n, :, :eye], frames[:n, :, eye:], times[:n]


def mirror_luma(prefix, chunk=128):
    """load_mirror as float32 luma per eye, converted a chunk of frames at a time, so a 1/8- or 1/4-size
    stream does not need a float copy of all its RGB. Returns (left luma, right luma, times)."""
    left, right, times = load_mirror(prefix, mapped=True)
    out = []
    for eye in (left, right):
        y = np.empty(eye.shape[:3], np.float32)
        for i in range(0, len(eye), chunk):
            y[i:i + chunk] = luma(np.asarray(eye[i:i + chunk], np.float32))
        out.append(y)
    del left, right
    return out[0], out[1], times


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("recording", type=Path)
    p.add_argument("run", type=Path, help="run folder holding sm2vr.log, or the log itself")
    p.add_argument("--recording-start", help="clock time the recording started, HH:MM:SS (default: from the OBS file name)")
    p.add_argument("--from", dest="start", type=float, help="first recording second to read")
    p.add_argument("--to", dest="end", type=float, help="last recording second to read")
    p.add_argument("--events", action="store_true", help="list every blip")
    a = p.parse_args()
    log = a.run / "sm2vr.log" if a.run.is_dir() else a.run
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    if a.recording.suffix in (".rgb", ".times", ".json"):
        left, right, times = load_mirror(a.recording.with_suffix(""))
        intervals = viewer_intervals(lines, 0.0)
        print(f"mirror stream: {len(times)} frames, {times[-1] - times[0]:.1f} s, from {a.recording.stem}")
        for name, eye in (("left eye", left), ("right eye", right)):
            print(f"== {name}")
            print(report(analyse(eye.astype(np.float32), intervals, times=times), a.events))
        only_left, only_right, frames = one_eye_blips(luma(left.astype(np.float32)), luma(right.astype(np.float32)))
        print(f"== one eye only: {frames} game frames; left-only blips in {only_left} ({100 * only_left / max(frames, 1):.2f} per 100), "
              f"right-only in {only_right} ({100 * only_right / max(frames, 1):.2f} per 100)")
        return
    clock = a.recording_start or ":".join(OBS_NAME.search(a.recording.name).groups())
    start = seconds(*clock.split(":"))
    intervals = viewer_intervals(lines, start)
    print("camera mode (recording s): " + ", ".join(f"{x:.0f}-{y:.0f}" for x, y in intervals))
    rgb = decode(a.recording, a.start, a.end)
    print(report(analyse(rgb, intervals, a.start or 0.0), a.events))


if __name__ == "__main__":
    main()
