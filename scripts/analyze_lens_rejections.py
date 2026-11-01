"""Audit complete offline lens event retention; failures remain failures after recovery."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


def fields(line):
    return dict(re.findall(r'(\w+)=([^\s]+)', line))


def analyze(text, expected_updates=4096, require_accounting=False):
    events, state, experiment, totals, errors = [], None, None, {}, []
    skips, accounting = [], None
    for line in text.splitlines():
        if 'PipLensRejectionOverflow:' in line:
            errors.append('rejection event ring overflowed')
        elif 'PipLensSkipOverflow:' in line:
            errors.append('PIP skip event ring overflowed')
        elif 'PipLensSkip:' in line:
            skips.append(fields(line.split('PipLensSkip:', 1)[1]))
        elif 'PipLensAccounting:' in line:
            accounting = fields(line)
        elif 'PipLensRejection:' in line:
            events.append(fields(line.split('PipLensRejection:', 1)[1]))
        elif 'PipLensRejectionCount:' in line:
            value = fields(line)
            totals[value.get('reason')] = value.get('count')
        elif 'PipFinalLensState:' in line:
            state = fields(line)
        elif 'PipExperiment:' in line:
            experiment = fields(line)
    try:
        if state is None or experiment is None:
            raise ValueError('missing final state')
        rejected, applied = int(state['rejected']), int(state['applied'])
        serials = [int(e['serial']) for e in events]
        reasons = Counter(int(e['reason']) for e in events)
        if len(serials) != rejected or any(value != index for index, value in enumerate(serials, 1)):
            errors.append('events do not exactly cover every cumulative refusal')
        if any(reason not in range(2, 13) for reason in reasons):
            errors.append('unknown refusal reason')
        if {int(k): int(v) for k, v in totals.items()} != dict(reasons):
            errors.append('reason counters disagree with retained events')
        if experiment['finished'] != 'true' or experiment['lost'] != 'false' or int(experiment['updates']) != expected_updates:
            errors.append('offline experiment incomplete or ownership lost')
        if applied < 0 or rejected < 0:
            errors.append('negative cumulative count')
        # This bounded fixture starts rendering on the second accepted update.
        if applied + rejected != expected_updates - 1:
            errors.append('lens attempts do not cover the bounded fixture')
    except (KeyError, TypeError, ValueError):
        errors.append('missing or malformed lens/experiment evidence')
        applied, rejected, reasons = None, None, Counter()
    skipped, unexpected = 0, 0
    if require_accounting or accounting is not None or skips:
        try:
            skipped = int(accounting['skipped'])
            unexpected = int(accounting['unexpected_errors'])
            counts = {i: int(accounting[k]) for i, k in enumerate(('no_target', 'provider_error', 'foreign_record'), 1)}
            actual = Counter(int(e['reason']) for e in skips)
            if (skipped < 0 or unexpected < 0 or len(skips) != skipped
                    or any(int(e['serial']) != index or e['active_ownership_verified'] != 'false' for index, e in enumerate(skips, 1))
                    or any(count < 0 for count in counts.values()) or sum(counts.values()) != skipped
                    or any(reason not in counts for reason in actual) or any(actual[k] != v for k, v in counts.items())):
                errors.append('PIP skip events do not reconcile with candidate accounting')
        except (KeyError, TypeError, ValueError):
            errors.append('missing or malformed PIP candidate accounting')
    return dict(evidence_complete=not errors, zero_refusals_pass=not errors and rejected == 0 and skipped == 0 and unexpected == 0,
                applied=applied, rejected=rejected, reason_counts=dict(sorted(reasons.items())),
                events=events, skipped_candidates=skipped, skip_events=skips, unexpected_errors=unexpected,
                accounting_required=require_accounting, final_accounting=accounting,
                final_state=state, final_experiment=experiment, errors=errors)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-accounting', action='store_true', help='Require the new PIP candidate skip/exception receipt')
    args = parser.parse_args()
    result = analyze(args.log.read_text(errors='replace'), require_accounting=args.require_accounting)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['zero_refusals_pass'] else 1)
