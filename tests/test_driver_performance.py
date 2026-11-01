import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"scripts"))
from qualify_driver_performance import qualify, qualify_allocation, load_records
from benchmark_driver_performance import read_status


def fixture():
    summary = {"qpc_start_ms": 1000, "qpc_end_ms": 2000}
    clock = {"utc": "2026-10-19T00:00:00+00:00", "qpc_ms": 0}
    statuses = [{"id": "hold", "state": "running", "error": "", "qpc_ms": q} for q in range(750, 2251, 250)]
    statuses.append({"id": "hold", "state": "completed", "error": "", "qpc_ms": 4000})
    records = [{"type": "header", "wall_clock_local": "2026-10-19T00:00:00+00:00"},
               {"event": "test_driver", "detail": "hold running ", "t": .5},
               {"event": "test_driver", "detail": "hold completed ", "t": 3}]
    for n in range(31):
        records.append({"type": "sample", "t": 1+n/30,
                        "input": {"age_ms": 1, "head": [0, 1.7, 0], "head_forward": [0, 0, -1],
                                  "left": [-.18, 1.6, -.5], "right": [.18, 1.6, -.5],
                                  "pad": {"buttons": 0, "lt": 0, "rt": 0, "lx": 0, "ly": 0}},
                        "camera": {"age_ms": 1, "anchored": True, "eye_centre": [0, 5, 10], "eye_forward": [0, 0, -1]},
                        "character": {"age_ms": 1, "actor": "a", "driven": True, "units_per_metre": .9, "root": [0, 3, 10], "root_z_axis": [0, 0, -1]},
                        "game": {"available": True, "cinematic": False, "paused": False}})
    return summary, clock, statuses, records, "hold"


class DriverPerformanceTests(unittest.TestCase):
    def test_status_read_retries_sharing_race_without_reusing_old_state(self):
        path = Mock()
        path.read_text.side_effect = [PermissionError(), FileNotFoundError(), '{"state":"running"}']
        with patch("benchmark_driver_performance.time.sleep"):
            self.assertEqual(read_status(path), {"state": "running"})
        self.assertEqual(path.read_text.call_count, 3)
        path.read_text.side_effect = PermissionError()
        with patch("benchmark_driver_performance.time.sleep"), self.assertRaises(PermissionError):
            read_status(path)
        self.assertEqual(path.read_text.call_count, 9)
    def test_allocation_policy_must_be_observed_in_the_capture(self):
        summary = dict(fixture()[0], submitted_pair_count=10)
        rows = [{"qpc_start_ms": 1100+n*30, "event": "inline_copy_allocate_cpu", "value": "1"} for n in range(20)]
        self.assertEqual(qualify_allocation(rows, summary, "pooled")["observations"], 20)
        with self.assertRaisesRegex(ValueError, "allocation policy"):
            qualify_allocation(rows, summary, "committed")
        rows[10]["value"] = "0"
        with self.assertRaisesRegex(ValueError, "allocation policy"):
            qualify_allocation(rows, summary, "pooled")
        with self.assertRaisesRegex(ValueError, "allocation policy"):
            qualify_allocation([], summary, "pooled")
    def test_complete_actual_hold(self):
        result = qualify(*fixture())
        self.assertEqual(result["samples"], 31)
        self.assertLess(result["hand_max_error_metres"], 1e-10)
        self.assertEqual(result["position_extent_metres"]["camera"], 0)

    def test_cancellation_or_replacement(self):
        for field, value in (("state", "focus_lost"), ("id", "other"), ("error", "busy_stop_first")):
            data = fixture()
            data[2][3][field] = value
            with self.assertRaisesRegex(ValueError, "inactive"):
                qualify(*data)

    def test_status_gap(self):
        data = list(fixture())
        data[2] = [data[2][0], data[2][-1]]
        with self.assertRaisesRegex(ValueError, "coverage"):
            qualify(*data)

    def test_native_markers_must_cover_exact_window(self):
        for change in ("cancelled", "early_end", "late_start"):
            data = fixture()
            if change == "cancelled":
                data[3][2]["detail"] = "hold clock_gap "
            elif change == "early_end":
                data[3][2]["t"] = 1.8
            else:
                data[3][1]["t"] = 1.2
            with self.assertRaisesRegex(ValueError, "markers"):
                qualify(*data)

    def test_requires_completion(self):
        data = fixture()
        data[2][-1]["state"] = "running"
        with self.assertRaisesRegex(ValueError, "complete"):
            qualify(*data)

    def test_trace_gap(self):
        data = list(fixture())
        data[3] = [r for r in data[3] if not 1.3 < r.get("t", 0) < 1.6]
        with self.assertRaisesRegex(ValueError, "coverage"):
            qualify(*data)

    def test_actual_pose_and_control_failures(self):
        cases = (("input", "age_ms", 200), ("input", "head", [0, 1.7, .1]),
                 ("input", "right", [.18, 1.6, -.7]), ("input", "left", [float("nan"), 1.6, -.5]),
                 ("camera", "eye_centre", [0, 5.1, 10]), ("camera", "anchored", False),
                 ("camera", "eye_forward", [.1, 0, -.995]), ("character", "root_z_axis", [.1, 0, -.995]),
                 ("character", "actor", "b"), ("character", "driven", False),
                 ("character", "root", [0, 3, 10.1]), ("character", "units_per_metre", 0),
                 ("game", "paused", True), ("game", "cinematic", True), ("game", "available", False))
        for section, key, value in cases:
            with self.subTest(section=section, key=key):
                data = fixture()
                data[3][15][section][key] = value
                with self.assertRaises(ValueError):
                    qualify(*data)
        data = fixture()
        data[3][15]["input"]["pad"]["buttons"] = 1
        with self.assertRaisesRegex(ValueError, "non-neutral"):
            qualify(*data)

    def test_qpc_alignment_excludes_pose_changes_outside_capture(self):
        data = fixture()
        before = copy.deepcopy(data[3][-1])
        before["t"] = .7
        before["input"]["right"] = [0, 0, 0]
        data[3].insert(3, before)
        self.assertEqual(qualify(*data)["samples"], 31)

    def test_only_trailing_partial_json_is_tolerated(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"trace.jsonl"
            path.write_text('{"type":"sample"}\n{"t":', encoding="utf-8")
            self.assertEqual(len(load_records(path)), 1)
            path.write_text('{"type":"sample"}\nbad\n{"t":1}\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "malformed"):
                load_records(path)


if __name__ == "__main__":
    unittest.main()
