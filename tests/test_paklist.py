import sys
import unittest
from collections import namedtuple
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import paklist  # noqa: E402

Info = namedtuple("Info", "filename compress_type")


class PakList(unittest.TestCase):
    def test_compound_extension(self):
        self.assertEqual(paklist.ext_of("ssl/camera/camera.ssl.resource"), ".ssl.resource")
        self.assertEqual(paklist.ext_of("a/B.PCT"), ".pct")
        self.assertEqual(paklist.ext_of("a.b/noext"), "(none)")

    def test_summary_skips_directories(self):
        n, methods, exts = paklist.summarize([Info("ssl/", 0), Info("ssl/a.sso", 0), Info("ssl/b.sso", 8)])
        self.assertEqual(n, 3)
        self.assertEqual(methods, {0: 2, 8: 1})
        self.assertEqual(exts, {".sso": 2})


if __name__ == "__main__":
    unittest.main()
