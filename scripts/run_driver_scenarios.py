"""Queue test-driver scenarios one after another and record their IDs and end states.

Used for the OpenXR/OpenVR parity check (docs/OPENXR_MIGRATION_PLAN.md, phase 5): launch
run-ordinary-headset.ps1 -Runtime X -Controls -Gestures -DirectLaunch -TestDriver -ScriptedHead,
wait for stereo, then run
  .venv/Scripts/python.exe -I scripts/run_driver_scenarios.py <label> [scenario ...]
Results go to build/driver_parity/scenarios_<label>.json; compare two labels with
scripts/compare_driver_runs.py.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = str(ROOT / "scripts" / "vr_test_driver.py")
SCENARIOS = ["dodge_left", "dodge_forward", "sprint_click_headings", "hands_50cm_rotation", "raised_fist_emote",
             "holster_secondary", "hip_sidearm_roundtrip", "support_grip_aim", "grenade_grab_throw",
             "chainsword_rev_analog", "chainsword_rev_default", "pistol_fire_rev_reach", "combat_ownership", "hud_status_hold",
             "menu_tap", "pause_menu_panel", "viewer_tap_menu", "viewer_fly", "viewer_hold_menu"]
ACTIVE = ("preroll", "running")  # then completed | rejected | stopped


def cli(*args):
    out = subprocess.run([sys.executable, "-I", CLI, *args], capture_output=True, text=True, timeout=30)
    return json.loads(out.stdout.strip().splitlines()[-1]) if out.stdout.strip() else {"error": out.stderr.strip()}


def main(label, names):
    results = []
    for name in names:
        path = ROOT / "config" / "test_driver" / f"{name}.json"
        duration = json.loads(path.read_text())["duration_ms"] / 1000
        queued = cli("run", str(path))
        start = time.time()
        status = {}
        while time.time() - start < duration + 20:
            time.sleep(0.5)
            status = cli("status")
            if status.get("id") == queued.get("id") and status.get("state") not in ACTIVE:
                break
        else:
            cli("stop")
            time.sleep(1)
        results.append({"scenario": name, "id": queued.get("id"), "state": status.get("state"), "error": status.get("error"),
                        "elapsed_ms": status.get("elapsed_ms")})
        print(json.dumps(results[-1]), flush=True)
        time.sleep(3)
    out = ROOT / "build" / "driver_parity"
    out.mkdir(parents=True, exist_ok=True)
    (out / f"scenarios_{label}.json").write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:] or SCENARIOS)
