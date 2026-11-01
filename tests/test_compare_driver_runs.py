import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import compare_driver_runs as compare  # noqa: E402


class SummaryTests(unittest.TestCase):
    def test_buttons_events_and_displacement(self):
        report = {"terminal": "completed", "samples": 10, "root_displacement_initial_view_axes": {"right_metres": -9.654, "forward_metres": 0.004},
                  "events": [{"event": "test_driver", "detail": "x running"}, {"event": "button", "name": "A_dodge_confirm", "down": True},
                             {"event": "snap_turn"}, {"event": "snap_turn"}, {"event": "button", "name": "A_dodge_confirm", "down": False}]}
        s = compare.summary(report)
        self.assertEqual(s["buttons"], ["A_dodge_confirm+", "A_dodge_confirm-"])
        self.assertEqual(s["events"], {"snap_turn": 2})
        self.assertEqual((s["view_right_m"], s["view_forward_m"]), (-9.65, 0.0))
        self.assertEqual(s["terminal"], "completed")

    def test_report_errors_pass_through(self):
        self.assertEqual(compare.summary({"error": "unknown id"}), {"error": "unknown id"})


if __name__ == "__main__":
    unittest.main()
