"""Summarize passive same-Scene query dependencies in an explicit QPC window.

This can disprove simple wait deferral. Absence of an observed consumer cannot
prove buffering safe, particularly when key capture is truncated or unavailable.
"""
import argparse
from bisect import bisect_left
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
from analyze_performance import distribution


def analyze(rows, start, end):
    if not all(math.isfinite(x) for x in (start, end)) or end <= start:
        raise ValueError("finite increasing QPC window required")
    events = defaultdict(list)
    for row in rows:
        if "dependency_" not in row["event"]:
            continue
        t, d, v = (float(row[k]) for k in ("qpc_start_ms", "duration_ms", "value"))
        if not all(math.isfinite(x) for x in (t, d, v)) or d < 0:
            raise ValueError("invalid dependency event")
        if start <= t and t+d <= end:
            events[row["event"]].append((t, d, v))
    result = {}
    for eye in ("left", "right"):
        prefix = f"scene_{eye}_dependency_"
        def index(suffix, field):
            indexed = {}
            for record in events[prefix+suffix]:
                key = record[field]
                if key in indexed:
                    raise ValueError("duplicate dependency identity")
                indexed[key] = record
            return indexed
        summaries = index("summary", 0)
        counts = {name: index(name, 0) for name in
                  ("collects", "reads", "reads_before", "matched_reads", "gc_pending", "query_count", "keys_captured")}
        uses = index("publish_to_use_cpu", 2)
        metadata = {name: sorted(events[prefix+name]) for name in ("consumer_rva", "consumer_result", "consumer_column")}
        times = {name: [r[0] for r in records] for name, records in metadata.items()}
        complete, truncated, valid_reads = 0, 0, 0
        callers, columns, values = Counter(), Counter(), Counter()
        delays, gc, query_counts, read_counts = [], [], [], []
        for finish, (_, _, dispatch) in summaries.items():
            if dispatch <= 0 or dispatch != int(dispatch):
                raise ValueError("invalid dispatch")
            if any(finish not in records for records in counts.values()):
                continue
            c = {name: records[finish][2] for name, records in counts.items()}
            complete += 1
            gc.append(c["gc_pending"]);query_counts.append(c["query_count"]);read_counts.append(c["reads"])
            truncated += c["query_count"] < 0 or c["keys_captured"] < c["query_count"]
            if c["collects"] != 1 or c["matched_reads"] < 1 or dispatch not in uses:
                continue
            published, delay, _ = uses[dispatch]
            if published+delay > finish or c["query_count"] < 1:
                raise ValueError("consumer is outside its owning Scene")
            joined = {}
            for name, records in metadata.items():
                i = bisect_left(times[name], published+delay-0.000003)
                candidates = records[i:i+2]
                matches = [r for r in candidates if abs(r[0]-(published+delay)) <= 0.000003]
                if len(matches) == 1:
                    joined[name] = matches[0][2]
            if len(joined) != 3 or joined["consumer_result"] < 0:
                continue
            valid_reads += 1;delays.append(delay)
            callers[hex(int(joined["consumer_rva"]))] += 1
            columns[str(int(joined["consumer_column"]))] += 1
            values[str(int(joined["consumer_result"]))] += 1
        result[eye] = dict(scenes=len(summaries), complete_summaries=complete,
                           unavailable_or_truncated_key_scenes=truncated,
                           same_scene_published_key_valid_consumers=valid_reads,
                           publish_to_consumer_ms=distribution(delays), first_consumer_rvas=dict(callers),
                           first_consumer_columns=dict(columns), first_consumer_results=dict(values),
                           gc_pending=distribution(gc), query_count=distribution(query_counts), reads=distribution(read_counts))
    proven = sum(r["same_scene_published_key_valid_consumers"] for r in result.values())
    return dict(qpc_start_ms=start, qpc_end_ms=end, eyes=result,
                decision="no_go_for_simple_wait_deferral" if proven else "inconclusive",
                buffering_safety_proven=False,
                interpretation="Same-Scene consumption requires a semantic visibility/lifetime redesign; missing observations never prove buffering safe.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path);parser.add_argument("summary", type=Path);parser.add_argument("output", type=Path)
    args = parser.parse_args()
    window = json.loads(args.summary.read_text(encoding="utf-8-sig"))
    with args.trace.open(encoding="utf-8-sig", newline="") as source:
        report = analyze(csv.DictReader(source), window["qpc_start_ms"], window["qpc_end_ms"])
    args.output.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
