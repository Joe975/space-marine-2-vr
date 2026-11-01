"""Compare test-driver outcomes between two runs, e.g. OpenXR against OpenVR.

  .venv/Scripts/python.exe -I scripts/compare_driver_runs.py A.json A_trace.jsonl B.json B_trace.jsonl
A/B.json come from scripts/run_driver_scenarios.py; traces are the archived
build/run/<run>/sm2vr_trace.jsonl. Prints one line per scenario and writes
build/driver_parity/comparison.json. Native button sequences should match exactly; root
displacement differs slightly with timing.
"""
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def report(trace, request):
    out = subprocess.run([sys.executable, "-I", str(ROOT / "scripts" / "vr_test_driver.py"), "report", str(trace), "--id", request],
                         capture_output=True, text=True, timeout=120)
    return json.loads(out.stdout) if out.returncode == 0 else {"error": out.stderr.strip()[-300:]}


def summary(r):
    if "error" in r:
        return {"error": r["error"]}
    events = r.get("events", [])
    buttons = [f"{e['name']}{'+' if e.get('down') else '-'}" for e in events if e.get("event") == "button"]
    kinds = Counter(e.get("event") for e in events if e.get("event") not in ("button", "test_driver"))
    view = r.get("root_displacement_initial_view_axes") or {}
    return {"terminal": r.get("terminal"), "samples": r.get("samples"), "buttons": buttons, "events": dict(kinds),
            "view_right_m": round(view.get("right_metres", 0), 2), "view_forward_m": round(view.get("forward_metres", 0), 2)}


def main(a_runs, a_trace, b_runs, b_trace):
    a = {x["scenario"]: x for x in json.loads(Path(a_runs).read_text())}
    b = {x["scenario"]: x for x in json.loads(Path(b_runs).read_text())}
    rows = []
    for name in a:
        if name not in b:
            continue
        sa, sb = summary(report(a_trace, a[name]["id"])), summary(report(b_trace, b[name]["id"]))
        same_buttons = sa.get("buttons") == sb.get("buttons")
        rows.append({"scenario": name, "state": [a[name]["state"], b[name]["state"]], "same_buttons": same_buttons,
                     "a": sa, "b": sb})
        print(f"{name:24} {a[name]['state']:>9}/{b[name]['state']:<9} buttons {'same' if same_buttons else 'DIFFER'}"
              f"  a={len(sa.get('buttons', []))} b={len(sb.get('buttons', []))}"
              f"  view a=({sa.get('view_right_m')},{sa.get('view_forward_m')}) b=({sb.get('view_right_m')},{sb.get('view_forward_m')})")
    out = ROOT / "build" / "driver_parity"
    out.mkdir(parents=True, exist_ok=True)
    (out / "comparison.json").write_text(json.dumps(rows, indent=1))
    return rows


if __name__ == "__main__":
    main(*sys.argv[1:5])
