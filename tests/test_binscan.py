import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import binscan  # noqa: E402


class ExtractStrings(unittest.TestCase):
    def test_ascii_and_utf16_with_offsets(self):
        data = b"\x00\x01short\x00CameraFov\x00\xff" + "StereoMode".encode("utf-16-le") + b"\x00\x00"
        found = list(binscan.extract_strings(data))
        self.assertIn((8, "a", "CameraFov"), found)
        self.assertIn((19, "u", "StereoMode"), found)

    def test_runs_below_min_length_are_dropped(self):
        self.assertEqual(list(binscan.extract_strings(b"\x00abcde\x00")), [])

    def test_entropy_bounds(self):
        self.assertEqual(binscan.entropy(b""), 0.0)
        self.assertEqual(binscan.entropy(b"\x00" * 64), 0.0)
        self.assertAlmostEqual(binscan.entropy(bytes(range(256))), 8.0)


if __name__ == "__main__":
    unittest.main()
