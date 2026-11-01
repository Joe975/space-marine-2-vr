"""Hard wall-time supervisor. Run with the repository virtual environment."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


def run_child(command: list[str], timeout: float, receipt: Path) -> dict:
    if not 0 < timeout <= 180:
        raise ValueError("timeout must be greater than zero and at most 180 seconds")
    if not receipt.is_absolute() or receipt.exists():
        raise ValueError("fresh absolute supervisor receipt required")
    result = {"status": "incomplete", "timeout_seconds": timeout, "child_exit": None, "terminated": False}
    # Exclusive reservation precedes any child creation; never truncate another run's receipt.
    with receipt.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
    started = time.monotonic()
    child = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Avoid Popen's context-manager: its __exit__ can wait without a deadline.
    try:
        child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        result["status"] = "termination_unconfirmed_incomplete"
        try:
            child.kill()
            child.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            pass
        else:
            result.update(status="timed_out_incomplete", terminated=True)
    else:
        result["status"] = "child_exited" if child.returncode == 0 else "child_failed"
    result["child_exit"] = child.returncode
    result["elapsed_seconds"] = time.monotonic() - started
    receipt.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=120, choices=range(1, 181))
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    arguments = args.arguments
    if arguments and arguments[0] == "--":
        arguments = arguments[1:]
    if not args.exe.is_absolute() or not args.exe.is_file() or args.exe.name != "native_replay_extract.exe":
        parser.error("absolute built native_replay_extract.exe required")
    if len(arguments) < 3 or arguments[0] not in ("inventory", "event", "scan"):
        parser.error("inventory/event/scan CAPTURE OUTDIR arguments required")
    output = Path(arguments[2])
    if not output.is_absolute() or output.exists():
        parser.error("fresh absolute output directory required")
    receipt = output.with_name(output.name + ".supervisor.json")
    result = run_child([str(args.exe), *arguments], args.timeout, receipt)
    if result["status"] != "child_exited":
        return 1
    try:
        data = json.loads((output / "result.json").read_text(encoding="utf-8"))
        complete = isinstance(data, dict) and data.get("status") == "complete"
    except (OSError, ValueError):
        complete = False
    result["status"] = "complete" if complete else "child_result_incomplete"
    receipt.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0 if complete else 1


if __name__ == "__main__":
    sys.exit(main())
