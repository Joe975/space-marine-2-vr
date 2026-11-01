from pathlib import Path
import subprocess
import tempfile
import unittest
from pwsh import pwsh


class RuntimeApiTests(unittest.TestCase):
    def test_owned_borrowed_and_changed_runtime_files(self):
        repo = Path(__file__).resolve().parents[1]
        temp = repo / 'build/test_temp'
        temp.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp) as work:
            result = subprocess.run([pwsh(), '-NoProfile', '-File',
                                     str(repo / 'tests/runtime_api_fixture.ps1'), work],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('runtime API ownership passed', result.stdout)
