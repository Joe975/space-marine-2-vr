import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from check_camera_log import check_log


def sample(pos="1,2,3", right="1,0,0", fwd="0,0,1", up="0,1,0", age="0.001", engine_pos=None):
    prefix = "[16:40:00.000] [I] "
    lines = ["StreamlineProbe: state=1 calls=100 invalid=0 rejected=0",
             f"StreamlineProbe: frame=100 viewport=0 result=0 age={age}s pos=[{pos}] "
             f"right=[{right}] up=[{up}] fwd=[{fwd}] near=0.1 far=1000 "
             "fov_rad=1.5707963267948966 aspect=2 depth_inverted=1",
             "StreamlineProbe: projection[0]=[0.5,0,0,0]",
             "StreamlineProbe: projection[1]=[0,1,0,0]",
             f"StreamlineProbe: projection[2]=[0,0,{0.1/(0.1-1000)},1]",
             f"StreamlineProbe: projection[3]=[0,0,{-0.1*1000/(0.1-1000)},0]",
             f"EngineCameraProbe: caller=0x18033bf calls=100 age=0.008s index=0 pos=[{engine_pos or pos}] "
             f"row0=[{right}] row1=[{up}] row2=[{fwd}] near=0.1 far=1000 fov_v=90"]
    return "\n".join(prefix + line for line in lines) + "\n"


class CameraEvidence(unittest.TestCase):
    def test_actual_projection_convention_and_rotated_motion(self):
        report = check_log(sample() + sample(pos="2,2,3", right="0,0,-1", fwd="1,0,0"), require_motion=True)
        self.assertEqual(report["matching_engine_samples"], 2)
        self.assertEqual(report["position_change_from_first"], 1)
        self.assertAlmostEqual(report["forward_change_from_first"], math.sqrt(2))

    def test_stale_and_static_samples_cannot_prove_motion(self):
        for text in (sample(age="12"), sample(), sample() + sample(pos="2,2,3")):
            with self.assertRaises(ValueError):
                check_log(text, require_motion=True)

    def test_bad_basis_projection_errors_and_wrong_engine_are_rejected(self):
        for text in (sample(up="1,0,0"), sample().replace("[0.5,0,0,0]", "[2,0,0,0]"),
                     sample().replace("invalid=0", "invalid=1"), sample(engine_pos="9,9,9"),
                     sample(engine_pos="1,2"),
                     sample().replace("index=0", "index=2"), sample(pos="nan,2,3")):
            with self.assertRaises(ValueError):
                check_log(text)

    def test_asynchronous_movement_may_mismatch_but_needs_a_match(self):
        report = check_log(sample() + sample(pos="2,2,3", engine_pos="1,2,3"))
        self.assertEqual(report["fresh_gameplay_samples"], 2)
        self.assertEqual(report["matching_engine_samples"], 1)

    def test_time_filter_and_incomplete_projection_fail_closed(self):
        with self.assertRaises(ValueError):
            check_log(sample(), since="17:00:00")
        with self.assertRaises(ValueError):
            check_log(sample().replace("projection[3]", "inverse_projection[3]"))


if __name__ == "__main__":
    unittest.main()
