import importlib.util
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "run_replay_extract.py"
spec = importlib.util.spec_from_file_location("run_replay_extract", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class SupervisorTests(unittest.TestCase):
    def test_retained_child_success(self):
        with tempfile.TemporaryDirectory() as d:
            receipt = Path(d) / "receipt.json"
            result = module.run_child([sys.executable, "-c", "pass"], 5, receipt)
            self.assertEqual(result["status"], "child_exited")
            self.assertEqual(result["child_exit"], 0)
            self.assertFalse(result["terminated"])
    def test_blocking_child_killed_and_waited_incomplete(self):
        with tempfile.TemporaryDirectory() as d:
            result = module.run_child([sys.executable, "-c", "import time;time.sleep(30)"], .1, Path(d)/"receipt.json")
            self.assertEqual(result["status"], "timed_out_incomplete")
            self.assertTrue(result["terminated"])
            self.assertIsNotNone(result["child_exit"])
            self.assertLess(result["elapsed_seconds"], 5)
    def test_nonzero_child_not_complete(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(module.run_child([sys.executable, "-c", "raise SystemExit(7)"], 5, Path(d)/"receipt.json")["status"], "child_failed")
    def test_failed_kill_is_explicit_and_never_unbounded_wait(self):
        child = mock.Mock(returncode=None)
        child.wait.side_effect = subprocess.TimeoutExpired("fake", .1)
        child.kill.side_effect = OSError("cannot terminate")
        with tempfile.TemporaryDirectory() as d, mock.patch.object(module.subprocess, "Popen", return_value=child):
            result = module.run_child(["fake"], .1, Path(d)/"receipt.json")
            self.assertEqual(result["status"], "termination_unconfirmed_incomplete")
            self.assertFalse(result["terminated"])
            self.assertEqual(child.wait.call_args_list, [mock.call(timeout=.1)])
    def test_kill_wait_has_separate_bound(self):
        child = mock.Mock(returncode=None)
        child.wait.side_effect = subprocess.TimeoutExpired("fake", .1)
        with tempfile.TemporaryDirectory() as d, mock.patch.object(module.subprocess, "Popen", return_value=child):
            result = module.run_child(["fake"], .1, Path(d)/"receipt.json")
            self.assertEqual(result["status"], "termination_unconfirmed_incomplete")
            self.assertEqual(child.wait.call_args_list, [mock.call(timeout=.1), mock.call(timeout=5)])
    def test_exclusive_reservation_race_never_launches(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"receipt.json"
            original=Path.open
            def raced(path, mode="r", *args, **kwargs):
                if path == p and mode == "x":
                    p.write_text("other run")
                return original(path, mode, *args, **kwargs)
            with mock.patch.object(Path, "open", raced), mock.patch.object(module.subprocess, "Popen") as launch:
                with self.assertRaises(FileExistsError): module.run_child(["fake"], 1, p)
                launch.assert_not_called()
            self.assertEqual(p.read_text(), "other run")
    def test_receipt_overwrite_and_unbounded_timeout_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"receipt.json";p.write_text("existing")
            with self.assertRaises(ValueError): module.run_child([sys.executable], 5, p)
            with self.assertRaises(ValueError): module.run_child([sys.executable], 181, Path(d)/"new.json")

if __name__ == "__main__": unittest.main()
