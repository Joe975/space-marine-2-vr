"""Summarise tracker-handle corruption across run folders (sm2vr.log per run).

The corruption: a stray 4-byte write of a small value at object+0xdc overwrites the high
dword of a tracker's last pointer field (+0xd8) in the first frames of the second ordinary
view. Unguarded it crashes at one of four native sites; NativeTrackerHandleGuard turns
those reads into the game's own invalid/no-geometry answers and logs a
TrackerHandleGuardTrip line instead. See docs/validation/tracker-handle-corruption-2026-10-20.md.

Per run: when stereo started (first consumed pair or first desktop second-view append),
whether the guard was installed for the executable and the server DLL, each trip (time
after stereo start, image, kind, corrupted value) and any crash with its native site.

  .venv/Scripts/python.exe -I scripts/check_tracker_guard.py RUN_DIR [RUN_DIR ...] [--json OUT]
"""
import argparse
import json
import re
from pathlib import Path

# Native sites where the corrupted +0xd8 field faults (image, RVA).
FAMILY = {
    ('Retail.exe', 0x134c30c): 'exe getter',
    ('Retail.dll', 0xd6c19c): 'dll getter',
    ('Retail.exe', 0x1946437): 'exe two-handle validity',
    ('Retail.dll', 0x136a157): 'dll two-handle validity',
}
TS = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]')
PAIR = re.compile(r'OrdinaryHeadset: consumed=([1-9]\d*)')
APPEND = re.compile(r'OrdinarySecondCamera: render_id=\d+ appended=true')
INSTALL = re.compile(r'TrackerHandleGuard: image=(exe|dll) installed=(true|false) refused=(true|false) enabled=(true|false)')
TRIP = re.compile(r'TrackerHandleGuardTrip: image=(exe|dll) kind=(\w+) thread=\d+ object=([0-9a-f]+) vtable=([0-9a-f]+) value=([0-9a-f]+)')
STATS = re.compile(r'TrackerHandleGuard: enabled=(true|false) ')


def seconds(line):
    m = TS.match(line)
    if not m:
        return None
    h, mi, s, ms = map(int, m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000


def short_module(path):
    return path.replace('/', '\\').split('\\')[-1].replace('Warhammer 40000 Space Marine 2 - ', '')


def analyse(lines):
    """Summarise one run's log lines (list of str)."""
    out = dict(stereo_start=None, guard={}, guard_enabled=None, trips=[], crash=None)
    crash_t = rip = None
    for line in lines:
        t = seconds(line)
        if out['stereo_start'] is None and (PAIR.search(line) or APPEND.search(line)):
            out['stereo_start'] = t
        m = INSTALL.search(line)
        if m:
            out['guard'][m.group(1)] = dict(installed=m.group(2) == 'true', refused=m.group(3) == 'true')
            out['guard_enabled'] = m.group(4) == 'true'
        m = STATS.search(line)
        if m:
            out['guard_enabled'] = m.group(1) == 'true'
        m = TRIP.search(line)
        if m:
            value = int(m.group(5), 16)
            out['trips'].append(dict(t=t, image=m.group(1), kind=m.group(2), object=m.group(3), vtable=m.group(4),
                                     value=m.group(5), high_dword=value >> 32, low_dword=value & 0xffffffff))
        if crash_t is None and 'Exception occurred:' in line:
            crash_t = t
            out['crash'] = dict(t=t, code=line.split('Exception occurred:')[1].strip(), site=None, family=None)
        elif crash_t is not None and rip is None and '] RIP:' in line:
            rip = int(line.split('RIP:')[1].strip(), 16)
        elif crash_t is not None and out['crash']['site'] is None and '] Module:' in line:
            parts = line.split('Module:')[1].strip().split(' ', 1)
            try:
                base = int(parts[0], 16)
            except ValueError:
                continue
            if rip is not None and rip >= base and len(parts) == 2:
                module = short_module(parts[1])
                out['crash']['site'] = '%s+%x' % (module, rip - base)
                out['crash']['family'] = FAMILY.get((module, rip - base))
    start = out['stereo_start']
    for trip in out['trips']:
        trip['after_stereo_s'] = round(trip['t'] - start, 3) if start is not None and trip['t'] is not None else None
    if out['crash'] and start is not None and out['crash']['t'] is not None:
        out['crash']['after_stereo_s'] = round(out['crash']['t'] - start, 3)
    out['verdict'] = verdict(out)
    return out


def verdict(run):
    if run['crash'] and run['crash']['family']:
        return 'crash_family'
    if run['crash']:
        return 'crash_other'
    if run['stereo_start'] is None:
        return 'no_stereo'
    if run['guard_enabled'] is False:
        return 'guard_off'
    if not run['guard'].get('exe', {}).get('installed') or not run['guard'].get('dll', {}).get('installed'):
        return 'guard_incomplete'
    return 'guarded_trip' if run['trips'] else 'clean'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('runs', nargs='+', type=Path)
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    rows = []
    for run in args.runs:
        log = run / 'sm2vr.log'
        if not log.exists():
            rows.append(dict(run=str(run), verdict='missing_log'))
            continue
        summary = analyse(log.read_text(encoding='utf-8', errors='replace').splitlines())
        summary['run'] = str(run)
        rows.append(summary)
        trips = ', '.join('%s %s value=%s +%ss' % (t['image'], t['kind'], t['value'], t['after_stereo_s']) for t in summary['trips'])
        crash = summary['crash']['site'] if summary['crash'] else '-'
        print('%-60s %-16s guard=%s trips=[%s] crash=%s' % (run.name, summary['verdict'],
              ','.join('%s:%s' % (k, 'on' if v['installed'] else 'refused') for k, v in sorted(summary['guard'].items())) or 'none', trips, crash))
    counts = {}
    for row in rows:
        counts[row['verdict']] = counts.get(row['verdict'], 0) + 1
    print('verdicts:', json.dumps(counts, sort_keys=True))
    if args.json:
        if args.json.exists():
            raise SystemExit('refusing to overwrite %s' % args.json)
        args.json.write_text(json.dumps(dict(runs=rows, verdicts=counts), indent=1) + '\n', encoding='utf-8')
    return 1 if counts.get('crash_family') or counts.get('guard_incomplete') else 0


if __name__ == '__main__':
    raise SystemExit(main())
