"""Preflight verdicts for faked machine states: each problem a player can hit gives one failing
check with a fix, and only real blockers stop the install or launch."""
from pathlib import Path
import json
import tempfile
import unittest

from installer_support import FIXTURES, TEMP, run_ps_json

GOOD = {
    'GameFound': True, 'GameRoot': 'F:\\Steam\\steamapps\\common\\Space Marine 2',
    'ExeTimestamp': '0x6ad10fcb', 'SupportedTimestamp': '0x6ad10fcb', 'SupportedBuildDate': '2026-10-21',
    'GameRunning': False, 'Steam': 'running', 'SteamVr': 'not_running', 'InstallerElevated': False,
    'OpenXrRuntime': 'H:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win64.json',
    'ProxyFiles': [], 'ModMarkers': [], 'OwnedLeftovers': [],
    'BinWritable': True, 'ModsWritable': True, 'ModsMissing': False, 'LockPresent': False,
    'GamepadConnected': True,
}


class PreflightTests(unittest.TestCase):
    def run_checks(self, changes, for_='install'):
        TEMP.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TEMP) as work:
            state = Path(work) / 'state.json'
            state.write_text(json.dumps({**GOOD, **changes}), encoding='utf-8')
            result = run_ps_json(FIXTURES / 'preflight.ps1', state, for_)
        failed = {c['Id']: c for c in result['checks'] if not c['Ok']}
        return result['passed'], failed

    def test_healthy_machine_passes_both(self):
        for for_ in ('install', 'play'):
            passed, failed = self.run_checks({}, for_)
            self.assertTrue(passed, failed)
            self.assertEqual(failed, {})

    def test_each_blocker(self):
        cases = {
            'version': {'ExeTimestamp': '0x6b000000'},
            'elevation': {'InstallerElevated': True},
            'steam': {'Steam': 'elevated'},
            'steamvr': {'SteamVr': 'elevated'},
            'openxr': {'OpenXrRuntime': 'C:\\Program Files\\Oculus\\Support\\oculus-runtime\\oculus_openxr_64.json'},
            'other_mods': {'ProxyFiles': ['dxgi.dll'], 'ModMarkers': ['ReShade']},
            'writable': {'ModsWritable': False},
        }
        for expected, changes in cases.items():
            with self.subTest(expected):
                passed, failed = self.run_checks(changes)
                self.assertFalse(passed)
                self.assertEqual(list(failed), [expected])
                self.assertTrue(failed[expected]['Fix'])

    def test_other_mod_is_named(self):
        _, failed = self.run_checks({'ProxyFiles': ['dxgi.dll', 'd3d11.dll'], 'ModMarkers': ['Special K']})
        self.assertIn('dxgi.dll, d3d11.dll', failed['other_mods']['Message'])
        self.assertIn('Special K', failed['other_mods']['Message'])

    def test_no_openxr_runtime_registered(self):
        _, failed = self.run_checks({'OpenXrRuntime': None})
        self.assertIn('none', failed['openxr']['Message'])

    def test_missing_game_stops_all_other_checks(self):
        passed, failed = self.run_checks({'GameFound': False})
        self.assertFalse(passed)
        self.assertEqual(list(failed), ['game'])

    def test_play_only_checks(self):
        for changes, expected in (({'Steam': 'not_running'}, 'steam'), ({'GameRunning': True}, 'running')):
            with self.subTest(expected):
                self.assertTrue(self.run_checks(changes, 'install')[0])
                passed, failed = self.run_checks(changes, 'play')
                self.assertFalse(passed)
                self.assertIn(expected, failed)

    def test_leftovers_are_reported_but_do_not_block(self):
        for changes in ({'OwnedLeftovers': ['dxgi.dll']}, {'LockPresent': True}):
            with self.subTest(changes):
                passed, failed = self.run_checks(changes, 'play')
                self.assertTrue(passed)
                self.assertIn('leftovers', failed)

    def test_missing_gamepad_is_mentioned_for_play_only_and_never_blocks(self):
        # Motion controls need no pad, and pads are often switched on late.
        passed, failed = self.run_checks({'GamepadConnected': False}, 'play')
        self.assertTrue(passed)
        self.assertIn('gamepad', failed['gamepad']['Fix'])
        self.assertEqual(self.run_checks({'GamepadConnected': False}, 'install')[1], {})


if __name__ == '__main__':
    unittest.main()
