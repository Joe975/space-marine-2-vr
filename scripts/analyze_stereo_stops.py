"""Classify how owned stereo ended (or recovered) in sm2vr.log files.

Usage: analyze_stereo_stops.py <run dir or sm2vr.log>... [--json]

Reports permanent stops (OrdinaryHeadsetStopped / first producer_stopped=true),
recoverable suspensions and recoveries, flat-panel fallback transitions and how
long fresh stereo pairs stayed frozen before the log ended. Read-only.
"frozen" (no stop line, game still presenting, pair count unchanged) is a hint only:
pause menus before the panel existed, loading and end-of-run waits look the same.
"""
import argparse
import json
import re
import sys
from pathlib import Path

# Bit names of native_texture_contract::Difference (mod/src/NativeTextureContract.h).
DIFFERENCE = ["incomplete", "module_changed", "factory_changed", "pool_changed", "pool_mode_changed", "banks_moved",
              "bank_changed", "bank_count_changed", "records_moved", "records_shrank", "variant_changed",
              "dimensions_changed", "targets_changed"]
# NativeOrdinaryBloom::Refusal and NativeTrackedOrdinaryCamera::Refusal names.
BLOOM_REFUSAL = ["none", "scope", "callsite", "duplicate", "creation", "continuity", "contract", "pool", "changed",
                 "temporal", "colour", "export_bank", "packet", "exception"]
PRODUCER_REFUSAL = ["none", "source", "job", "provider", "bank", "ticket", "native_slot", "projection", "changed",
                    "exception", "stopped", "culling", "initializing", "suspended", "foreign_view", "no_dlss"]

FROZEN_S = 10.0  # Fresh pairs unchanged this long before the log ended.
TIME = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\]")
FIELD = re.compile(r"(\w+)=(\S+)")


def seconds(line):
    m = TIME.match(line)
    if not m:
        return None
    h, mi, s, frac = m.groups()
    return int(h) * 3600 + int(mi) * 60 + int(s) + float("0." + frac)


def clock(value):
    if value is None:
        return None
    value %= 86400
    return f"{int(value // 3600):02d}:{int(value % 3600 // 60):02d}:{value % 60:06.3f}"


def fields(line):
    return dict(FIELD.findall(line))


def decode_difference(text):
    try:
        value = int(text, 0)
    except (TypeError, ValueError):
        return []
    return [name for bit, name in enumerate(DIFFERENCE) if value & (1 << bit)]


def name(table, text):
    try:
        index = int(text)
    except (TypeError, ValueError):
        return text
    return table[index] if 0 <= index < len(table) else str(index)


def analyze(lines):
    result = {"stops": [], "first_stopped_receipt": None, "suspensions": [], "recoveries": [], "fallback": [], "recovery": None, "rebases": [],
              "last_submitted": None, "submitted_frozen_since": None, "log_end": None}
    last_pairs = None
    presenting = 0  # Game presents at >= 5 fps since the pair count last changed.
    for line in lines:
        t = seconds(line)
        if t is not None:
            result["log_end"] = t
        if "OrdinaryHeadsetStopped:" in line:
            f = fields(line)
            result["stops"].append({"time": clock(t), "dispatch": int(f.get("dispatch", 0)),
                                    "packet_refusal": int(f.get("packet_refusal", 999)),
                                    "bloom_refusal": name(BLOOM_REFUSAL, f.get("bloom_refusal"))})
        elif "OrdinaryHeadsetSuspended:" in line:
            f = fields(line)
            result["suspensions"].append({"time": clock(t), "dispatch": int(f.get("dispatch", 0)),
                                          "bloom_refusal": name(BLOOM_REFUSAL, f.get("bloom_refusal")),
                                          # Recoverable tracked packet refusal (4/12/13) at a level transition; 999 before 23 Oct.
                                          "packet_refusal": int(f.get("packet_refusal", 999)),
                                          "contract_failure": f.get("contract_failure"),
                                          "changed": decode_difference(f.get("contract_difference")),
                                          "variant": [f.get("created_variant"), f.get("observed_variant")]})
        elif "OrdinaryHeadsetRecovered:" in line:
            f = fields(line)
            result["recoveries"].append({"time": clock(t), "dispatch": int(f.get("dispatch", 0)),
                                         "previous": int(f.get("previous", 0))})
        elif "OrdinaryBloomContractRebased:" in line:
            f = fields(line)
            result["rebases"].append({"time": clock(t), "records": f.get("records"), "record_count": f.get("record_count")})
        elif "OrdinaryStereoRecovery: suspended=" in line:
            # Totals: per-episode lines are thinned after 32 episodes, this summary is not.
            f = fields(line)
            result["recovery"] = {k: int(f[k]) for k in ("episodes", "attempts", "refused_attempts", "recoveries") if k in f}
            result["recovery"].update({k: f.get(k) == "true" for k in ("suspended", "exhausted")})
        elif "VrFlatFallback: active=" in line:
            f = fields(line)
            result["fallback"].append({"time": clock(t), "active": f.get("active") == "true", "reason": f.get("reason")})
        elif "OrdinaryHeadsetProducer:" in line and result["first_stopped_receipt"] is None:
            f = fields(line)
            if f.get("producer_stopped") == "true":
                result["first_stopped_receipt"] = {"time": clock(t), "job": int(f.get("job", 0)),
                                                   "accepted": f.get("accepted") == "true",
                                                   "refusal": name(PRODUCER_REFUSAL, f.get("refusal")),
                                                   "submitted": int(f.get("submitted", 0))}
        elif "Sm2Probe: presents=" in line:
            try:
                if float(fields(line).get("fps", 0)) >= 5 and last_pairs:
                    presenting += 1
            except ValueError:
                pass
        elif "VrSession: status=" in line and "submitted_pairs=" in line:
            pairs = int(fields(line)["submitted_pairs"])
            if pairs != last_pairs:
                result["submitted_frozen_since"] = t
                last_pairs = pairs
                presenting = 0
            result["last_submitted"] = pairs
    frozen = result.pop("submitted_frozen_since")
    end = result["log_end"]
    result["stereo_frozen_s"] = round(end - frozen, 3) if frozen is not None and end is not None else None
    result["log_end"] = clock(end)
    stopped = bool(result["stops"] or result["first_stopped_receipt"])
    if stopped:
        result["verdict"] = "stopped"
    elif result["suspensions"] and len(result["recoveries"]) < len(result["suspensions"]):
        result["verdict"] = "suspended"
    elif result["suspensions"]:
        result["verdict"] = "recovered"
    elif result["stereo_frozen_s"] is not None and result["stereo_frozen_s"] >= FROZEN_S and presenting >= 2:
        # No stop/suspension line (older builds, or a gate such as loading), but the
        # game kept presenting while no pair reached the headset.
        result["verdict"] = "frozen"
    else:
        result["verdict"] = "clean"
    return result


def log_path(argument):
    path = Path(argument)
    return path / "sm2vr.log" if path.is_dir() else path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="+")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    reports = {}
    for argument in args.paths:
        path = log_path(argument)
        if not path.is_file():
            continue
        with path.open(encoding="utf-8", errors="replace") as handle:
            reports[str(path.parent.name)] = analyze(handle)
    if args.json:
        print(json.dumps(reports, indent=2))
        return 0
    for run, r in reports.items():
        print(f"{run}: {r['verdict']} last_submitted={r['last_submitted']} stereo_frozen_s={r['stereo_frozen_s']} log_end={r['log_end']}")
        if r["recovery"]:
            print("  recovery totals " + " ".join(f"{k}={v}" for k, v in r["recovery"].items()))
        for stop in r["stops"]:
            print(f"  stop {stop['time']} dispatch={stop['dispatch']} packet_refusal={stop['packet_refusal']} bloom_refusal={stop['bloom_refusal']}")
        if r["first_stopped_receipt"]:
            s = r["first_stopped_receipt"]
            print(f"  producer stopped {s['time']} job={s['job']} refusal={s['refusal']} submitted={s['submitted']}")
        for s in r["suspensions"]:
            print(f"  suspended {s['time']} dispatch={s['dispatch']} {s['bloom_refusal']}/{s['contract_failure']} packet_refusal={s['packet_refusal']} changed={','.join(s['changed']) or '-'} variant={s['variant']}")
        for s in r["rebases"]:
            print(f"  contract re-based {s['time']} records={s['records']} record_count={s['record_count']}")
        for s in r["recoveries"]:
            print(f"  recovered {s['time']} dispatch={s['dispatch']} previous={s['previous']}")
        for s in r["fallback"]:
            print(f"  flat fallback {s['time']} active={s['active']} reason={s['reason']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
