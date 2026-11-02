"""Install orchestration against a fixture package whose builders are stand-ins: config preserve
rules, pack chaining, all-or-nothing pack replacement, save backup scope, staleness detection."""
from pathlib import Path
import json
import re
import shutil
import sys
import tempfile
import textwrap
import unittest

from installer_support import APP, FIXTURES, REPO, TEMP, run_ps, run_ps_json
from test_installer_game import EXE, fake_exe

LAYOUT = int(re.search(r'(?m)^INPUT_LAYOUT = (\d+)', (REPO / 'scripts/build_vr_controls.py').read_text(encoding='utf-8')).group(1))

BUILDERS = ('build_first_person.py', 'build_vr_diagnostics.py', 'build_vr_menus.py', 'build_vr_controls.py')
# Built from the menus step, beside the controls pack.
CHASE_BUILDER = 'build_chase_cam.py'
# Writes --out as "<previous pack>\n<script name> <inputs>", so the test sees the chain.
STAND_IN = textwrap.dedent('''\
    import sys
    from pathlib import Path
    args = sys.argv[1:]
    named = dict(zip(args[::2], args[1::2])) if '--camera-only' not in args else dict(zip(args[1::2], args[2::2]))
    for key in ('--source', '--presets-pack', '--preset'):
        if key in named and not Path(named[key]).is_file():
            sys.exit(f'missing input {named[key]}')
    base = named.get('--camera-pack') or named.get('--base-pack') or named.get('--base')
    previous = Path(base).read_text() if base else ''
    print('warning on stderr', file=sys.stderr)
    Path(named['--out']).write_text(previous + Path(__file__).stem + '\\n')
    if Path(__file__).stem == 'build_vr_controls':
        Path(named['--out']).with_suffix('.json').write_text('{"input_layout": LAYOUT}')
    ''').replace('LAYOUT', str(LAYOUT))


SWING_V1 = '# swing\nmin_speed=1.0\nmax_angle=40\n'


def make_package(root, failing=None, version='0.1.0', swing=SWING_V1):
    (root / 'builders/config').mkdir(parents=True)
    for name in BUILDERS + (CHASE_BUILDER,):
        body = 'import sys\nsys.exit("builder broke")\n' if name == failing else STAND_IN
        (root / 'builders' / name).write_text(body)
    (root / 'builders/inspect_grenade_equipment_layout.py').write_text('print("{}")\n')
    for preset in ('first_person_wide_fov.json', 'vr_menus.json'):
        (root / 'builders/config' / preset).write_text('{}')
    (root / 'mod/config').mkdir(parents=True)
    (root / 'mod/config/index_swing.calibration').write_text(swing)
    (root / 'mod/config/index.calibration').write_text('units_per_metre=0.9\n')
    (root / 'version.json').write_text(json.dumps({
        'mod_version': version, 'game_exe_timestamp': '0x6ad10fcb', 'game_build_date': '2026-10-21',
        'preserve': ['index_swing.calibration']}))


def make_game(root):
    fake_exe(root / 'client_pc/root/bin/pc' / EXE, 0x6ad10fcb)
    paks = root / 'client_pc/root/paks/client/default'
    paks.mkdir(parents=True)
    for pak in ('default_other.pak', 'default_presets.pak', 'default_ssl.pak'):
        (paks / pak).write_bytes(b'game data')
    (root / 'client_pc/root/mods').mkdir()


def make_saber(root):
    (root / 'storage/steam/user/1/Main/config').mkdir(parents=True)
    (root / 'storage/steam/user/1/Main/config/progress.cfg').write_bytes(b'SSF1 save')
    (root / 'server').mkdir()
    (root / 'server/x.bin').write_bytes(b's')
    (root / 'client').mkdir()
    (root / 'client/pso.cache').write_bytes(b'big cache')


class InstallTests(unittest.TestCase):
    def setUp(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(dir=TEMP))
        self.package, self.game, self.state, self.saber = (self.work / n for n in ('pkg', 'game', 'state', 'saber'))
        make_game(self.game)
        make_saber(self.saber)

    def install(self, ok=True):
        # The test's own interpreter stands in for the bundled python/python.exe.
        args = (FIXTURES / 'install.ps1', self.package, self.game, self.state, self.saber, sys.executable)
        if ok:
            return run_ps_json(*args, timeout=120)
        result = run_ps(*args, timeout=120)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_fresh_install(self):
        make_package(self.package)
        result = self.install()
        state = result['state']
        self.assertEqual(result['problems'], [])
        pak = Path(state['pack']['path'])
        self.assertEqual(pak.read_text().split(), [b[:-3] for b in BUILDERS])
        self.assertEqual(pak.parent, self.state / 'pack')
        chase = Path(state['pack']['chase']['path'])
        self.assertEqual(chase.read_text().split(), [b[:-3] for b in BUILDERS[:3] + (CHASE_BUILDER,)],
                         'the chase pack is the menus pack minus its cameras, without the motion controls')
        self.assertEqual(chase.parent, self.state / 'pack/chase')
        self.assertEqual(chase.name, pak.name)
        self.assertEqual(state['exe_timestamp'], '0x6ad10fcb')
        self.assertTrue(state['grenade_layout_verified'])
        self.assertEqual(state['game_files'], [])
        backup = Path(state['last_save_backup'])
        self.assertTrue((backup / 'storage/steam/user/1/Main/config/progress.cfg').is_file())
        self.assertTrue((backup / 'server/x.bin').is_file())
        self.assertFalse((backup / 'client').exists())
        self.assertEqual((self.state / 'config/index_swing.calibration').read_text(), SWING_V1)
        self.assertEqual([p.name for p in self.state.iterdir() if p.name.startswith('pack-build')], [])
        self.assertEqual(sorted(p.name for p in (self.game / 'client_pc/root/bin/pc').iterdir()), [EXE])

    def test_reinstall_keeps_preserved_tuning_and_replaces_the_rest(self):
        make_package(self.package)
        self.install()
        # The DLL rewrites a file when an F11 slider saves it: no comments, its own key order.
        (self.state / 'config/index_swing.calibration').write_text('max_angle=40\nmin_speed=1.6\n')
        (self.state / 'config/index.calibration').write_text('units_per_metre=1.2\n')
        state = self.install()['state']
        self.assertEqual((self.state / 'config/index_swing.calibration').read_text(), '# swing\nmin_speed=1.6\nmax_angle=40\n')
        self.assertEqual((self.state / 'config/index.calibration').read_text(), 'units_per_metre=0.9\n')
        self.assertEqual(state['preserved_config'], ['index_swing.calibration: min_speed'])

    def test_update_merges_new_keys_and_new_defaults_under_the_players_changes(self):
        """A release that adds a key (as body scale added reference_eye_height_metres) and changes a
        default the player never touched must reach a player who tuned something else."""
        make_package(self.package)
        self.install()
        (self.state / 'config/index_swing.calibration').write_text('min_speed=1.6\nmax_angle=40\nold_key=3\n')
        shutil.rmtree(self.package)
        make_package(self.package, version='0.2.0', swing='# swing v2\nmin_speed=1.2\nmax_angle=35\nreference=1.644\n')
        state = self.install()['state']
        self.assertEqual((self.state / 'config/index_swing.calibration').read_text(),
                         '# swing v2\nmin_speed=1.6\nmax_angle=35\nreference=1.644\n')
        self.assertEqual(state['preserved_config'], ['index_swing.calibration: min_speed'])

    def test_install_over_a_config_with_no_shipped_base_keeps_every_player_value(self):
        make_package(self.package)
        (self.state / 'config').mkdir(parents=True)
        (self.state / 'config/index_swing.calibration').write_text('max_angle=50\n')
        self.install()
        self.assertEqual((self.state / 'config/index_swing.calibration').read_text(), '# swing\nmin_speed=1.0\nmax_angle=50\n')

    def test_failed_builder_keeps_the_previous_pack(self):
        make_package(self.package)
        good = self.install()['state']['pack']
        shutil.rmtree(self.package)
        make_package(self.package, failing='build_vr_menus.py')
        result = self.install(ok=False)
        self.assertIn('builder broke', result.stderr + result.stdout)
        self.assertEqual(json.loads((self.state / 'install.json').read_text(encoding='utf-8'))['pack'], good)
        self.assertTrue(Path(good['path']).is_file())
        self.assertEqual([p.name for p in self.state.iterdir() if p.name.startswith('pack-build')], [])

    def test_failed_chase_builder_keeps_both_previous_packs(self):
        make_package(self.package)
        good = self.install()['state']['pack']
        shutil.rmtree(self.package)
        make_package(self.package, failing=CHASE_BUILDER)
        self.install(ok=False)
        self.assertEqual(json.loads((self.state / 'install.json').read_text(encoding='utf-8'))['pack'], good)
        self.assertEqual(Path(good['path']).read_text().split(), [b[:-3] for b in BUILDERS])
        self.assertEqual([p.name for p in self.state.iterdir() if p.name.startswith('pack-build')], [])

    def test_missing_game_pak_fails_the_install(self):
        make_package(self.package)
        (self.game / 'client_pc/root/paks/client/default/default_presets.pak').unlink()
        result = self.install(ok=False)
        self.assertIn('missing input', result.stderr + result.stdout)
        self.assertFalse((self.state / 'install.json').exists())

    def test_pack_layout_check_expects_the_builders_layout(self):
        check = re.search(r'\$expected = (\d+)', (APP / 'sm2-install.ps1').read_text(encoding='utf-8'))
        self.assertEqual(int(check.group(1)), LAYOUT, 'raise both together: the DLL and the pack must agree')

    def test_staleness(self):
        make_package(self.package)
        self.install()
        fixture = FIXTURES / 'install_problems.ps1'
        problems = lambda: run_ps_json(fixture, self.package, self.game, self.state)  # noqa: E731
        self.assertEqual(problems(), [])
        fake_exe(self.game / 'client_pc/root/bin/pc' / EXE, 0x6b000000)
        self.assertEqual(problems(), ['the game updated since install'])
        fake_exe(self.game / 'client_pc/root/bin/pc' / EXE, 0x6ad10fcb)
        chase = self.state / 'pack/chase/SM2VR_FirstPerson_Test.pak'
        original = chase.read_text()
        chase.write_text('edited')
        self.assertEqual(problems(), ['the chase camera pack is missing or changed'])
        chase.write_text(original)
        self.assertEqual(problems(), [])
        # An install from before the chase camera has no chase pack recorded: reinstall.
        state_file = self.state / 'install.json'
        current = state_file.read_text(encoding='utf-8')
        old = json.loads(current)
        del old['pack']['chase']
        state_file.write_text(json.dumps(old), encoding='utf-8')
        self.assertEqual(problems(), ['the chase camera pack is missing or changed'])
        state_file.write_text(current, encoding='utf-8')
        # A DLL replaced at the same mod version, over a pack with the old button layout.
        report = self.state / 'pack/SM2VR_FirstPerson_Test.json'
        original = report.read_text()
        # Layout 2 is 1.0.0's first pack: same version, same buttons, the old Edit Squad prompt.
        for stale in ('{"input_layout": 1}', '{"input_layout": 2}', '{}', 'not json'):
            report.write_text(stale)
            self.assertEqual(problems(), ['the data pack was built for an older version of the mod'], stale)
        report.unlink()
        self.assertEqual(problems(), ['the data pack was built for an older version of the mod'])
        report.write_text(original)
        self.assertEqual(problems(), [])
        pak = self.state / 'pack/SM2VR_FirstPerson_Test.pak'
        pak.write_text('edited')
        self.assertEqual(problems(), ['the data pack is missing or changed'])
        version = json.loads((self.package / 'version.json').read_text())
        (self.package / 'version.json').write_text(json.dumps({**version, 'mod_version': '0.2.0'}))
        self.assertEqual(len(problems()), 2)


if __name__ == '__main__':
    unittest.main()
