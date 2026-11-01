"""Benchmark a warmed-up ordinary OpenVR scene using the fixed virtual-player hold.

Keep the game foreground and F11 hidden. Do not build, test, capture screenshots
or send other input until this command returns. Outputs retain exact requests,
driver states, clock correlation, game trace and qualified PresentMon summaries.
"""
import argparse
import csv
import datetime
import json
from pathlib import Path
import subprocess
import time
import uuid

from analyze_performance import analyze
from qualify_driver_performance import qualify, qualify_allocation, load_records
from vr_test_driver import compile_script, write_request, mailbox, report_trace


def read_status(path):
    # Atomic MoveFileEx can briefly conflict with Windows readers. Never invent
    # a state or reuse an old one: retry at most 100ms, then fail the capture.
    for attempt in range(6):
        try:
            return json.loads(path.read_text())
        except (PermissionError, FileNotFoundError):
            if attempt == 5:
                raise
            time.sleep(.02)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("label")
    parser.add_argument("game_process_id", type=int)
    parser.add_argument("--allocation-mode", choices=("committed", "pooled"))
    args = parser.parse_args()
    out = args.output_directory
    if not out.is_absolute() or not out.is_dir() or not args.label.replace("_", "").replace("-", "").isalnum():
        parser.error("existing absolute output directory and plain filename label required")
    if list(out.glob(args.label+".*")):
        parser.error("refusing to overwrite an existing capture")
    root = Path(__file__).resolve().parents[1]
    directory = mailbox()
    scenario = json.loads((root/"config/test_driver/performance_stationary.json").read_text())
    request_id = uuid.uuid4().hex
    statuses = []
    process = None

    def save(suffix, value):
        (out/(args.label+suffix)).write_text(json.dumps(value, indent=2)+"\n", encoding="utf-8")

    def status():
        state = read_status(directory/"status.json")
        statuses.append(dict(state, qpc_ms=time.perf_counter_ns()/1e6))
        return state

    # Refuse loading/no backend or an active earlier request. The caller should
    # inspect the mirror and warm the scene before invoking this capture.
    try:
        previous = status()
    except FileNotFoundError:
        parser.error("driver is not ready; wait for the scene to load")
    if previous["state"] in ("preroll", "running"):
        parser.error("driver busy; wait for it to finish or explicitly stop it")
    clock = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "qpc_ms": time.perf_counter_ns()/1e6}
    save(".request.json", {"id": request_id, "scenario": scenario, "mailbox": str(directory)})
    save(".clock.json", clock)
    write_request(directory, compile_script(scenario, request_id))
    try:
        deadline = time.monotonic()+20
        while True:
            state = status()
            if state["id"] == request_id and state["state"] == "running" and state["elapsed_ms"] >= 1500:
                break
            if state["id"] == request_id and (state["error"] or state["state"] not in ("preroll", "running")):
                raise RuntimeError(state)
            if time.monotonic() > deadline:
                raise RuntimeError("driver did not start")
            time.sleep(.2)
        with (out/(args.label+".capture.log")).open("w") as log:
            command = ["pwsh", "-NoProfile", "-File", str(root/"scripts/capture-performance.ps1"),
                       "-GameProcessId", str(args.game_process_id), "-OutputDirectory", str(out), "-Label", args.label,
                       "-Mode", "full", "-Seconds", "30", "-WarmupSeconds", "5"]
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            invalid = []
            while process.poll() is None:
                state = status()
                if state["id"] != request_id or state["state"] != "running" or state["error"]:
                    invalid.append(state)
                time.sleep(.25)
            if process.returncode or invalid:
                raise RuntimeError({"capture_exit": process.returncode, "invalid_driver_states": invalid[:2]})
        deadline = time.monotonic()+20
        while True:
            state = status()
            if state["id"] != request_id:
                raise RuntimeError("request replaced")
            if state["state"] == "completed":
                break
            if state["state"] != "running" or time.monotonic() > deadline:
                raise RuntimeError(state)
            time.sleep(.25)
        with (out/(args.label+".csv")).open(encoding="utf-8-sig", newline="") as pm, (out/"cpu_trace.csv").open(encoding="utf-8-sig", newline="") as cpu:
            summary = analyze(pm, cpu, "full", True, "single_frame")
        trace_path = out/(args.label+".trace.jsonl")
        trace_path.write_bytes((Path.home()/"AppData/Local/sm2vr/sm2vr_trace.jsonl").read_bytes())
        records = load_records(trace_path)
        driver = report_trace(trace_path.read_text(encoding="utf-8").splitlines(), request_id)
        save(".driver.json", driver)
        qualified = qualify(summary, clock, statuses, records, request_id)
        if args.allocation_mode:
            with (out/"cpu_trace.csv").open(encoding="utf-8-sig", newline="") as cpu:
                qualified["allocation"] = qualify_allocation(csv.DictReader(cpu), summary, args.allocation_mode)
        save(".qualification.json", qualified)
        save(".summary.json", summary)
        print(json.dumps({"id": request_id, "fresh_pairs_per_second": summary["fresh_pairs_per_second"],
                          "p99_ms": summary["present_interval_ms"]["p99"], "qualification": qualified}))
    finally:
        save(".statuses.json", statuses)
        if process is not None and process.poll() is None:
            # PresentMon has its own fixed 30s lifetime. Do not leave our capture
            # running in the background after a qualification/read failure.
            process.wait(timeout=50)


if __name__ == "__main__":
    main()
