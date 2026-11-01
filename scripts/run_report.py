"""Run report: what a mod log saw, against the table of states seen before.

Usage:
    run_report.py <run dir | sm2vr.log> [--strict] [--json] [--learn] [--table PATH]

Reads the mod log (a run directory's sm2vr.log, or a log path) and reports:

- every native flag mask (published|requested) seen, and the scene mode it maps to;
- owned readiness reasons, packet and bloom refusal codes, flat-fallback reasons, scene modes;
- stereo lifecycle: stops, suspensions, recoveries, restarts, cold restarts, re-arms; hangs;
- owned pair renewals after a rebuilt pool: how many reused a retired pair, how many pairs were made;
- call-census hooks that doubled in stereo;
- anything not in the known-state table (config/known_run_states.json) as NEW, with when it was
  first seen and how often;
- verdicts: stereo stopped for good, a suspension never recovered, a hang, a doubled hook.

--strict exits 1 on any verdict or NEW item. --learn adds the run's unseen values to the table
(look at them first: the table is what "normal" means). See docs/GENERIC_GUARDS_2026-10-26.md.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "config" / "known_run_states.json"

LINE = re.compile(r"^\[(?P<time>[0-9:.]+)\] \[(?P<level>[A-Z])\] \[tid:\d+\] (?P<kind>[A-Za-z0-9_]+):(?P<rest>.*)$")
FIELD = re.compile(r"(\w+)=(\S+)")
EXCEPTION = re.compile(r"^\[(?P<time>[0-9:.]+)\] \[E\] \[tid:\d+\] Exception occurred: (?P<code>\w+)")
# The crash handler's register dump and module list: part of an exception, not kinds of their own.
DUMP_KINDS = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "RIP", "R8", "R9", "R10", "R11", "R12",
              "R13", "R14", "R15", "EFLAGS", "CS", "DS", "ES", "FS", "GS", "SS", "Module"}

# Categories the table knows. Each maps a category name to how a value is pulled from a line.
# A suspension that took longer than this to recover is a verdict (26 Oct 11:55: 78 s at the pair cap).
SLOW_RECOVERY_MS = 60000
CATEGORIES = ("masks", "scene_modes", "readiness_reasons", "packet_refusals", "bloom_refusals",
              "fallback_reasons", "warning_kinds", "doubled_hooks", "missing_hooks")


@dataclass
class Seen:
    first: str
    count: int = 0


@dataclass
class Report:
    log: Path
    values: dict[str, dict[str, Seen]] = field(default_factory=lambda: {c: {} for c in CATEGORIES})
    events: list[tuple[str, str, str]] = field(default_factory=list)  # (time, kind, summary)
    verdicts: list[str] = field(default_factory=list)
    new: dict[str, list[str]] = field(default_factory=dict)
    lines: int = 0
    stereo_pairs: bool = False
    # Owned pair renewals after a rebuilt pool: how many took a retired pair again, and the most pairs created.
    renewals: int = 0
    renewal_reuses: int = 0
    pairs: int = 0

    def note(self, category: str, value: str, time: str) -> None:
        seen = self.values[category].setdefault(value, Seen(time))
        seen.count += 1


def mask_text(value: int) -> str:
    return f"{value:#x}"


def parse_hex(text: str) -> int | None:
    try:
        return int(text, 16) if text.lower().startswith("0x") else int(text)
    except ValueError:
        return None


def native_mask(fields: dict[str, str]) -> int | None:
    """published|requested from a CinematicState line or a native_flags=P/R field."""
    if "published" in fields and "requested" in fields:
        published, requested = parse_hex(fields["published"]), parse_hex(fields["requested"])
        if published is not None and requested is not None:
            return published | requested
    if "native_flags" in fields:
        parts = fields["native_flags"].split("/")
        values = [parse_hex(p) for p in parts]
        if values and all(v is not None for v in values):
            out = 0
            for v in values:
                out |= v  # type: ignore[operator]
            return out
    if "mask" in fields:
        return parse_hex(fields["mask"])
    return None


def read(log: Path) -> Report:
    report = Report(log)
    stopped_at: str | None = None
    suspended_at: str | None = None
    for raw in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE.match(raw)
        if not match:
            if exception := EXCEPTION.match(raw):
                report.events.append((exception["time"], "Exception", exception["code"]))
                report.verdicts.append(f"exception {exception['code']} at {exception['time']} (crash handler)")
            continue
        report.lines += 1
        time, level, kind, rest = match["time"], match["level"], match["kind"], match["rest"]
        fields = dict(FIELD.findall(rest))
        if level in ("W", "E") and kind not in DUMP_KINDS:
            report.note("warning_kinds", kind, time)
        if kind == "CinematicState" and fields.get("available") == "true" or kind in ("VrMenuPanel", "VrScreenMenu", "SceneMode"):
            mask = native_mask(fields)
            if mask is not None:
                report.note("masks", mask_text(mask), time)
        if kind == "SceneMode" and "mode" in fields:
            report.note("scene_modes", fields["mode"], time)
            # A mask no rule explains, or one with a bit no rule reads (SceneMode.h novel()).
            if fields["mode"] == "unknown" or fields.get("unknown_bits", "0x0") != "0x0":
                report.events.append((time, kind, f"classifier has no rule for mask {fields.get('mask')} (unknown_bits={fields.get('unknown_bits', '0x0')}) -> {fields['mode']}"))
        if kind == "OrdinaryOwnedReadiness" and "reason" in fields:
            report.note("readiness_reasons", fields["reason"], time)
        if kind.startswith("OrdinaryHeadset"):
            for key, category in (("packet_refusal", "packet_refusals"), ("bloom_refusal", "bloom_refusals")):
                # 999 is the producer's "no packet yet" placeholder, 0 is no refusal.
                if key in fields and fields[key] not in ("0", "999"):
                    report.note(category, fields[key], time)
        if kind == "OrdinaryHeadsetProducer" and fields.get("submitted", "0") != "0":
            report.stereo_pairs = True
        if kind == "OrdinaryHeadsetResumed" or kind == "OrdinaryHeadsetRecovered":
            report.stereo_pairs = True
        if kind == "VrFlatFallback" and "reason" in fields:
            report.note("fallback_reasons", fields["reason"], time)
        if kind == "CallCensus":  # NativeCallCensus.h: hooks whose stereo rate breaks their kind
            for key, category in (("doubled", "doubled_hooks"), ("missing", "missing_hooks")):
                for hook in fields.get(key, "[]").strip("[]").split(","):
                    if hook:
                        report.note(category, hook, time)
        # Stereo lifecycle.
        if kind == "OrdinaryHeadsetStopped":
            stopped_at = time
            report.events.append((time, kind, rest.strip()))
        elif kind == "OrdinaryHeadsetSuspended":
            suspended_at = suspended_at or time
            report.events.append((time, kind, rest.strip()))
        elif kind in ("OrdinaryHeadsetRecovered", "OrdinaryHeadsetRestarted", "OrdinaryHeadsetColdRestart"):
            report.events.append((time, kind, rest.strip()))
            if kind == "OrdinaryHeadsetRecovered":
                suspended_at = None
                held = fields.get("held_ms", "0")
                if held.isdigit() and int(held) > SLOW_RECOVERY_MS:
                    report.verdicts.append(f"stereo came back slowly at {time}: {int(held) // 1000} s on the flat panel")
            if kind == "OrdinaryHeadsetColdRestart":  # the whole stack back to its startup state
                stopped_at = suspended_at = None
        elif kind == "OrdinaryHeadsetRearm":
            report.events.append((time, kind, rest.strip()))
            if fields.get("armed") == "true":
                stopped_at = None
        elif kind == "OrdinaryDlssHold":  # OrdinaryDlssPresence: stereo waits for the game's DLSS or TAA
            report.events.append((time, kind, rest.strip()))
            if fields.get("held") == "true":
                report.verdicts.append(f"the game ran neither DLSS nor TAA from {time}: stereo can't run, flat panel until one is on again")
        elif kind == "OrdinaryBloomRenewed" and rest.strip().startswith("refused"):
            report.events.append((time, kind, rest.strip()))
            report.verdicts.append(f"owned pair cap reached at {time} ({rest.strip()[:60]}): a rebuilt pool can't be renewed")
        elif kind == "OrdinaryBloomRenewed":
            # reused=true: the rebuilt pool's layout had a retired pair (since 26 Oct); logs before that
            # have no reused field and always made a new pair.
            report.events.append((time, kind, rest.strip()))
            report.renewals += 1
            if fields.get("reused") == "true":
                report.renewal_reuses += 1
            if "generation" in fields and fields["generation"].isdigit():
                report.pairs = max(report.pairs, int(fields["generation"]))
        elif kind == "StereoStall":  # ReadinessStall: plain play, producer running, readiness blocked
            report.events.append((time, kind, rest.strip()))
            if fields.get("action") == "give_up" and level == "E":
                report.verdicts.append(f"stereo stalled in plain play at {time} ({fields.get('reason')}), rediscovery did not help")
        elif kind == "StereoForeignView":  # ForeignViewHold: a game PIP view behind the owned pair (slot 2+)
            report.events.append((time, kind, rest.strip()))
            report.verdicts.append(f"game PIP view behind the owned pair at {time} ({rest.strip()[:60]}): its HDR index overruns the game's table (docs/LOAD_CRASH_2026-10-26.md)")
        elif kind == "ForeignViewHold" and rest.strip().startswith("start"):
            report.events.append((time, kind, rest.strip()))
        elif kind == "HangWatchdog" and level == "E":
            report.events.append((time, kind, rest.strip()))
            report.verdicts.append(f"hang at {time}: {rest.strip()[:160]}")
    if stopped_at:
        report.verdicts.append(f"stereo stopped for good at {stopped_at} (no re-arm after it)")
    if suspended_at:
        report.verdicts.append(f"stereo suspended at {suspended_at} and never recovered")
    # Doubled and missing hooks are not verdicts of their own: one not yet in the table is NEW
    # (--strict fails); once it has been looked at, its label in the table says what it is.
    hang = log.parent / "dump" / "hang.dmp"
    if hang.exists():
        report.verdicts.append(f"hang dump present: {hang}")
    return report


def load_table(path: Path) -> dict:
    if not path.exists():
        return {c: {} for c in CATEGORIES}
    table = json.loads(path.read_text(encoding="utf-8"))
    for category in CATEGORIES:
        table.setdefault(category, {})
    return table


def compare(report: Report, table: dict) -> None:
    for category in CATEGORIES:
        known = table.get(category, {})
        fresh = [v for v in report.values[category] if v not in known]
        if fresh:
            report.new[category] = sorted(fresh, key=lambda v: report.values[category][v].first)


def learn(report: Report, table: dict, path: Path) -> int:
    added = 0
    for category, values in report.new.items():
        for value in values:
            table[category][value] = table[category].get(value, "") or f"learned from {report.log.parent.name}"
            added += 1
    path.write_text(json.dumps(table, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return added


def describe(report: Report, table: dict) -> str:
    out = [f"Run report: {report.log} ({report.lines} mod lines)"]
    out.append(f"Stereo pairs flowed: {'yes' if report.stereo_pairs else 'no'}")
    if report.renewals:
        out.append(f"Owned pair renewals: {report.renewals} ({report.renewal_reuses} reused a retired pair, "
                   f"{report.renewals - report.renewal_reuses} new layouts; {report.pairs} pairs created)")
    for category in CATEGORIES:
        values = report.values[category]
        if not values:
            continue
        known = table.get(category, {})
        items = []
        for value, seen in sorted(values.items(), key=lambda kv: kv[1].first):
            label = known.get(value)
            items.append(f"{value}" + (f" ({label})" if label else "") + f" x{seen.count}")
        out.append(f"{category}: " + ", ".join(items))
    if report.events:
        out.append("Stereo lifecycle:")
        out.extend(f"  {t} {k}: {s[:180]}" for t, k, s in report.events[:40])
        if len(report.events) > 40:
            out.append(f"  ... {len(report.events) - 40} more")
    for category, values in report.new.items():
        for value in values:
            seen = report.values[category][value]
            out.append(f"NEW {category} {value}: first {seen.first}, {seen.count} times")
    out.extend(f"VERDICT {v}" for v in report.verdicts)
    if not report.new and not report.verdicts:
        out.append("Nothing new, no verdicts.")
    return "\n".join(out)


def resolve(target: Path) -> Path:
    if target.is_dir():
        return target / "sm2vr.log"
    return target


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", type=Path, help="run directory or sm2vr.log")
    parser.add_argument("--table", type=Path, default=TABLE)
    parser.add_argument("--strict", action="store_true", help="exit 1 on any verdict or NEW item")
    parser.add_argument("--learn", action="store_true", help="add this run's NEW values to the table")
    parser.add_argument("--json", action="store_true", help="print a JSON summary instead of text")
    args = parser.parse_args(argv)
    log = resolve(args.target)
    if not log.exists():
        print(f"no mod log at {log}", file=sys.stderr)
        return 2
    table = load_table(args.table)
    report = read(log)
    compare(report, table)
    if args.json:
        print(json.dumps({
            "log": str(log), "lines": report.lines, "stereo_pairs": report.stereo_pairs,
            "renewals": {"total": report.renewals, "reused": report.renewal_reuses, "pairs": report.pairs},
            "values": {c: {v: {"first": s.first, "count": s.count} for v, s in vals.items()} for c, vals in report.values.items()},
            "new": report.new, "verdicts": report.verdicts,
            "events": [{"time": t, "kind": k, "summary": s} for t, k, s in report.events],
        }, indent=2))
    else:
        print(describe(report, table))
    if args.learn and report.new:
        print(f"learned {learn(report, table, args.table)} values into {args.table}")
    return 1 if args.strict and (report.new or report.verdicts) else 0


if __name__ == "__main__":
    sys.exit(main())
