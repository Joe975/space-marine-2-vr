import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import still_view_changes as module  # noqa: E402


class StillViewChangesTest(unittest.TestCase):
    def test_counts_captures_where_a_few_pixels_toggle(self):
        times = np.arange(40) * 0.03
        still = np.full((40, 30, 20), 50.0, np.float32)
        noisy = still + np.random.default_rng(1).normal(0, 0.5, still.shape).astype(np.float32)  # capture noise only
        spot = still.copy()
        spot[10:14, 5:7, 5:7] += 40  # a 2x2 specular spot lit for 4 frames: on at 10, off at 14
        self.assertEqual(module.changes(noisy, times)[0], 0)
        count, per_second, _ = module.changes(spot, times)
        self.assertEqual(count, 2)
        self.assertAlmostEqual(per_second, 2 / 1.17, places=3)
        small = still.copy()
        small[10:14, 5, 5] += 40  # one pixel is under the 4-pixel floor
        self.assertEqual(module.changes(small, times)[0], 0)


if __name__ == "__main__":
    unittest.main()
