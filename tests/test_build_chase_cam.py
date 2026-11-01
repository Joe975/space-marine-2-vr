import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('build_chase_cam', Path(__file__).parents[1] / 'scripts/build_chase_cam.py')
chase = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chase)


def pack(path, entries):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_STORED) as z:
        for name, data in entries.items():
            z.writestr(zipfile.ZipInfo(name, date_time=(2026, 10, 12, 0, 0, 0)), data)


class ChaseCamPackTests(unittest.TestCase):
    def test_drops_only_camera_files_and_keeps_the_rest_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as tmp:
            base, out = Path(tmp) / 'base.pak', Path(tmp) / 'out' / 'chase.pak'
            pack(base, {'ssl/camera/cam_modes/cam_base.sso': b'fp camera',
                        'ssl/camera/cam_modes/mgs/cam_idle.sso': b'fp idle',
                        'ssl/ui/menu.sso': b'vr menu',
                        'presets/data/default_blur/motion_blur_low.s3dprs': b'no blur'})
            manifest = chase.build(base, out)
            with zipfile.ZipFile(out) as z:
                self.assertEqual(sorted(z.namelist()), ['presets/data/default_blur/motion_blur_low.s3dprs', 'ssl/ui/menu.sso'])
                self.assertEqual(z.read('ssl/ui/menu.sso'), b'vr menu')
                self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()))
            self.assertEqual(manifest['removed'], ['ssl/camera/cam_modes/cam_base.sso', 'ssl/camera/cam_modes/mgs/cam_idle.sso'])
            self.assertEqual(json.loads(out.with_suffix('.json').read_text())['pak_sha256'], manifest['pak_sha256'])

    def test_refuses_a_base_without_camera_files(self):
        # Stripping nothing would silently install a pack that changes nothing about the camera.
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / 'base.pak'
            pack(base, {'ssl/ui/menu.sso': b'vr menu'})
            with self.assertRaises(ValueError):
                chase.build(base, Path(tmp) / 'out.pak')

    def test_is_deterministic(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / 'base.pak'
            pack(base, {'ssl/camera/a.sso': b'a', 'ssl/ui/b.sso': b'b'})
            first = chase.build(base, Path(tmp) / 'one.pak')['pak_sha256']
            self.assertEqual(chase.build(base, Path(tmp) / 'two.pak')['pak_sha256'], first)


if __name__ == '__main__':
    unittest.main()
