"""The window's steps for typical player situations: which step fails, what it says, which button."""
from pathlib import Path
import json
import tempfile
import unittest

from installer_support import FIXTURES, TEMP, run_ps_json
from test_installer_preflight import GOOD


class StepViewTests(unittest.TestCase):
    def view(self, changes=None, play_changes=None, problems='', bindings=True, playing=False):
        TEMP.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TEMP) as work:
            install, play = Path(work) / 'install.json', Path(work) / 'play.json'
            install.write_text(json.dumps({**GOOD, **(changes or {})}))
            play.write_text(json.dumps({**GOOD, **(changes or {}), **(play_changes or {})}))
            args = [FIXTURES / 'view.ps1', install, play, problems]
            if bindings:
                args.append('-BindingsCurrent')
            if playing:
                args.append('-Playing')
            steps = run_ps_json(*args)
        return [(s['Status'], s['Action']) for s in steps], steps

    def test_ready_to_play(self):
        summary, steps = self.view()
        self.assertEqual(summary, [('ok', 'change'), ('ok', None), ('ok', None), ('ok', 'reinstall'), ('ready', 'play')])
        self.assertIn('3-4 minutes', steps[4]['Detail'])
        self.assertEqual(steps[4]['Action2'], 'play_chase', 'both views are offered')
        self.assertIn('Chase camera', steps[4]['Detail'])
        self.assertNotIn('gamepad is connected', steps[4]['Detail'])
        self.assertEqual([s['Action2'] for s in steps[:4]], [None] * 4)

    def test_chase_camera_without_a_pad_still_offered_with_a_hint(self):
        summary, steps = self.view(play_changes={'GamepadConnected': False})
        self.assertEqual(summary[4], ('ready', 'play'))
        self.assertEqual(steps[4]['Action2'], 'play_chase')
        self.assertIn('No gamepad is connected', steps[4]['Detail'])

    def test_chase_camera_does_not_wait_for_the_steamvr_binding(self):
        summary, steps = self.view(bindings=False)
        self.assertEqual(summary[4], ('warn', 'bindings_done'))
        self.assertEqual(steps[4]['Action2'], 'play_chase')

    def test_blocked_play_offers_neither_view(self):
        for bindings in (True, False):
            summary, steps = self.view(play_changes={'Steam': 'not_running'}, bindings=bindings)
            self.assertEqual(summary[4], ('fail', 'recheck'))
            self.assertIsNone(steps[4]['Action2'])

    def test_first_run(self):
        summary, steps = self.view(problems='not installed', bindings=False)
        self.assertEqual(summary[3:], [('todo', 'install'), ('todo', None)])
        self.assertIn('backs up your saves', steps[3]['Detail'])

    def test_bindings_once_after_install(self):
        summary, steps = self.view(bindings=False)
        self.assertEqual(summary[4], ('warn', 'bindings_done'))
        self.assertIn('Manage Controller Bindings', steps[4]['Fix'])

    def test_game_missing(self):
        summary, steps = self.view({'GameFound': False}, problems='not installed')
        self.assertEqual(summary, [('fail', 'change'), ('todo', None), ('todo', None), ('todo', None), ('todo', None)])

    def test_game_updated(self):
        summary, steps = self.view({'ExeTimestamp': '0x6b000000'}, problems='the game updated since install')
        self.assertEqual(summary[2], ('fail', 'updates'))
        self.assertEqual(summary[3:], [('todo', None), ('todo', None)])
        self.assertIn('Wait for a mod update', steps[2]['Fix'])

    def test_wrong_openxr_runtime(self):
        summary, steps = self.view({'OpenXrRuntime': 'oculus_openxr_64.json'})
        self.assertEqual(summary[1], ('fail', 'recheck'))
        self.assertEqual(summary[3:], [('todo', None), ('todo', None)])
        self.assertIn('Set SteamVR as OpenXR runtime', steps[1]['Fix'])

    def test_folder_problems_show_on_install(self):
        self.assertEqual(self.view({'ModsWritable': False})[0][3], ('fail', 'fix_permissions'))
        summary, steps = self.view({'ProxyFiles': ['dxgi.dll'], 'ModMarkers': ['ReShade']})
        self.assertEqual(summary[3], ('fail', 'recheck'))
        self.assertIn('ReShade', steps[3]['Detail'])

    def test_play_blocked_by_steam_not_running(self):
        summary, steps = self.view(play_changes={'Steam': 'not_running'})
        self.assertEqual(summary[3:], [('ok', 'reinstall'), ('fail', 'recheck')])
        self.assertIn('Steam is not running', steps[4]['Detail'])

    def test_leftovers_mentioned(self):
        _, steps = self.view(play_changes={'LockPresent': True})
        self.assertEqual(steps[4]['Status'], 'ready')
        self.assertIn('did not end cleanly', steps[4]['Detail'])

    def test_playing(self):
        summary, _ = self.view(play_changes={'GameRunning': True}, playing=True)
        self.assertEqual(summary[3:], [('ok', None), ('running', None)])


if __name__ == '__main__':
    unittest.main()
