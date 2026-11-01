"""What a playtest runs and what a player gets must not drift apart (docs/FLAGS.md).

Three headset playtests ran without the game HUD because the installer always set SM2VR_NATIVE_HUD
and the dev launcher only set it with an opt-in switch. These checks keep the launcher, the
installer, the DLL and docs/FLAGS.md in step. All but the last two are static and need no PowerShell.
"""
from pathlib import Path
import json
import re
import shutil
import tempfile
import unittest

from installer_support import REPO, TEMP, run_ps

LAUNCHER = REPO / 'scripts' / 'run-ordinary-headset.ps1'
FLAGS = REPO / 'docs' / 'FLAGS.md'
DEVELOPING = REPO / 'docs' / 'DEVELOPING.md'
LAUNCH_JSON = REPO / 'installer' / 'launch.json'
CLASSES = ('shipped', 'opt-out', 'diagnostic', 'dev tool', 'dead')
# docs/FLAGS.md is development-only: the public export (scripts/export_public.py, itself left out of
# it) drops it, so its checks run only in the development repo.
DEV_REPO = (REPO / 'scripts' / 'export_public.py').exists()
needs_flags_doc = unittest.skipUnless(DEV_REPO, 'docs/FLAGS.md is left out of the public export')

# String literals in mod/src that look like variables but are not read from the environment.
NOT_VARIABLES = {
    'SM2VR_TEST': 'test-driver file magic (VrTestDriver.h)',
    'SM2VR_PIP': 'prefix match for the performance-camera conflict check (NativeOrdinaryCamera.h)',
    'SM2VR_ORDINARY_': 'prefix match (NativeOrdinaryCamera.h)',
    'SM2VR_ORDINARY_RIGHT_BLOOM_': 'texture name prefix (NativeOrdinaryTextureCreation.h)',
    'SM2VR_ORDINARY_RIGHT_TAA_': 'texture name prefix (NativeOrdinaryTextureCreation.cpp)',
}
# Variables the launch scripts set or read that the DLL never reads, and why.
SCRIPT_ONLY = {
    'SM2VR_GAME_ROOT': 'game folder override for the scripts and installer (installer/app/sm2-game.ps1)',
    'SM2VR_PLAYER_STATE': 'installer state folder (installer/app/sm2-install.ps1)',
}
# Removed 27 October: no-op opt-ins whose features had become defaults. The launcher refuses them.
REMOVED_SWITCHES = ('ChainswordRevCandidate', 'CompactHudCandidate', 'GazeCameraCandidate', 'GrenadeAimCandidate',
                    'UprightTorso', 'Haptics', 'RefreshShadows')
# Dated history records keep the switches they were run with (same rule as scripts/export_public.py).
HISTORY_DOC = re.compile(r'^docs/(PLAYTEST_[^/]*|[^/]*_20\d\d-\d\d-\d\d[^/]*)\.md$')
CURRENT_ROOTS = ('scripts', 'tests', 'installer', 'config', 'docs', 'release-docs', '.claude')
CURRENT_FILES = ('README.md', 'CONTRIBUTING.md', 'STATUS.md')
TEXT_SUFFIXES = {'.md', '.ps1', '.py', '.json', '.cmd', '.bat', '.txt', '.html', '.calibration', '.sh', '.inc', '.h', '.cpp'}
VARIABLE = re.compile(r'SM2VR_[A-Z0-9_]*[A-Z0-9](?![A-Za-z0-9_])')


def launcher_switches():
    """Parameter names of the launcher's param() block, in order."""
    text = LAUNCHER.read_text(encoding='utf-8')
    start = text.index('param(')
    depth = 0
    for end in range(start + len('param'), len(text)):
        depth += {'(': 1, ')': -1}.get(text[end], 0)
        if depth == 0:
            break
    return re.findall(r'\]\$(\w+)', text[start:end + 1])


def launcher_classes():
    """The launcher's own $switchClass table: switch -> class."""
    text = LAUNCHER.read_text(encoding='utf-8')
    found = {}
    for cls, names in re.findall(r"^\s*'(shipped|opt-out|diagnostic|dev tool|dead)' = @\(([^)]*)\)", text, re.M):
        for name in re.findall(r"'(\w+)'", names):
            found.setdefault(name, []).append(cls)
    return found


def table_rows(path, first_cell):
    """Rows of the Markdown tables in path whose first cell matches first_cell: name -> cells."""
    rows = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if line.startswith('|') and (m := re.fullmatch(first_cell, cells[0])):
            rows.setdefault(m.group(1), []).append(cells)
    return rows


def flags_switches():
    return table_rows(FLAGS, r'`-(\w+)`')


def flags_variables():
    return table_rows(FLAGS, r'`(SM2VR_[A-Z0-9_]+)`')


def dll_variables():
    """Every SM2VR_* string literal in mod/src code (not comments): variable -> reading files."""
    found = {}
    for path in sorted((REPO / 'mod' / 'src').rglob('*')):
        if not path.is_file() or path.suffix not in {'.h', '.cpp', '.inc'}:
            continue
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            code = line.split('//', 1)[0]
            for name in re.findall(r'L?"(SM2VR_[A-Z0-9_]+)"', code):
                if name not in NOT_VARIABLES:
                    found.setdefault(name, set()).add(path.name)
    return found


def scripts_variables():
    """SM2VR_* names the launch scripts and the installer set or read, with the ORDINARY lists expanded."""
    files = [LAUNCHER, REPO / 'scripts' / 'run-modded.ps1', REPO / 'scripts' / 'owned-game-process.ps1',
             *sorted((REPO / 'installer' / 'app').glob('*.ps1'))]
    names = set()
    for path in files:
        text = path.read_text(encoding='utf-8')
        names |= set(VARIABLE.findall(text))
        for listed in re.findall(r"@\(('OBSERVER'[^)]*)\)", text):
            names |= {'SM2VR_ORDINARY_' + n for n in re.findall(r"'(\w+)'", listed)}
    return names


def current_text_files():
    """Current docs, scripts, tests, installer and config: everything but dated history docs and evidence."""
    paths = [REPO / name for name in CURRENT_FILES if (REPO / name).exists()]
    for root in CURRENT_ROOTS:
        paths += [p for p in sorted((REPO / root).rglob('*')) if p.is_file() and p.suffix in TEXT_SUFFIXES]
    for path in paths:
        relative = path.relative_to(REPO).as_posix()
        public = relative.removeprefix('release-docs/')
        if (relative.startswith(('docs/validation/', '.claude/worktrees/')) or HISTORY_DOC.match(public)
                or '__pycache__' in relative or path in (FLAGS, Path(__file__).resolve())):
            continue
        yield relative, path


class LaunchFlagDriftTests(unittest.TestCase):
    @needs_flags_doc
    def test_flags_doc_lists_every_launcher_switch_with_the_launchers_class(self):
        switches = launcher_switches()
        documented = flags_switches()
        self.assertEqual(sorted(documented), sorted(switches), 'docs/FLAGS.md launcher table vs the param() block')
        classes = launcher_classes()
        self.assertEqual(sorted(classes), sorted(switches), '$switchClass in the launcher vs the param() block')
        for name in switches:
            with self.subTest(name):
                self.assertEqual(len(documented[name]), 1, 'one row per switch')
                self.assertEqual(classes[name], [documented[name][0][1]], 'launcher class vs docs/FLAGS.md')
                self.assertIn(documented[name][0][1], CLASSES)

    @needs_flags_doc
    def test_flags_doc_counts_match_its_tables(self):
        def summary(rows):
            classes = [cells[0][1] for cells in rows.values()]
            return f'{len(classes)} ' + '{} (' + ', '.join(f'{classes.count(c)} {c}' for c in CLASSES if c in classes) + ')'
        expected = (f"Counts: {summary(flags_switches()).format('switches')}; "
                    f"{summary(flags_variables()).format('variables')}.")
        self.assertIn(expected, ' '.join(FLAGS.read_text(encoding='utf-8').split()))

    @needs_flags_doc
    def test_developing_table_names_only_switches_the_launcher_has(self):
        section = DEVELOPING.read_text(encoding='utf-8').split('## Launch options', 1)[1].split('\n## ', 1)[0]
        named = set(re.findall(r'^\| `-(\w+)', section, re.M))
        self.assertTrue(named)
        self.assertLessEqual(named, set(launcher_switches()), 'DEVELOPING.md documents a switch the launcher lacks')

    @needs_flags_doc
    def test_flags_doc_lists_every_variable_the_dll_reads(self):
        read = dll_variables()
        documented = flags_variables()
        self.assertEqual(sorted(documented), sorted(read), 'docs/FLAGS.md variable table vs mod/src')
        for name, rows in documented.items():
            with self.subTest(name):
                self.assertEqual(len(rows), 1, 'one row per variable')
                self.assertIn(rows[0][1], CLASSES)
                self.assertEqual(set(rows[0][3].split(', ')), read[name], 'the "Read in" column')

    def test_every_variable_the_launchers_set_is_read_by_the_dll(self):
        names = scripts_variables()
        read = dll_variables()
        self.assertIn('SM2VR_NATIVE_HUD', names)
        self.assertIn('SM2VR_ORDINARY_HEADSET', names, 'the ORDINARY flag lists are expanded')
        unread = sorted(names - set(read) - set(SCRIPT_ONLY))
        self.assertEqual(unread, [], 'set by the launcher or installer but read nowhere in mod/src')
        for name in SCRIPT_ONLY:
            with self.subTest(name):
                self.assertIn(name, names, 'stale exception: no script uses it')
                self.assertNotIn(name, read, 'stale exception: the DLL reads it')

    def test_shipped_profile_keys_follow_the_launcher_rule(self):
        # A feature launch.json ships on is on by default in the launcher (a shipped switch or a -NoX
        # opt-out); one it ships off has an opt-in. Values (runtime, gain, thumbs-up) take an argument.
        profile = json.loads(LAUNCH_JSON.read_text(encoding='utf-8'))
        if not DEV_REPO:
            self.skipTest('docs/FLAGS.md is left out of the public export')
        by_key = {}
        for name, rows in flags_switches().items():
            if key := rows[0][3].strip('`'):
                by_key.setdefault(key, []).append((name, rows[0][1]))
        self.assertEqual(sorted(by_key), sorted(set(profile) - {'schema', 'snap_degrees'}),
                         'every launch.json key has its launcher switch in docs/FLAGS.md')
        text = LAUNCHER.read_text(encoding='utf-8')
        for key, value in profile.items():
            if key not in by_key:
                continue
            with self.subTest(key):
                for name, cls in by_key[key]:
                    switch = re.search(r'\[switch\]\$' + name + r'\b', text) is not None
                    if isinstance(value, bool) and switch:
                        on_by_default = cls == 'shipped' or name.startswith('No')
                        self.assertEqual(on_by_default, value,
                                         f'launch.json {key}={value} but -{name} is {"on" if on_by_default else "off"} by default')
                    else:
                        self.assertFalse(switch, f'-{name} sets the value of {key}, so it takes an argument')

    def test_removed_switches_stay_out_of_current_docs_and_scripts(self):
        pattern = re.compile(r'(?<![\w-])-(' + '|'.join(REMOVED_SWITCHES) + r')(?!\w)')
        found = []
        for relative, path in current_text_files():
            for number, line in enumerate(path.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
                found += [f'{relative}:{number}: -{m}' for m in pattern.findall(line)]
        self.assertEqual(found, [])
        self.assertFalse(set(REMOVED_SWITCHES) & set(launcher_switches()))


class LauncherFlagTests(unittest.TestCase):
    """Windows PowerShell 5.1 (what this machine and players have) on the real launcher."""

    def test_launcher_refuses_removed_and_unknown_switches(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        for switch in (*REMOVED_SWITCHES, 'NoCompactHuds'):
            with self.subTest(switch):
                result = run_ps(LAUNCHER, '-Controls', '-Gestures', f'-{switch}', '-Tag', 'removed', '-PrintEnv')
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertRegex(result.stderr, 'Unknown launcher arguments|Missing an argument')

    def test_launch_flags_list_what_a_launch_runs_beyond_the_player_defaults(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TEMP) as work:
            root = Path(work)
            (root / 'scripts').mkdir()
            shutil.copyfile(LAUNCHER, root / 'scripts' / LAUNCHER.name)
            (root / 'installer' / 'app').mkdir(parents=True)
            shutil.copy(REPO / 'installer' / 'app' / 'sm2-install.ps1', root / 'installer' / 'app')
            for pack in ('vr_controls', 'chase_cam'):
                (root / 'build' / pack).mkdir(parents=True)
                (root / 'build' / pack / 'SM2VR_FirstPerson_Test.pak').write_bytes(b'pk')
            layout = re.search(r'(?m)^INPUT_LAYOUT = (\d+)', (REPO / 'scripts' / 'build_vr_controls.py').read_text(encoding='utf-8')).group(1)
            (root / 'build' / 'vr_controls' / 'SM2VR_FirstPerson_Test.json').write_text('{"input_layout": %s}' % layout)
            # run-modded.ps1 prints the summary and saves the flags; this stand-in returns them.
            (root / 'scripts' / 'run-modded.ps1').write_text(
                'param($Seconds,$Tag,$DataPak,$GameArgs,[switch]$NoUiAutomation,[switch]$AutoContinue,$LaunchFlags)\n'
                '$LaunchFlags | ConvertTo-Json -Compress -Depth 4\n', encoding='utf-8')

            def flags(*switches):
                result = run_ps(root / 'scripts' / LAUNCHER.name, *switches, '-Tag', 'flags')
                self.assertEqual(result.returncode, 0, result.stderr)
                return json.loads(result.stdout.strip().splitlines()[-1])

            player = flags('-Controls', '-Gestures')
            self.assertEqual(player['summary'], 'player defaults (first person)')
            self.assertEqual((player['player_options'], player['diagnostics'], player['dev_tools'], player['env_set'], player['env_off']),
                             ([], [], [], {}, []))
            self.assertEqual(flags('-ChaseCam', '-Seconds', '3600')['summary'], 'player defaults (chase)',
                             '-Seconds and -Tag are bookkeeping')
            self.assertEqual(flags('-Controls', '-Gestures', '-NoHaptics', '-HapticsGain', '0.5', '-Trace')['summary'],
                             'player options (first person): -HapticsGain 0.5 -NoHaptics -Trace')
            self.assertEqual(flags('-Controls')['summary'], 'player options (first person): no -Gestures')
            self.assertEqual(flags()['verdict'], 'NOT a player configuration', 'stereo only is not a player view')
            mixed = flags('-Controls', '-Gestures', '-DirectLaunch', '-NativeHzb', '-StereoWarmupJobs', '2400',
                          '-EnvSet', 'SM2VR_DIAG_NO_NATIVE_REFERENCE=1', '-EnvOff', 'SM2VR_STEREO')
            self.assertEqual(mixed['verdict'], 'NOT a player configuration')
            self.assertEqual(mixed['diagnostics'], ['-NativeHzb', '-StereoWarmupJobs 2400'])
            self.assertEqual(mixed['dev_tools'], ['-DirectLaunch'])
            self.assertEqual((mixed['env_set'], mixed['env_off']), ({'SM2VR_DIAG_NO_NATIVE_REFERENCE': '1'}, ['SM2VR_STEREO']))
            self.assertEqual(mixed['summary'], 'NOT a player configuration (first person): diagnostics -NativeHzb '
                             '-StereoWarmupJobs 2400; dev tools -DirectLaunch; -EnvSet SM2VR_DIAG_NO_NATIVE_REFERENCE=1; -EnvOff SM2VR_STEREO')
            implied = flags('-Controls', '-Gestures', '-TestDriver')
            self.assertEqual((implied['player_options'], implied['dev_tools']), (['-Trace'], ['-TestDriver']),
                             'implied switches count as they took effect')
            printed = run_ps(root / 'scripts' / LAUNCHER.name, '-Controls', '-Gestures', '-Tag', 'flags', '-PrintEnv')
            self.assertEqual(printed.returncode, 0, printed.stderr)
            self.assertNotIn('verdict', printed.stdout, '-PrintEnv prints only the environment')

    def test_launcher_refuses_a_controls_pack_of_another_button_layout(self):
        # A rebuilt DLL over an old pack puts interact on the trackpad. run-modded.ps1 stays out of this
        # tree, so the launcher can't reach the game whatever the check does.
        TEMP.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TEMP) as folder:
            root = Path(folder)
            (root / 'scripts').mkdir()
            shutil.copy(LAUNCHER, root / 'scripts')
            (root / 'installer' / 'app').mkdir(parents=True)
            shutil.copy(REPO / 'installer' / 'app' / 'sm2-install.ps1', root / 'installer' / 'app')
            pack = root / 'build' / 'vr_controls'
            pack.mkdir(parents=True)
            (pack / 'SM2VR_FirstPerson_Test.pak').write_bytes(b'pack')
            (pack / 'SM2VR_FirstPerson_Test.json').write_text('{"input_layout": 1}')
            result = run_ps(root / 'scripts' / LAUNCHER.name, '-Controls', '-Gestures', '-Tag', 'layout')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('out of date', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
