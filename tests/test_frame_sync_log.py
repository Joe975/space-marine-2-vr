import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("frame_checker", Path(__file__).resolve().parents[1] / "scripts/check_frame_sync_log.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def capture():
    lines = []

    def event(kind, marker=0, frame=-1, render=-1, presenter=-1, render_id=0, present_id=0, eye=-1):
        seq = len(lines)+1
        lines.append(f"FrameSyncTrace: seq={seq} t={seq*10} thread=7 kind={kind} marker={marker} frame={frame} "
                     f"simulation={frame} render={render} presenter={presenter} render_id={render_id} present_id={present_id} eye={eye} result=0")
    for token, identity, eye in [(100,1,0),(101,2,1)]:
        for phase in range(6):
            event(0,phase,token,token if phase >= 2 else -1,token if phase >= 4 else -1,
                  identity if phase >= 2 else 0,identity if phase >= 4 else 0,eye if phase >= 2 else -1)
            if phase == 1:
                event(1)
            if phase == 3:
                event(2,frame=token)
                event(3)
                event(3)  # Additional presentations are not new rendered eyes.
        event(0,11,token)
    return "\n".join(lines)


class FrameSyncLogTests(unittest.TestCase):
    def test_complete_pipeline_with_extra_presentations(self):
        report = checker.analyze(capture())
        self.assertTrue(report["valid"])
        self.assertEqual(report["complete_frames"], 2)
        self.assertEqual(report["present_callbacks"], 4)
        self.assertEqual(report["out_of_band_markers"], 2)
        self.assertEqual([frame["eye"] for frame in report["frames"]], [0,1])

    def test_wrong_phase_and_binding_fail(self):
        wrong_order = capture().replace("kind=0 marker=2 frame=100", "kind=0 marker=TEMP frame=100").replace("kind=0 marker=3 frame=100", "kind=0 marker=2 frame=100").replace("marker=TEMP", "marker=3")
        self.assertFalse(checker.analyze(wrong_order)["valid"])
        wrong_binding = capture().replace("present_id=1", "present_id=99")
        self.assertGreater(checker.analyze(wrong_binding)["binding_errors"], 0)

    def test_incomplete_empty_and_malformed_fail(self):
        self.assertFalse(checker.analyze("")["valid"])
        self.assertFalse(checker.analyze(capture().split("frame=101")[0])["valid"])
        self.assertFalse(checker.analyze(capture()+"\nFrameSyncTrace: invalid")["valid"])

    def test_sequence_and_rejected_markers_fail(self):
        self.assertFalse(checker.analyze(capture().replace("seq=2 ", "seq=1 "))["valid"])
        self.assertFalse(checker.analyze(capture().replace("result=0", "result=1", 1))["valid"])

    def test_missing_camera_or_constants_is_not_evidence(self):
        self.assertFalse(checker.analyze(capture().replace("kind=1", "kind=3"))["valid"])
        self.assertFalse(checker.analyze(capture().replace("kind=2", "kind=3"))["valid"])

    def test_sm2_cached_start_and_late_simulation_end(self):
        rows = capture().splitlines()
        # Independent fixture: move end to after present-end and offset each raw start only.
        end_rows = [line for line in rows if "kind=0 marker=1 " in line]
        rows = [line for line in rows if "kind=0 marker=1 " not in line]
        for token, end in zip((100,101),end_rows):
            rows = [line.replace(f"marker=0 frame={token} ", f"marker=0 frame={token-1} ") if "kind=0 marker=0 " in line else line for line in rows]
            index = next(i for i,line in enumerate(rows) if f"marker=5 frame={token} " in line)
            rows.insert(index+1,end)
        import re
        text = "\n".join(re.sub(r"seq=\d+ t=\d+", f"seq={i} t={i*10}", line) for i,line in enumerate(rows,1))
        report = checker.analyze(text,"sm2")
        self.assertTrue(report["valid"])
        self.assertFalse(checker.analyze(text,"standard")["valid"])

    def test_repeated_capture_selects_last_window(self):
        report = checker.analyze("FrameSyncTrace: invalid\nFrameSyncCapture: count=16384\n"+capture())
        self.assertTrue(report["valid"])
        self.assertEqual(report["malformed"],0)


if __name__ == "__main__":
    unittest.main()
