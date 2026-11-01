from pathlib import Path
import subprocess
import tempfile
import unittest
from pwsh import pwsh


class VideoSettingsTests(unittest.TestCase):
    def test_override_validation_json_edit_backup_restore_and_crash_marker(self):
        repo = Path(__file__).resolve().parents[1]
        temp = repo / 'build/test_temp'
        temp.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp) as work:
            result = subprocess.run([pwsh(), '-NoProfile', '-File',
                                     str(repo / 'tests/video_settings_fixture.ps1'), work],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('video settings fixture passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
