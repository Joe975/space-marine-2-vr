"""Update choice from a release manifest, the in-place package swap with rollback, and the crash
report's contents."""
from pathlib import Path
import hashlib
import json
import tempfile
import unittest
import zipfile

from installer_support import FIXTURES, TEMP, run_ps, run_ps_json

ITEMS = ('SM2VR.cmd', 'version.json', 'app/sm2-game.ps1', 'mod/dxgi.dll', 'builders/b.py', 'python/python.exe')


def release(version, timestamp='0x6ad10fcb'):
    return {'version': version, 'url': f'https://example.invalid/SM2VR-{version}.zip', 'sha256': '0' * 64,
            'size': 1, 'game_exe_timestamp': timestamp, 'changelog': ''}


def make_package(root, version, extra=()):
    for item in ITEMS + tuple(extra):
        path = root / item
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({'mod_version': version}) if item == 'version.json' else f'{item} {version}')


class UpdatePlanTests(unittest.TestCase):
    def plan(self, channels, current, channel='stable', timestamp=''):
        TEMP.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TEMP) as work:
            manifest = Path(work) / 'manifest.json'
            manifest.write_text(json.dumps({'schema': 1, 'channels': channels}))
            return run_ps_json(FIXTURES / 'update.ps1', '-Mode', 'plan', '-ManifestPath', manifest,
                               '-CurrentVersion', current, '-Channel', channel, '-GameExeTimestamp', timestamp)

    def test_choices(self):
        stable, beta = release('0.2.0'), release('0.3.0')
        cases = [
            ({'stable': stable}, '0.1.0', 'stable', ('update', '0.2.0')),
            ({'stable': stable}, '0.2.0', 'stable', ('none', '0.2.0')),
            ({'stable': stable, 'beta': beta}, '0.2.0', 'stable', ('none', '0.2.0')),
            ({'stable': stable, 'beta': beta}, '0.2.0', 'beta', ('update', '0.3.0')),
            # A beta older than stable is not offered; the beta user moves to stable.
            ({'stable': release('0.4.0'), 'beta': beta}, '0.3.0', 'beta', ('update', '0.4.0')),
            # Back from beta to stable is offered as a switch.
            ({'stable': stable, 'beta': beta}, '0.3.0', 'stable', ('update', '0.2.0')),
            ({}, '0.1.0', 'stable', ('none', None)),
        ]
        for channels, current, channel, (action, version) in cases:
            with self.subTest(channels=list(channels), current=current, channel=channel):
                plan = self.plan(channels, current, channel)
                self.assertEqual((plan['action'], plan['version']), (action, version))
        self.assertTrue(self.plan({'stable': stable, 'beta': beta}, '0.3.0')['message'].startswith('Switch to 0.2.0'))

    def test_game_version_mismatch_is_mentioned(self):
        plan = self.plan({'stable': release('0.2.0', '0x6b000000')}, '0.1.0', timestamp='0x6ad10fcb')
        self.assertIn('update the game in Steam', plan['message'])


class ApplyUpdateTests(unittest.TestCase):
    def setUp(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(dir=TEMP))
        self.root = self.work / 'SM2VR'
        make_package(self.root, '0.1.0', extra=('notes.txt',))
        new = self.work / 'new/SM2VR-0.2.0'
        make_package(new, '0.2.0')
        self.zip = self.work / 'SM2VR-0.2.0.zip'
        with zipfile.ZipFile(self.zip, 'w') as z:
            for path in new.rglob('*'):
                z.write(path, path.relative_to(new.parent))
        self.sha = hashlib.sha256(self.zip.read_bytes()).hexdigest()

    def apply(self, sha=None, ok=True):
        args = (FIXTURES / 'update.ps1', '-Mode', 'apply', '-Zip', self.zip, '-Sha256', sha or self.sha, '-PackageRoot', self.root)
        if ok:
            return run_ps_json(*args)
        result = run_ps(*args)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stdout + result.stderr

    def contents(self):
        return sorted(str(p.relative_to(self.root)).replace('\\', '/') for p in self.root.rglob('*') if p.is_file())

    def test_update_replaces_the_package_in_place(self):
        self.assertEqual(self.apply(), '0.2.0')
        self.assertEqual(self.contents(), sorted(ITEMS))
        self.assertEqual((self.root / 'mod/dxgi.dll').read_text(), 'mod/dxgi.dll 0.2.0')

    def test_bad_hash_changes_nothing(self):
        before = self.contents()
        self.assertIn('damaged', self.apply(sha='1' * 64, ok=False))
        self.assertEqual(self.contents(), before)

    def test_failed_swap_rolls_back(self):
        before = {p: (self.root / p).read_text() for p in self.contents()}
        # An open handle stops Windows moving notes.txt, after app/ builders/ mod/ already moved.
        with open(self.root / 'notes.txt'):
            message = self.apply(ok=False)
        self.assertIn('rolled back', message)
        self.assertEqual({p: (self.root / p).read_text() for p in self.contents()}, before)
        self.assertEqual([p.name for p in self.root.iterdir() if p.name.startswith('.')], [])

    def test_zip_missing_package_items_is_refused(self):
        with zipfile.ZipFile(self.zip, 'w') as z:
            z.writestr('SM2VR/readme.txt', 'not a release')
        self.sha = hashlib.sha256(self.zip.read_bytes()).hexdigest()
        self.assertIn('has no', self.apply(ok=False))
        self.assertIn('notes.txt', self.contents())


class CrashReportTests(unittest.TestCase):
    def test_newest_run_without_dumps(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(dir=TEMP))
        package, state = work / 'pkg', work / 'state'
        (package / 'mod').mkdir(parents=True)
        (package / 'mod/dxgi.dll').write_bytes(b'dll')
        (package / 'mod/launch.json').write_text('{}')
        (package / 'version.json').write_text(json.dumps({'mod_version': '0.1.0'}))
        for name, text in (('20261020_100000_play', 'old'), ('20261021_100000_play', 'new')):
            (state / 'runs' / name / 'dump').mkdir(parents=True)
            (state / 'runs' / name / 'sm2vr.log').write_text(text)
        (state / 'runs/20261021_100000_play/vrframework_crash.dmp').write_bytes(b'dump')
        (state / 'runs/20261021_100000_play/dump/shading.txt').write_text('x')
        (state / 'runs/20261021_100000_play/dump/scene_fault_pod.bin').write_bytes(b'heap')
        (state / 'install.json').write_text('{}')
        zip_path = Path(run_ps_json(FIXTURES / 'update.ps1', '-Mode', 'report', '-PackageRoot', package,
                                    '-StateRoot', state, '-OutDirectory', work / 'out'))
        with zipfile.ZipFile(zip_path) as z:
            names = sorted(n.replace('\\', '/') for n in z.namelist())
            self.assertEqual(z.read(next(n for n in z.namelist() if n.endswith('sm2vr.log'))), b'new')
            report = json.loads(z.read('report.json').decode('utf-8'))
        self.assertEqual(names, ['install.json', 'launch.json', 'report.json', 'run/dump/shading.txt',
                                 'run/sm2vr.log', 'version.json'])
        self.assertEqual(sorted(report['skipped_files']), ['scene_fault_pod.bin', 'vrframework_crash.dmp'])
        self.assertEqual(report['dll_sha256'], hashlib.sha256(b'dll').hexdigest())
        self.assertEqual([p.name for p in state.iterdir() if p.name.startswith('report-')], [])


if __name__ == '__main__':
    unittest.main()
