"""Package audit (no game data, no run output, ASCII scripts), the window's self-test on Windows
PowerShell 5.1 against a fixture package, and the supported-build list."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest

from installer_support import APP, FIXTURES, REPO, TEMP, run_ps, run_ps_json
from test_installer_game import EXE, fake_exe

REQUIRED = ('SM2VR.cmd', 'README.txt', 'app/Sm2VrApp.ps1', 'app/lib/owned-game-process.ps1', 'mod/dxgi.dll',
            'mod/openvr_api.dll', 'mod/launch.json', 'mod/config/vr_actions.json', 'mod/config/bindings_index.json',
            'builders/build_vr_controls.py', 'builders/build_chase_cam.py', 'python/python.exe', 'docs/CONTROLS.html',
            'THIRD_PARTY_NOTICES.txt', 'licenses/python.txt')
LICENSES = REPO / 'installer/licenses'
VERSION = {'mod_version': '0.1.0', 'game_exe_timestamp': '0x6ad10fcb', 'game_build_date': '2026-10-21',
           'manifest_url': 'https://example.invalid/manifest.json', 'preserve': ['index_guard.calibration']}


def make_fixture_package(root):
    for item in REQUIRED:
        (root / item).parent.mkdir(parents=True, exist_ok=True)
        (root / item).write_text('x')
    for script in APP.glob('*.ps1'):
        shutil.copy(script, root / 'app' / script.name)
    shutil.copy(REPO / 'scripts/owned-game-process.ps1', root / 'app/lib/owned-game-process.ps1')
    shutil.copy(REPO / 'installer/launch.json', root / 'mod/launch.json')
    shutil.copy(REPO / 'config/vr_actions.json', root / 'mod/config/vr_actions.json')
    for license in LICENSES.glob('*.txt'):
        shutil.copy(license, root / 'licenses' / license.name)
    (root / 'version.json').write_text(json.dumps(VERSION))


class PackageAuditTests(unittest.TestCase):
    def setUp(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=TEMP)) / 'SM2VR-0.1.0'
        make_fixture_package(self.root)

    def audit(self):
        return run_ps_json(FIXTURES / 'audit.ps1', self.root)

    def test_clean_package_passes(self):
        self.assertEqual(self.audit(), [])

    def test_game_data_and_run_output_are_refused(self):
        (self.root / 'mod/SM2VR_FirstPerson_Test.pak').write_bytes(b'pak')
        (self.root / 'builders/default_other.bin').write_bytes(b'saber')
        (self.root / 'mod/vrframework_crash.dmp').write_bytes(b'dump')
        problems = '\n'.join(self.audit())
        for name in ('SM2VR_FirstPerson_Test.pak', 'default_other.bin', 'vrframework_crash.dmp'):
            self.assertIn(name, problems)

    def test_missing_items_and_non_ascii_scripts(self):
        (self.root / 'docs/CONTROLS.html').unlink()
        (self.root / 'app/sm2-game.ps1').write_text('# café\n', encoding='utf-8')
        problems = self.audit()
        self.assertIn('missing docs\\CONTROLS.html', problems)
        self.assertIn('not ASCII: app\\sm2-game.ps1', problems)

    def test_third_party_licenses_ship(self):
        (self.root / 'licenses/openvr.txt').unlink()
        (self.root / 'THIRD_PARTY_NOTICES.txt').unlink()
        problems = self.audit()
        self.assertIn('missing license: openvr', problems)
        self.assertIn('missing THIRD_PARTY_NOTICES.txt', problems)

    def test_every_license_is_listed_and_indexed(self):
        # The audit's list, the committed texts and the notices index name the same files.
        TEMP.mkdir(parents=True, exist_ok=True)
        script = Path(tempfile.mkdtemp(dir=TEMP)) / 'licenses.ps1'
        script.write_text(". '{}'\nConvertTo-Json -InputObject @($Sm2ThirdPartyLicenses)".format(REPO / 'scripts/installer-package.ps1'))
        listed = set(json.loads(run_ps(script).stdout))
        self.assertEqual(listed, {p.stem for p in LICENSES.glob('*.txt')})
        index = (REPO / 'installer/THIRD_PARTY_NOTICES.txt').read_text(encoding='ascii')
        for name in listed | {'python'}:
            self.assertIn(name + '.txt', index)


class SourceScriptTests(unittest.TestCase):
    def test_installer_scripts_are_ascii(self):
        # Windows PowerShell 5.1 reads BOM-less scripts in the ANSI code page.
        scripts = list(APP.glob('*.ps1')) + [REPO / 'installer/SM2VR.cmd', REPO / 'scripts/installer-package.ps1',
                                             REPO / 'scripts/package-installer.ps1', REPO / 'scripts/owned-game-process.ps1']
        for script in scripts:
            with self.subTest(script.name):
                self.assertTrue(script.read_bytes().isascii())


class SourceBehaviourTests(unittest.TestCase):
    def test_the_launcher_alone_says_to_extract(self):
        # What double-clicking it inside the zip leaves: one file, no app folder.
        TEMP.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TEMP) as work:
            cmd = Path(work) / 'SM2VR.cmd'
            shutil.copy(REPO / 'installer/SM2VR.cmd', cmd)
            result = subprocess.run(['cmd', '/c', str(cmd)], input='', capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('Extract the whole zip', result.stdout)

    def test_settings_the_mod_saves_survive_an_update(self):
        script = (REPO / 'scripts/installer-package.ps1').read_text(encoding='utf-8')
        preserved = script[script.index('$Sm2PreservedConfig'):script.index(')', script.index('$Sm2PreservedConfig'))]
        for name in ('index_camera_guard.calibration', 'index_camera_modes.calibration'):
            self.assertIn(name, preserved)


class WindowSelfTest(unittest.TestCase):
    def test_self_test_repairs_nothing(self):
        # 1 Nov: run beside a launch, the self-test tried the after-crash repair and only the game's
        # process, seconds old, stopped it. A lock with no game running is what a repair acts on.
        TEMP.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(dir=TEMP))
        package = work / 'SM2VR-0.1.0'
        make_fixture_package(package)
        game = work / 'game'
        fake_exe(game / 'client_pc/root/bin/pc' / EXE, 0x6ad10fcb)
        (game / 'client_pc/root/mods').mkdir(parents=True)
        state = work / 'state'
        state.mkdir()
        lock = state / 'running.lock'
        lock.write_text('{}')
        env = {**os.environ, 'SM2VR_GAME_ROOT': str(game), 'SM2VR_PLAYER_STATE': str(state)}
        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
                                 str(package / 'app/Sm2VrApp.ps1'), '-SelfTest'],
                                capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(lock.exists(), 'the lock stays for the real window to act on')
        self.assertFalse((state / 'runs').exists(), 'no recovered run was made')

    def test_window_builds_and_fills_its_steps(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(dir=TEMP))
        package = work / 'SM2VR-0.1.0'
        make_fixture_package(package)
        game = work / 'game'
        fake_exe(game / 'client_pc/root/bin/pc' / EXE, 0x6ad10fcb)
        (game / 'client_pc/root/mods').mkdir(parents=True)
        env = {**os.environ, 'SM2VR_GAME_ROOT': str(game), 'SM2VR_PLAYER_STATE': str(work / 'state')}
        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
                                 str(package / 'app/Sm2VrApp.ps1'), '-SelfTest'],
                                capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        view = json.loads(result.stdout)
        steps = [(s['title'], s['status']) for s in view['steps']]
        self.assertEqual([t for t, _ in steps], ['Game found', 'Steam and SteamVR ready', 'Game version supported', 'Install', 'Play in VR'])
        self.assertEqual(steps[0][1], 'ok')
        self.assertEqual(steps[2][1], 'ok')
        self.assertEqual(view['main_hand'], 'Right', 'a new player is right-handed')
        # The Main hand box shows the saved choice, and a bad one falls back rather than stopping the app.
        (work / 'state').mkdir(exist_ok=True)
        for saved, shown in (('left', 'Left'), ('left_sticks', 'Left, walk on left stick'), ('nonsense', 'Right')):
            (work / 'state/settings.json').write_text(json.dumps({'game_root': None, 'channel': 'stable', 'main_hand': saved}), encoding='utf-8')
            again = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
                                    str(package / 'app/Sm2VrApp.ps1'), '-SelfTest'],
                                   capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, timeout=120)
            self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
            self.assertEqual(json.loads(again.stdout)['main_hand'], shown)
        self.assertIn('SM2VR 0.1.0 for game version 2026-10-21', view['log'])
        # Steam/SteamVR are this machine's real state, so only the install step's wording is fixed.
        self.assertIn(steps[3][1], ('todo', 'fail'))


class ReleaseGateTests(unittest.TestCase):
    def gate(self, doc, visibility):
        return run_ps_json(FIXTURES / 'release_gate.ps1', doc, visibility)

    def test_todays_distribution_doc_authorizes_publishing(self):
        self.assertEqual(self.gate(REPO / 'docs/DISTRIBUTION.md', 'PUBLIC'), [])
        problems = self.gate(REPO / 'docs/DISTRIBUTION.md', 'PRIVATE')
        self.assertEqual(len(problems), 1)
        self.assertIn('not PUBLIC', problems[0])

    def test_permission_and_public_repo_both_needed(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        doc = Path(tempfile.mkdtemp(dir=TEMP)) / 'DISTRIBUTION.md'
        doc.write_text('# Review\n\nRelease authorized: Joe Walker, 2027-01-05\n')
        self.assertEqual(self.gate(doc, 'PUBLIC'), [])
        self.assertEqual(len(self.gate(doc, 'PRIVATE')), 1)
        doc.write_text('Release authorized:\nnext line\n')
        self.assertEqual(len(self.gate(doc, 'PUBLIC')), 1)


class SupportedBuildTests(unittest.TestCase):
    def test_reference_and_ported_builds(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        script = Path(tempfile.mkdtemp(dir=TEMP)) / 'builds.ps1'
        script.write_text(". '{}'\nGet-Sm2SupportedBuilds '{}' | ConvertTo-Json".format(
            REPO / 'scripts/installer-package.ps1', REPO))
        builds = json.loads(run_ps(script).stdout)
        self.assertEqual(builds.get('0x6ac3e716'), '2026-10-05')
        self.assertEqual(builds.get('0x6ad10fcb'), '2026-10-21')


class ShippedCalibrationTests(unittest.TestCase):
    def test_package_ships_only_the_calibrations_the_mod_loads(self):
        # 26 Oct release tidy: every config/*.calibration shipped, including five hand-calibration
        # candidates and baselines the player profile never loads.
        import re
        script = (REPO / 'scripts/installer-package.ps1').read_text(encoding='utf-8')
        block = re.search(r'^\$Sm2ShippedCalibrations = (.*?)\n\n', script, re.S | re.M)
        self.assertIsNotNone(block, 'explicit list in installer-package.ps1')
        shipped = re.findall(r"'([\w.]+\.calibration)'", block[1])
        self.assertNotIn("Filter '*.calibration'", script)
        config = {p.name for p in (REPO / 'config').glob('*.calibration')}
        # Named by the profile (the player's own guard/grenade files are made by the F11 overlay).
        profile = set(re.findall(r"'(\w+\.calibration)'", (APP / 'sm2-profile.ps1').read_text(encoding='utf-8'))) & config
        # Read by the DLL beside index.calibration. index_hands.calibration is only the fallback
        # when SM2VR_HANDS_CALIBRATION is unset, and the profile always sets it with SM2VR_HANDS.
        siblings = set(re.findall(r'parent_path\(\)/"(\w+\.calibration)"', (REPO / 'mod/src/Sm2Probe.cpp').read_text(encoding='utf-8')))
        self.assertIn('index_hands.calibration', siblings, 'fixture: the fallback is still there')
        self.assertEqual(sorted(shipped), sorted(profile | (siblings - {'index_hands.calibration'})))
        self.assertTrue(set(shipped) <= config)
        for unused in ('index_hands.calibration', 'index_hands_axis_candidate.calibration', 'index_hands_outward_45_candidate.calibration',
                       'index_hands_raw_baseline.calibration', 'index_walking_baseline.calibration'):
            self.assertNotIn(unused, shipped)


if __name__ == '__main__':
    unittest.main()
