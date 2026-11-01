import math
import re
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import fit_openxr_grip_offset as fit  # noqa: E402

RENDER_MODEL = {0: ([0, -0.015, 0.13], [15.392, -2.071, 0.303]), 1: ([0, -0.015, 0.13], [15.392, 2.071, -0.303])}


def raw_trajectory(hand, t):
    """Smooth synthetic controller motion: a sweeping arc with wrist rotation."""
    rotation = fit.compose_degrees([40 * math.sin(1.3 * t + hand), 70 * math.sin(0.7 * t), 30 * math.cos(1.1 * t)])
    return fit.pose(rotation, [0.3 * math.sin(t) - 0.2 + 0.4 * hand, 1.1 + 0.2 * math.cos(0.8 * t), -0.3 + 0.1 * math.sin(2 * t)])


def write(path, rows):
    with open(path, "w") as out:
        out.write("qpc,hand," + ",".join(f"m{n}" for n in range(16)) + "\n")
        for qpc, hand, matrix in rows:
            out.write(f"{qpc},{hand}," + ",".join(f"{v:.9g}" for v in matrix.reshape(16)) + "\n")


class FitTests(unittest.TestCase):
    def test_degree_convention_round_trips(self):
        for degrees in ([15.392, -2.071, 0.303], [-90, -45, 90 - 1e-3], [10, 20, 30], [0, 0, 0]):
            recovered = fit.decompose_degrees(fit.compose_degrees(degrees))
            np.testing.assert_allclose(fit.compose_degrees(recovered), fit.compose_degrees(degrees), atol=1e-9)

    def test_quaternion_round_trip_and_slerp_endpoints(self):
        rotation = fit.compose_degrees([33, -71, 120])
        q = fit.to_quaternion(rotation)
        np.testing.assert_allclose(fit.from_quaternion(q), rotation, atol=1e-9)
        other = fit.to_quaternion(fit.compose_degrees([0, 10, 0]))
        np.testing.assert_allclose(abs(np.dot(fit.slerp(q, other, 0), q)), 1, atol=1e-9)
        np.testing.assert_allclose(abs(np.dot(fit.slerp(q, other, 1), other)), 1, atol=1e-9)

    def test_recovers_known_offset_from_interleaved_logs(self):
        frequency = 10_000_000
        witness, grips = [], []
        offsets = {hand: fit.pose(fit.compose_degrees(d), m) for hand, (m, d) in RENDER_MODEL.items()}
        for n in range(3000):  # 2 ms witness samples
            t = n * 0.002
            for hand in (0, 1):
                witness.append((int(t * frequency), hand, raw_trajectory(hand, t)))
        for n in range(500):  # 11.1 ms OpenXR samples, between witness samples
            t = 0.0013 + n * 0.0111
            for hand in (0, 1):
                grips.append((int(t * frequency), hand, offsets[hand] @ raw_trajectory(hand, t)))
        with tempfile.TemporaryDirectory() as directory:
            w, g, out = Path(directory) / "w.csv", Path(directory) / "g.csv", Path(directory) / "grip.calibration"
            write(w, witness)
            write(g, grips)
            self.assertEqual(fit.main(["--witness", str(w), "--grips", str(g), "--out", str(out)]), 0)
            text = out.read_text()
        self.assertIn("measured=1", text)
        for hand, name in ((0, "left"), (1, "right")):
            metres = [float(v) for v in re.search(rf"^{name}_grip_in_raw_metres=(.*)$", text, re.M).group(1).split()]
            degrees = [float(v) for v in re.search(rf"^{name}_grip_in_raw_degrees=(.*)$", text, re.M).group(1).split()]
            np.testing.assert_allclose(metres, RENDER_MODEL[hand][0], atol=5e-4)
            np.testing.assert_allclose(degrees, RENDER_MODEL[hand][1], atol=0.05)

    def test_inconsistent_offsets_fail_the_gate(self):
        frequency = 10_000_000
        witness, grips = [], []
        for n in range(3000):
            t = n * 0.002
            witness += [(int(t * frequency), hand, raw_trajectory(hand, t)) for hand in (0, 1)]
        for n in range(500):
            t = 0.0013 + n * 0.0111
            wrong = fit.pose(fit.compose_degrees([15 + (n % 2) * 6, 0, 0]), [0, -0.015, 0.13 + (n % 2) * 0.01])
            grips += [(int(t * frequency), hand, wrong @ raw_trajectory(hand, t)) for hand in (0, 1)]
        with tempfile.TemporaryDirectory() as directory:
            w, g, out = Path(directory) / "w.csv", Path(directory) / "g.csv", Path(directory) / "grip.calibration"
            write(w, witness)
            write(g, grips)
            self.assertEqual(fit.main(["--witness", str(w), "--grips", str(g), "--out", str(out)]), 1)
            self.assertIn("measured=0", out.read_text())

    def test_gaps_are_not_interpolated(self):
        samples = [(0, np.eye(4)), (1_000_000, np.eye(4))]
        self.assertIsNone(fit.interpolate(samples, 500_000, max_gap=100_000))
        self.assertIsNone(fit.interpolate(samples, 2_000_000, max_gap=10_000_000))


if __name__ == "__main__":
    unittest.main()
