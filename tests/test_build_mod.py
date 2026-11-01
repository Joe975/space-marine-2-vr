"""scripts/build-mod.bat must build every target and run every native suite. ctest runs whatever
binary is on disk, so a suite the script does not build runs stale and still passes (27 Oct 2026:
41 suites, vr_rumble among them, were never rebuilt)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildModTests(unittest.TestCase):
    def test_builds_every_target_and_runs_every_suite(self):
        lines = [line.strip() for line in (ROOT / 'scripts' / 'build-mod.bat').read_text(encoding='utf-8').splitlines()]
        commands = [line for line in lines if line and not line.lower().startswith('rem ')]
        full_build = [i for i, line in enumerate(commands)
                      if line.startswith('cmake --build') and '--target' not in line and '|| exit /b 1' in line]
        full_test = [i for i, line in enumerate(commands)
                     if line.startswith('ctest ') and not re.search(r'\s-R\s', line) and '|| exit /b 1' in line]
        self.assertTrue(full_build, 'a cmake --build with no --target (every target)')
        self.assertTrue(full_test, 'a ctest with no -R filter (every suite)')
        self.assertLess(full_build[-1], full_test[-1], 'the full test run comes after the full build')


if __name__ == '__main__':
    unittest.main()
