"""Play sessions against a fixture game (a renamed cmd.exe): the mod is in the game folder only
while the game runs, crashes are repaired, foreign files are never touched, uninstall is exact."""
from pathlib import Path
import hashlib
import json
import shutil
import tempfile
import unittest

from installer_support import FIXTURES, REPO, TEMP, run_ps, run_ps_json
from test_installer_game import EXE

PHASE_LOG = '''\
[20:14:46.800] [I] [tid:26116] sm2vr loaded as dxgi proxy: pid=67604 exe='F:\\x.exe'
[20:15:04.430] [I] [tid:26116] Framework initialized
[20:15:04.433] [I] [tid:26116] GameBuild: exe timestamp 0x6ad10fcb (2026-10-21) known=true
[20:15:15.075] [I] [tid:58756] Sm2Probe: first present: swap chain 3840x2160 format=28
[20:15:18.346] [I] [tid:58756] VrSession: status=OpenXR runtime: SteamVR/OpenXR; recommended eye 2468x2740 (max 8192x8192); waiting for session readiness epoch=5
[20:19:59.261] [I] [tid:58756] VrSession: status=OpenXR tracking active; session state=3 epoch=5
'''


class LaunchTests(unittest.TestCase):
    def setUp(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(dir=TEMP))
        self.package, self.game, self.state, self.logs = (self.work / n for n in ('pkg', 'game', 'state', 'logs'))
        self.bin = self.game / 'client_pc/root/bin/pc'
        self.bin.mkdir(parents=True)
        shutil.copy(Path(shutil.which('cmd.exe')), self.bin / EXE)
        (self.package / 'mod/config').mkdir(parents=True)
        (self.package / 'mod/dxgi.dll').write_bytes(b'mod dll')
        (self.package / 'mod/openvr_api.dll').write_bytes(b'openvr')
        shutil.copy(REPO / 'installer/launch.json', self.package / 'mod/launch.json')
        (self.package / 'mod/config/index.calibration').write_text('head\n')
        (self.package / 'version.json').write_text(json.dumps({'mod_version': '0.1.0', 'preserve': []}))
        self.logs.mkdir()

    def ps(self, mode, command='', pretend=False, ok=True, view='motion'):
        args = [FIXTURES / 'launch.ps1', '-Mode', mode, '-PackageRoot', self.package, '-GameRoot', self.game,
                '-StateRoot', self.state, '-LogRoot', self.logs, '-GameCommand', command, '-View', view]
        if pretend:
            args.append('-Pretend')
        if ok:
            return run_ps_json(*args, timeout=120)
        result = run_ps(*args, timeout=120)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def install_json(self):
        return json.loads((self.state / 'install.json').read_text(encoding='utf-8'))

    def test_play_session(self):
        # The fake game records what it sees, writes mod log lines and a framework dropping.
        log = self.logs / 'sm2vr.log'
        command = (f'/c (if exist dxgi.dll echo dll>>probe.txt) & (if exist ..\\..\\mods\\SM2VR_FirstPerson_Test.pak echo pak>>probe.txt)'
                   f' & echo stereo=%SM2VR_STEREO% appid=%SteamAppId%>>probe.txt & echo cfg>vr_config.txt'
                   f' & echo [I] sm2vr loaded as dxgi proxy>>{log} & ping -n 2 127.0.0.1 >nul'
                   f' & echo [I] Framework initialized>>{log}')
        out = self.ps('play', command, pretend=True)
        probe = (self.bin / 'probe.txt').read_text().split()
        self.assertEqual(probe, ['dll', 'pak', 'stereo=1', 'appid=2183900'])
        self.assertEqual(out['seen'], ['loaded', 'hooked'])
        self.assertEqual(out['result']['ExitCode'], 0)
        self.assertEqual(sorted(p.name for p in self.bin.iterdir()), sorted([EXE, 'probe.txt']))
        self.assertEqual(list((self.game / 'client_pc/root/mods').iterdir()), [])
        run = Path(out['result']['RunDir'])
        self.assertTrue((run / 'vr_config.txt').is_file())
        self.assertIn('Framework initialized', (run / 'sm2vr.log').read_text())
        self.assertEqual((run / 'dump/shading.txt').read_text(), 'light_union_shadow_refresh')
        self.assertFalse((self.state / 'running.lock').exists())
        self.assertEqual(self.install_json()['game_files'], [])

    def test_chase_session_installs_the_chase_pack_without_motion_controls(self):
        pak = '..\\..\\mods\\SM2VR_FirstPerson_Test.pak'
        command = (f'/c (type {pak}>>probe.txt) & echo. >>probe.txt'
                   f' & echo chase=%SM2VR_CHASE_CAM% controllers=%SM2VR_CONTROLLERS% >>probe.txt')  # space: "1>>" is a redirect
        out = self.ps('play', command, pretend=True, view='chase')
        self.assertEqual((self.bin / 'probe.txt').read_text().split(), ['chase', 'chase=1', 'controllers=%SM2VR_CONTROLLERS%'])
        self.assertEqual(out['result']['ExitCode'], 0)
        self.assertEqual(list((self.game / 'client_pc/root/mods').iterdir()), [])
        self.assertEqual(self.install_json()['game_files'], [])
        # The motion-controls view still gets the first-person pack and its controllers.
        (self.bin / 'probe.txt').unlink()
        command = command.replace('chase=', 'x=')
        self.ps('play', command.replace('controllers=', 'c='), pretend=True)
        self.assertEqual((self.bin / 'probe.txt').read_text().split(), ['pack', 'x=%SM2VR_CHASE_CAM%', 'c=1'])

    def test_foreign_dxgi_is_refused_and_untouched(self):
        (self.bin / 'dxgi.dll').write_bytes(b'reshade')
        result = self.ps('play', '/c exit 0', pretend=True, ok=False)
        self.assertIn('Refusing to replace', result.stderr + result.stdout)
        self.assertEqual((self.bin / 'dxgi.dll').read_bytes(), b'reshade')
        self.assertEqual(self.install_json()['game_files'], [])
        self.assertFalse((self.game / 'client_pc/root/mods/SM2VR_FirstPerson_Test.pak').exists())

    def test_not_installed_is_refused(self):
        result = self.ps('play', '/c exit 0', ok=False)
        self.assertIn('Install first', result.stderr + result.stdout)

    def crash(self, changed=False):
        """State a killed session leaves: lock, recorded files still in the game folder."""
        self.ps('repair', pretend=True)
        dll = self.bin / 'dxgi.dll'
        dll.write_bytes(b'mod dll')
        recorded = {'path': str(dll), 'sha256': hashlib.sha256(b'mod dll').hexdigest()}
        if changed:
            dll.write_bytes(b'someone replaced it')
        state = self.install_json()
        state['game_files'] = [recorded]
        (self.state / 'install.json').write_text(json.dumps(state), encoding='utf-8')
        (self.state / 'running.lock').write_text('{}')
        (self.logs / 'sm2vr.log').write_text('crashed run log')
        return dll

    def test_crash_is_repaired(self):
        dll = self.crash()
        out = self.ps('repair')
        self.assertFalse(dll.exists())
        self.assertEqual(out['Warnings'], [])
        run = Path(out['RunDir'])
        self.assertTrue((run / 'running.lock').is_file())
        self.assertEqual((run / 'sm2vr.log').read_text(), 'crashed run log')
        self.assertEqual(self.install_json()['game_files'], [])
        self.assertIsNone(self.ps('repair'))

    def test_changed_file_is_left_and_reported(self):
        dll = self.crash(changed=True)
        out = self.ps('repair')
        self.assertTrue(dll.exists())
        self.assertIn('left in place', out['Warnings'][0])

    def test_uninstall_keeps_backups_and_runs(self):
        self.crash()
        (self.state / 'save-backups/1').mkdir(parents=True)
        self.assertEqual(self.ps('uninstall'), [])
        self.assertEqual(sorted(p.name for p in self.state.iterdir()), ['runs', 'save-backups'])
        self.assertEqual(sorted(p.name for p in self.bin.iterdir()), [EXE])

    def test_startup_phases_from_a_real_log(self):
        log = self.work / 'phase.log'
        log.write_text(PHASE_LOG)
        self.assertEqual(self.ps('phases', str(log)), ['loaded', 'hooked', 'window', 'headset_wait', 'vr'])
        log.write_text(PHASE_LOG.replace('known=true', 'known=false').splitlines()[2] + '\n')
        self.assertEqual(self.ps('phases', str(log)), ['unknown_build'])

    def test_startup_phases_after_a_swap_chain_reset(self):
        # 22 Oct: the game entering exclusive fullscreen resets the swap chain; the mod re-arms
        # stereo (armed=true) or gives up (armed=false / OrdinaryHeadsetStopped). The window says which.
        log = self.work / 'phase.log'
        rearmed = PHASE_LOG + ('[20:17:01.877] [I] [tid:58756] Sm2Probe: device reset: swap chain 3840x2160 format=28\n'
                               '[20:17:02.036] [I] [tid:58756] OrdinaryHeadsetRetention: ready=true stopped=true active=false pairs=0 refusal=8\n'
                               '[20:17:02.050] [I] [tid:58756] OrdinaryHeadsetRearm: armed=true attempts=1 rearms=1\n')
        log.write_text(rearmed)
        self.assertEqual(self.ps('phases', str(log)), ['loaded', 'hooked', 'window', 'headset_wait', 'vr', 'stereo_restarted'])
        log.write_text(PHASE_LOG + '[20:17:12.050] [E] [tid:58756] OrdinaryHeadsetRearm: armed=false reason=retention attempts=600 rearms=0\n')
        self.assertEqual(self.ps('phases', str(log)), ['loaded', 'hooked', 'window', 'headset_wait', 'vr', 'stereo_stopped'])
        # 26 Oct (GENERIC_GUARDS): a plain stop is followed by a cold restart of the stereo stack; only
        # the restart budget running out ends VR for the run.
        stopped = PHASE_LOG + '[20:17:12.050] [E] [tid:58756] OrdinaryHeadsetStopped: dispatch=5 packet_refusal=0 bloom_refusal=3\n'
        log.write_text(stopped)
        self.assertEqual(self.ps('phases', str(log)), ['loaded', 'hooked', 'window', 'headset_wait', 'vr'], 'a stop the cold restart will retry is not the end')
        restarted = stopped + '[20:17:15.100] [W] [tid:58756] OrdinaryHeadsetColdRestart: cold_restarts=1 in_window=1 budget=3\n'
        log.write_text(restarted)
        self.assertEqual(self.ps('phases', str(log)), ['loaded', 'hooked', 'window', 'headset_wait', 'vr', 'stereo_cold_restart'])
        log.write_text(restarted + '[20:19:00.000] [E] [tid:58756] OrdinaryHeadsetStopped: budget=exhausted cold_restarts=3 in_window=3 attempts=1\n')
        self.assertEqual(self.ps('phases', str(log)), ['loaded', 'hooked', 'window', 'headset_wait', 'vr', 'stereo_cold_restart', 'stereo_stopped'])
        log.write_text(PHASE_LOG + '[20:17:02.036] [I] [tid:58756] OrdinaryHeadsetRetention: ready=true stopped=true active=false pairs=0 refusal=8\n')
        self.assertEqual(self.ps('phases', str(log)), ['loaded', 'hooked', 'window', 'headset_wait', 'vr'],
                         'a transient stopped retention (re-arm pending) is not reported')
        # 31 Oct: DLSS off in the game's settings. Stereo holds, nothing stops, and the window says
        # which setting it is instead of "close the game and play again", which would not help.
        held = PHASE_LOG + '[19:00:36.000] [W] [tid:58756] OrdinaryDlssHold: held=true jobs=120 submitted=38383 (the game is running no DLSS)\n'
        log.write_text(held)
        self.assertEqual(self.ps('phases', str(log)), ['loaded', 'hooked', 'window', 'headset_wait', 'vr', 'stereo_needs_dlss'])
        log.write_text(held + '[19:02:00.000] [I] [tid:58756] OrdinaryDlssHold: held=false submitted=38383 (DLSS is back)\n')
        self.assertEqual(self.ps('phases', str(log)), ['loaded', 'hooked', 'window', 'headset_wait', 'vr', 'stereo_needs_dlss', 'stereo_dlss_back'])

    def test_play_loop_matches_only_new_lines(self):
        # 26 Oct release review: the play loop rescanned every line read so far on each 500 ms poll;
        # by 25 minutes (133k lines) one poll took 491 ms. Each poll must cost only its new lines,
        # and the log is no longer read once every phase has been reached.
        lines = PHASE_LOG.splitlines(keepends=True)
        filler = '[20:16:00.000] [I] [tid:58756] ArmLocalBone: index=12 parent=11\n'
        everything = (lines[3:] + [lines[2].replace('known=true', 'known=false'),
                                   '[20:17:02.050] [I] [tid:58756] OrdinaryHeadsetRearm: armed=true attempts=1 rearms=1\n',
                                   '[20:17:15.100] [W] [tid:58756] OrdinaryHeadsetColdRestart: cold_restarts=1 in_window=1 budget=3\n',
                                   '[20:19:00.000] [E] [tid:58756] OrdinaryHeadsetStopped: budget=exhausted cold_restarts=3\n',
                                   '[20:19:01.000] [W] [tid:58756] OrdinaryDlssHold: held=true jobs=120 submitted=0\n',
                                   '[20:19:02.000] [I] [tid:58756] OrdinaryDlssHold: held=false submitted=0\n'])
        batches = [lines[0], filler * 20000, lines[1], filler, ''.join(everything), filler * 10]
        folder = self.work / 'batches'
        folder.mkdir()
        for i, text in enumerate(batches):
            (folder / f'{i}.txt').write_text(text)
        polls = self.ps('watch', str(folder))
        self.assertEqual([p['phases'] for p in polls],
                         [['loaded'], [], ['hooked'], [], ['unknown_build', 'window', 'headset_wait', 'vr', 'stereo_restarted',
                                                            'stereo_cold_restart', 'stereo_stopped', 'stereo_needs_dlss', 'stereo_dlss_back'], []])
        self.assertEqual([p['examined'] for p in polls], [1, 20000, 1, 1, len(everything), 0],
                         'a poll after 20,001 lines matches only the one new line')
        self.assertEqual([p['done'] for p in polls], [False] * 4 + [True, True])
        self.assertEqual(polls[5]['offset'], polls[4]['offset'], 'the log is not read once every phase is reached')


if __name__ == '__main__':
    unittest.main()
