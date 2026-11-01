"""Check a run's sm2vr.log for the game-patch port: every hook installed or bound, no build refusals,
stereo pairs produced. Prints one line per check and exits 1 if any required check fails.

    python -I scripts/check_game_build_run.py build/run/<dir>/sm2vr.log [--controls] [--stereo]

`--controls` adds the -Controls hooks (input, body, arms, chainsword); `--stereo` requires the
ordinary stereo producer to have delivered pairs. Standard library only.
"""
import argparse
import re
import sys
from pathlib import Path

ALWAYS = [
    ("build identified and known", r"GameBuild: exe timestamp 0x[0-9a-f]+ \([^)]*\) known=true", "any"),
    ("camera list add hooked", r"EngineCameraProbe: hooked camera-list add", "any"),
    ("projection builder hooked", r"VrProjection: hooked builder", "any"),
    ("camera probe receiving calls", r"EngineCameraProbe: state=1 calls=[1-9]", "any"),
    ("projection receiving calls", r"VrProjection: state=1 calls=[1-9]", "any"),
    # Since 26 Oct the screen UI hook is installed only for eye snapshots or SM2VR_DIAG_SCREEN_UI=1;
    # a run without it has no ScreenUi line, and that passes.
    ("screen UI hook", r"ScreenUi: installed=true", "optional:ScreenUi: installed="),
    ("world markers and readers", r"WorldMarkers: installed=true .*readers_installed=true", "last:WorldMarkers: installed="),
    ("tracker guard on exe and dll", r"TrackerHandleGuard: enabled=true exe_installed=true dll_installed=true", "last:TrackerHandleGuard: enabled="),
]
CONTROLS = [
    ("controller registration hook", r"EngineControllerRegistration: native input-update hook installed", "any"),
    ("XInput slot registered", r"EngineControllerRegistration: registered native XInput slot zero", "any"),
    ("body probe installed", r"EngineBodyProbe: passive=\w+ installed=true", "any"),
    ("cinematic state bound", r"CinematicState: bound=true", "any"),
    ("body following installed", r"BodyFollowing: installed=true", "any"),
    ("player opacity installed", r"PlayerOpacity: installed=true", "any"),
    ("chainsword rev bound", r"ChainswordRev: bound=true", "last:ChainswordRev: bound="),
]
STEREO = [
    ("stereo pairs delivered", r"OrdinaryHeadsetRetention: ready=true .*pairs=[1-9]", "any"),
    ("stereo pairs submitted (100+)", r"OrdinaryHeadset: consumed=\d+ submitted=[1-9]\d{2,} ", "last:OrdinaryHeadset: consumed="),
]
FORBIDDEN = [
    ("a hook refused the build", r"not hooking|prologue mismatch|unsupported executable"),
    ("unknown game build", r"GameBuild: unknown build"),
    ("tracker guard refused an image", r"TrackerHandleGuard: image=\w+ installed=false refused=true"),
    # The flat head-locked fallback also submits frames to the headset; that is not stereo
    # (2026-10-21 11:17: one pair, then bloom refused and the run fell back to flat).
    ("stereo stop", r"OrdinaryHeadsetStopped:|VrFlatFallback: active=true reason=stopped"),
]


def check(lines, controls=False, stereo=False):
    results = []
    for name, pattern, mode in ALWAYS + (CONTROLS if controls else []) + (STEREO if stereo else []):
        rx = re.compile(pattern)
        if mode == "any":
            ok = any(rx.search(l) for l in lines)
        else:
            kind, prefix = mode.split(":", 1)
            last = next((l for l in reversed(lines) if prefix in l), None)
            ok = bool(last and rx.search(last)) or (kind == "optional" and last is None)
        results.append((name, ok))
    for name, pattern in FORBIDDEN:
        rx = re.compile(pattern)
        hit = next((l for l in lines if rx.search(l)), None)
        results.append((f"no {name}", hit is None))
    return results


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("log")
    p.add_argument("--controls", action="store_true")
    p.add_argument("--stereo", action="store_true")
    a = p.parse_args(argv)
    lines = Path(a.log).read_text(errors="replace").splitlines()
    results = check(lines, a.controls, a.stereo)
    for name, ok in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())
