import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"scripts"))
from build_benchmark_fov import build, CAMERAS


class FovIsolation(unittest.TestCase):
    def test_reference_cannot_be_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)/"base.pak"
            base.write_bytes(b"reference must survive")
            with self.assertRaisesRegex(ValueError, "reference pack"):
                build(base, base, 100)
            self.assertEqual(base.read_bytes(), b"reference must survive")

    def test_only_native_fov_changes_and_all_other_assets_survive(self):
        with tempfile.TemporaryDirectory() as folder:
            base, output = Path(folder)/"base.pak", Path(folder)/"out.pak"
            original = b'camera = {\r\n   fov = 110.0\r\n   near = 0.1\r\n}\r\n'
            with zipfile.ZipFile(base, "w") as archive:
                for name in CAMERAS:
                    archive.writestr(name, original)
                archive.writestr("menu.sso", b"unchanged menus and controls")
            report = build(base, output, 100)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.read("menu.sso"), b"unchanged menus and controls")
                for name in CAMERAS:
                    self.assertEqual(archive.read(name), original.replace(b"110.0", b"100.0"))
                self.assertEqual(set(archive.namelist()), {*CAMERAS, "menu.sso"})
            self.assertEqual(len(report["changed_entries"]), len(CAMERAS))
            self.assertFalse(report["runtime_eye_projection_changed"])

    def test_invalid_or_ambiguous_source_fails_instead_of_mislabeling(self):
        with tempfile.TemporaryDirectory() as folder:
            base, output = Path(folder)/"base.pak", Path(folder)/"out.pak"
            for fov in (79, 121, float("nan"), True):
                with self.assertRaises(ValueError):
                    build(base, output, fov)
            with zipfile.ZipFile(base, "w") as archive:
                for name in CAMERAS:
                    archive.writestr(name, b"fov = 110.0\nfov = 100.0\n")
            with self.assertRaisesRegex(ValueError, "one native FOV"):
                build(base, output, 100)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
