"""Replay the held-weapon reading (vr_weapon::read_grip and GripSwitch, mod/src/VrWeaponAim.h) over a
playtest trace and print when each rule holds the pistol grip (gun in the left hand).

Reads the 30 Hz trace's native GUN_L, GUN_R and Gun_pose, the published pad (D-pad down = a weapon
switch) and actor changes (respawns). Two rules: `old` (30 Oct master: a new actor keeps only a
primary; pistol to primary only on a `right` reading held 250 ms) and `new` (31 Oct: the draw rule,
a new actor holds the primary, and a primary landed off its socket waits to be seated). A 30 Hz
port, so timings are approximate.

Usage: replay_weapon_grip.py <sm2vr_trace.jsonl> [--readings] [--fit]
  --readings  also print one line per second of readings (L left, R right, T two_handed, . none;
              lower case or ',' while the right trigger is pulled) with the D-pad down presses.
  --fit       also print the held fit's error per stretch on GUN_R: on those frames the socket is
              the seat, so the drawn Gun_pose should sit on the drawn GUN_R. Error under about
              2 degrees is a seated gun; the 31 Oct 16:04 run reads 68 degrees and 27.5 cm.
"""
import json
import math
import statistics
import sys
from datetime import datetime, timedelta

SOCKET_M, SOCKET_DEG, TWO_M, TWO_DEG = 0.05, 45.0, 0.40, 45.0
HOLD, WINDOW, DRAW_START, DRAW_HOLD, PRESS_GAP = 0.25, 3.0, 0.5, 0.5, 0.1


def _sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def _length(a):
    return math.sqrt(sum(x * x for x in a))


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _rows(bone):
    x, z = bone['native_x_axis'], bone['native_z_axis']
    return [x, _cross(z, x), z]


def turn_degrees(a, b):
    trace = 0.0
    for u, v in zip(a, b):
        scale = _length(u) * _length(v)
        if not scale > 0:
            return 180.0
        trace += sum(u[i] * v[i] for i in range(3)) / scale
    return math.degrees(math.acos(max(-1.0, min(1.0, (trace - 1) / 2))))


def read_grip(gun, left, right, units_per_metre):
    """vr_weapon::read_grip on traced bones: 'left', 'right', 'two_handed' or None."""
    socket = SOCKET_M * units_per_metre
    if _length(_sub(gun['native'], right['native'])) <= socket and turn_degrees(_rows(gun), _rows(right)) <= SOCKET_DEG:
        return 'right'
    if _length(_sub(gun['native'], left['native'])) > socket:
        return None
    two = (_length(_sub(left['native'], right['native'])) <= TWO_M * units_per_metre and
           turn_degrees(_rows(left), _rows(right)) <= TWO_DEG)
    return 'two_handed' if two else 'left'


def primary(grip):
    return grip in ('right', 'two_handed')


class GripSwitch:
    """Port of vr_weapon::GripSwitch; new=False is the 30 Oct master rule."""

    def __init__(self, new):
        self.new = new
        self.current = self.candidate = None
        self.since = 0.0
        self.switch_at = self.request_at = self.request_from = self.off_since = self.seat_since = None
        self.spent = False
        self.seated = True
        self.draws = 0

    def switching(self, now):
        return self.switch_at is not None and not (self.new and self.spent) and 0 <= now - self.switch_at <= WINDOW

    def weapon_switch(self, now):
        if self.switch_at is None or now < self.switch_at or now - self.switch_at > PRESS_GAP:
            self.request_at, self.request_from, self.spent = now, self.current, False
        self.switch_at = now

    def settle(self, grip):
        if self.current and primary(self.current) != primary(grip):
            self.spent = True
        self.current, self.candidate, self.seated = grip, None, True

    def new_actor(self):
        if self.new:
            if self.current and not primary(self.current):
                self.current = 'right'
            self.spent, self.off_since, self.seat_since = True, None, None
            self.seated = self.current is None
        elif not self.current or not primary(self.current):
            self.current = None
        self.candidate = None

    def update(self, reading, now):
        held = self._settled(reading, now)
        if not reading or reading != held:
            self.seat_since = None
        elif self.seat_since is None:
            self.seat_since = now
        if self.seat_since is not None and now - self.seat_since >= HOLD:
            self.seated = True
        return held

    def _settled(self, reading, now):
        if self.new:
            if reading:
                self.off_since = None
            elif self.off_since is None:
                self.off_since = now
            if (not reading and self.switching(now) and self.current and not primary(self.current)
                    and self.request_from and not primary(self.request_from)):
                start = max(self.off_since, self.request_at)
                if start - self.request_at <= DRAW_START and now - start >= DRAW_HOLD:
                    self.settle('right')
                    self.seated = False
                    self.draws += 1
                    return self.current
        if reading and not primary(reading) and self.current and primary(self.current) and not self.switching(now):
            self.candidate, reading = None, None
        if not reading:
            return self.current
        if not self.current or reading == self.current:
            self.current, self.candidate = reading, None
            return self.current
        if self.candidate != reading:
            self.candidate, self.since = reading, now
        if now - self.since >= HOLD:
            self.settle(reading)
        return self.current


def samples(path):
    """(t, clock, actor, reading, buttons, right trigger) per traced sample with the three bones."""
    with open(path, encoding='utf-8', errors='replace') as f:
        wall = datetime.fromisoformat(json.loads(f.readline())['wall_clock_local'])
        for line in f:
            if '"type":"sample"' not in line or '"weapons"' not in line:
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            c = d.get('character') or {}
            bones = {w['name']: w for w in c.get('weapons') or [] if w.get('native') and w.get('native_x_axis')}
            if not all(k in bones for k in ('GUN_L', 'GUN_R', 'Gun_pose')):
                continue
            pad = (d.get('input') or {}).get('pad') or {}
            clock = (wall + timedelta(seconds=d['t'])).strftime('%H:%M:%S.%f')[:-3]
            yield (d['t'], clock, c.get('actor'), read_grip(bones['Gun_pose'], bones['GUN_L'], bones['GUN_R'], c.get('units_per_metre') or 1.0),
                   int(pad.get('buttons') or 0), float(pad.get('rt') or 0))


def fit_errors(path):
    """(clock, seconds, degrees, metres) per stretch of `right` readings held for HOLD: the median over
    its frames (the ends are GUN_R blending on and off) of how far the drawn Gun_pose is from the drawn
    GUN_R, the gun's true seat in the hand."""
    start, clock_at, frames = None, None, []
    with open(path, encoding='utf-8', errors='replace') as f:
        wall = datetime.fromisoformat(json.loads(f.readline())['wall_clock_local'])
        for line in f:
            if '"type":"sample"' not in line or '"weapons"' not in line:
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            c = d.get('character') or {}
            bones = {w['name']: w for w in c.get('weapons') or [] if w.get('native') and w.get('native_x_axis')}
            if not all(k in bones for k in ('GUN_L', 'GUN_R', 'Gun_pose')):
                continue
            gun, right = bones['Gun_pose'], bones['GUN_R']
            units = c.get('units_per_metre') or 1.0
            if read_grip(gun, bones['GUN_L'], right, units) == 'right' and gun.get('final_x_axis') and right.get('final_x_axis'):
                drawn = [gun['final_x_axis'], _cross(gun['final_z_axis'], gun['final_x_axis']), gun['final_z_axis']]
                socket = [right['final_x_axis'], _cross(right['final_z_axis'], right['final_x_axis']), right['final_z_axis']]
                if start is None:
                    start, clock_at = d['t'], (wall + timedelta(seconds=d['t'])).strftime('%H:%M:%S.%f')[:-3]
                frames.append((d['t'], turn_degrees(drawn, socket), _length(_sub(gun['final'], right['final'])) / units))
            elif start is not None:
                if frames[-1][0] - start >= HOLD:
                    yield (clock_at, frames[-1][0] - start, statistics.median(f[1] for f in frames),
                           statistics.median(f[2] for f in frames))
                start, frames = None, []


def main():
    rows = list(samples(sys.argv[1]))
    for new in (False, True):
        switch, actor, spans, waits = GripSwitch(new), None, [], []
        for t, clock, who, reading, buttons, _ in rows:
            if who != actor:
                if actor is not None:
                    switch.new_actor()
                actor = who
            if buttons & 0x0002:
                switch.weapon_switch(t)
            grip = switch.update(reading, t)
            if not spans or spans[-1][0] != grip:
                spans.append([grip, clock, clock])
            spans[-1][2] = clock
            if not switch.seated:
                if not waits or waits[-1][1] != previous:
                    waits.append([clock, clock])
                waits[-1][1] = clock
            previous = clock
        print('new rule (31 Oct)' if new else 'old rule (30 Oct master)', f'primary_draws={switch.draws}')
        for grip, start, end in spans:
            if grip != 'right' and grip != 'two_handed':
                print(f'  {grip or "none"}: {start} - {end}')
        for start, end in waits:
            print(f'  waiting for the socket: {start} - {end}')
    if '--fit' in sys.argv:
        print('fit error on GUN_R (start, seconds on the socket, degrees, metres)')
        for clock, seconds, degrees, metres in fit_errors(sys.argv[1]):
            print(f'  {clock} {seconds:5.2f} s {degrees:5.1f} deg {metres:.3f} m')
    if '--readings' in sys.argv:
        letters = {'left': 'L', 'right': 'R', 'two_handed': 'T', None: '.'}
        second, line, notes, previous = None, '', [], 0
        for _, clock, _, reading, buttons, rt in rows:
            if clock[:8] != second:
                if second:
                    print(second, line.ljust(34), ' '.join(notes))
                second, line, notes = clock[:8], '', []
            letter = letters[reading]
            line += (letter.lower() if letter != '.' else ',') if rt > 0 else letter
            if buttons & 0x0002 and not previous & 0x0002:
                notes.append(f'Ddown@{clock[9:]}')
            previous = buttons
        if second:
            print(second, line.ljust(34), ' '.join(notes))


if __name__ == '__main__':
    main()
