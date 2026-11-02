"""Reads a -LeftHanded run's trace: did the weapons change wrists in the real game?

Run config/test_driver/left_handed_weapons.json under -LeftHanded -TestDriver -ScriptedHead -Trace
(docs/LEFT_HANDED.md), then:

  check_left_handed.py build/run/<run>/sm2vr_trace.jsonl

Each stretch of a loadout must show the drawn (final) weapon on the expected physical hand:
Gun_pose on the left with the primary, on the right with the pistol, and GUN_R, the sword socket, on
the left throughout. Trace positions are world units; units_per_metre converts them. The gun's
origin sits up to 0.5 m from the wrist on a long gun, so the test is which wrist is closer.

Exit 0 when every check passes, 1 otherwise, with one line per check.
"""
import json
import math
import sys


def distance(a, b):
    return math.dist(a, b) if a and b else None


def samples(path):
    with open(path, 'rb') as trace:
        for line in trace:
            if not line.startswith(b'{"t"') or b'"type":"sample"' not in line[:80].replace(b' ', b''):
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue  # The launcher stops the game mid-write, so a trace's last line can be cut short.
            character = record.get('character')
            hands = (record.get('input') or {})
            if character and character.get('driven') and hands:
                yield record['t'], character, hands


def reading(character, input_record):
    """One frame: the loadout and which wrist the gun and the sword are nearer, or None."""
    left, right = character.get('left_solved'), character.get('right_solved')
    bones = {w['name']: w.get('final') for w in character.get('weapons', [])}
    gun, sword = bones.get('Gun_pose'), bones.get('GUN_R')
    if not (left and right and gun and sword):
        return None
    side = lambda bone: 'left' if distance(bone, left) < distance(bone, right) else 'right'
    return {'loadout': input_record.get('loadout'), 'gun': side(gun), 'sword': side(sword),
            'marked': input_record.get('left_handed', False),
            'sword_metres': distance(sword, left) / (character.get('units_per_metre') or 1)}


def summarise(records):
    """{loadout: {'frames', 'gun_left', 'sword_left', 'sword_metres'}} over driven frames."""
    out = {}
    for frame in records:
        if frame is None or frame['loadout'] is None:
            continue
        row = out.setdefault(frame['loadout'], {'frames': 0, 'gun_left': 0, 'sword_left': 0, 'sword_metres': 0.0})
        row['frames'] += 1
        row['gun_left'] += frame['gun'] == 'left'
        row['sword_left'] += frame['sword'] == 'left'
        row['sword_metres'] = max(row['sword_metres'], frame['sword_metres'])
    return out


def verdicts(summary, marked):
    """The checks, as (passed, sentence)."""
    checks = [(marked, 'the trace is marked left_handed')]
    for loadout, gun_side in (('primary', 'left'), ('secondary', 'right')):
        row = summary.get(loadout)
        if not row or row['frames'] < 30:
            checks.append((False, f'{loadout}: too few frames ({row["frames"] if row else 0})'))
            continue
        on_left = row['gun_left'] / row['frames']
        share = on_left if gun_side == 'left' else 1 - on_left
        checks.append((share >= .95, f'{loadout}: the gun is on the {gun_side} hand on {share:.0%} of {row["frames"]} frames'))
        sword = row['sword_left'] / row['frames']
        checks.append((sword >= .95, f'{loadout}: GUN_R is on the left hand on {sword:.0%} of {row["frames"]} frames, at most {row["sword_metres"]:.2f} m away'))
    return checks


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    records, marked = [], False
    for _, character, input_record in samples(argv[1]):
        frame = reading(character, input_record)
        records.append(frame)
        marked = marked or bool(frame and frame['marked'])
    checks = verdicts(summarise(records), marked)
    for passed, sentence in checks:
        print('PASS' if passed else 'FAIL', sentence)
    return 0 if all(passed for passed, _ in checks) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
