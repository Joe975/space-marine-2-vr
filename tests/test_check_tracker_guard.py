import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('tracker_guard', ROOT / 'scripts/check_tracker_guard.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

EXE = r'F:\Steam\steamapps\common\Space Marine 2\client_pc\root\bin\pc\Warhammer 40000 Space Marine 2 - Retail.exe'
DLL = EXE.replace('Retail.exe', 'Retail.dll')
INSTALLED = [
    '[19:44:58.000] [I] [tid:1] TrackerHandleGuard: image=exe installed=true refused=false enabled=true (getter falls back)',
    '[19:45:30.000] [I] [tid:1] TrackerHandleGuard: image=dll installed=true refused=false enabled=true (getter falls back)',
]
PAIR = '[19:47:40.060] [I] [tid:1] OrdinaryHeadset: consumed=1 submitted=0 dropped=0 dispatch=247 gpu_verified=true'


def crash(module, base, rip):
    return ['[19:47:40.085] [E] [tid:2] Exception occurred: c0000005',
            '[19:47:40.085] [E] [tid:2] RIP: %x' % rip,
            '[19:47:40.265] [E] [tid:2] Module: %x %s' % (base, module)]


class TrackerGuardCheckerTests(unittest.TestCase):
    def test_unguarded_family_crash_from_the_20_october_run(self):
        # Registers and module base of 20261020_194422_sprintcam_ab.
        run = checker.analyse(['[19:44:23.241] [I] sm2vr loaded', PAIR] + crash(EXE, 0x7ff769290000, 0x7ff76a5dc30c))
        self.assertEqual(run['verdict'], 'crash_family')
        self.assertEqual(run['crash']['site'], 'Retail.exe+134c30c')
        self.assertEqual(run['crash']['family'], 'exe getter')
        self.assertAlmostEqual(run['crash']['after_stereo_s'], 0.025)

    def test_server_validity_site_is_in_the_family(self):
        run = checker.analyse([PAIR] + crash(DLL, 0x7ff95b880000, 0x7ff95b880000 + 0x136a157))
        self.assertEqual(run['crash']['family'], 'dll two-handle validity')

    def test_guarded_trip_records_value_and_timing(self):
        trip = ('[19:47:40.071] [W] [tid:1] TrackerHandleGuardTrip: image=dll kind=getter thread=7 object=19b0c6915e0 '
                'vtable=7ff76c8b6b78 value=4d00000000 bytes_valid=true bytes=00')
        run = checker.analyse(INSTALLED + [PAIR, trip])
        self.assertEqual(run['verdict'], 'guarded_trip')
        self.assertEqual(run['trips'][0]['high_dword'], 0x4d)
        self.assertEqual(run['trips'][0]['low_dword'], 0)
        self.assertAlmostEqual(run['trips'][0]['after_stereo_s'], 0.011)

    def test_clean_and_incomplete_and_off_and_no_stereo(self):
        self.assertEqual(checker.analyse(INSTALLED + [PAIR])['verdict'], 'clean')
        self.assertEqual(checker.analyse(INSTALLED[:1] + [PAIR])['verdict'], 'guard_incomplete')
        refused = INSTALLED[0].replace('installed=true refused=false', 'installed=false refused=true')
        self.assertEqual(checker.analyse([refused, INSTALLED[1], PAIR])['verdict'], 'guard_incomplete')
        off = '[19:45:00.000] [I] [tid:1] TrackerHandleGuard: enabled=false exe_installed=false dll_installed=false'
        self.assertEqual(checker.analyse([off, PAIR])['verdict'], 'guard_off')
        self.assertEqual(checker.analyse(INSTALLED)['verdict'], 'no_stereo')

    def test_desktop_append_counts_as_stereo_start(self):
        append = ('[01:35:18.841] [I] [tid:3] OrdinarySecondCamera: render_id=7941 appended=true right=1 refusal=0 scenes=2 '
                  'pair=true file=x')
        run = checker.analyse([append] + crash(DLL, 0x7ff894a60000, 0x7ff894a60000 + 0xd6c19c))
        self.assertEqual(run['verdict'], 'crash_family')
        self.assertIsNotNone(run['crash']['after_stereo_s'])

    def test_other_crash_is_not_the_family(self):
        run = checker.analyse([PAIR] + crash(EXE, 0x7ff769290000, 0x7ff769290000 + 0x1d52c2a))
        self.assertEqual(run['verdict'], 'crash_other')
        self.assertIsNone(run['crash']['family'])

    def test_cli_exit_code_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            good, bad = Path(tmp, 'good'), Path(tmp, 'bad')
            good.mkdir()
            bad.mkdir()
            (good / 'sm2vr.log').write_text('\n'.join(INSTALLED + [PAIR]) + '\n', encoding='utf-8')
            (bad / 'sm2vr.log').write_text('\n'.join([PAIR] + crash(EXE, 0x7ff769290000, 0x7ff76a5dc30c)) + '\n', encoding='utf-8')
            out = Path(tmp, 'summary.json')
            script = str(ROOT / 'scripts/check_tracker_guard.py')
            ok = subprocess.run([sys.executable, '-I', script, str(good), '--json', str(out)], capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
            self.assertEqual(json.loads(out.read_text())['verdicts'], {'clean': 1})
            again = subprocess.run([sys.executable, '-I', script, str(good), '--json', str(out)], capture_output=True, text=True)
            self.assertNotEqual(again.returncode, 0)
            failed = subprocess.run([sys.executable, '-I', script, str(good), str(bad)], capture_output=True, text=True)
            self.assertEqual(failed.returncode, 1)


if __name__ == '__main__':
    unittest.main()
