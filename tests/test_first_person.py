import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('first_person', Path(__file__).parents[1] / 'scripts/build_first_person.py')
fp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fp)

CAMERA = '''distanceSettings = {
   transitions = { default = { spline = [0, 1, 4, 1] } }
   distance = 4
   __type = "CameraValueDistance"
}
interestSettings = { interest = { x = -1 } }
untouched = { nested = { distance = 99 } }
__type = "CamBase"
'''


class FirstPersonTests(unittest.TestCase):
    def test_nested_export_has_one_assignment_per_line(self):
        actual = fp.object_text('Outer', nested=fp.vector(y=2.8), enabled='True')
        self.assertEqual(actual, '{\n   nested = {\n      x = 0\n      y = 2.8\n      z = 0\n      __type = "Vector3d"\n   }\n   enabled = True\n   __type = "Outer"\n}')
        # Check all generated camera fields, excluding unrelated fixture content.
        actual = fp.camera_patch('__type = "CamBase"\n', fp.parameters({}))
        self.assertTrue(all(line.count('=') <= 1 for line in actual.splitlines()))
        depth = 0
        for line in actual.splitlines():
            depth += line.count('{') - line.count('}')
            self.assertGreaterEqual(depth, 0)
        self.assertEqual(depth, 0)

    def test_real_pack_path_changes_parameters_and_preserves_other_fields(self):
        scratch = Path(__file__).parents[1] / 'build' / 'test-temp'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as tmp:
            root = Path(tmp)
            source = root / 'source.pak'
            with zipfile.ZipFile(source, 'w') as z:
                for name in fp.CAMERAS:
                    z.writestr(name, CAMERA)
                z.writestr(fp.OPACITY, 'bodyStateCondition = { condition = { bodyStateArray = ["STANDING_MODE"] } }\n__type = "prop_base"\n')
                z.writestr('ssl/unrelated.sso', 'unrelated')
            first = fp.build(source, root / 'first.pak', {})
            second = fp.build(source, root / 'second.pak', {'head_height': 2.8, 'forward': 0.3})
            repeat = fp.build(source, root / 'repeat.pak', {})
            isolated = fp.build(source, root / 'camera-only.pak', {}, camera_only=True)
            wide = fp.build(source, root / 'wide.pak', {'fov': 110}, camera_only=True)
            with zipfile.ZipFile(root / 'wide.pak') as z, zipfile.ZipFile(root / 'camera-only.pak') as baseline:
                self.assertEqual(set(z.namelist()), set(baseline.namelist()))
                for name in fp.CAMERAS:
                    self.assertEqual(z.read(name).decode(), baseline.read(name).decode().replace('fov = 80.0', 'fov = 110'))
            self.assertTrue(wide['camera_only'])
            with zipfile.ZipFile(root / 'camera-only.pak') as z:
                self.assertEqual(set(z.namelist()), set(fp.CAMERAS))
                for name in fp.CAMERAS:
                    with zipfile.ZipFile(root / 'first.pak') as full:
                        self.assertEqual(z.read(name), full.read(name))
            self.assertTrue(isolated['camera_only'])
            self.assertFalse(first['camera_only'])
            self.assertEqual(first['pak_sha256'], repeat['pak_sha256'])
            self.assertNotEqual(first['pak_sha256'], second['pak_sha256'])
            self.assertEqual(json.loads((root / 'second.json').read_text())['parameters']['head_height'], 2.8)
            with zipfile.ZipFile(root / 'second.pak') as z:
                self.assertEqual(set(z.namelist()), set((*fp.CAMERAS, fp.OPACITY)))
                self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()))
                for name in fp.CAMERAS:
                    text = z.read(name).decode()
                    self.assertIn('y = 2.8', text)
                    self.assertIn('z = 0.3', text)
                    self.assertIn('distance = 0.0', text)
                    self.assertNotIn('transitions', text)
                    self.assertIn('untouched = { nested = { distance = 99 } }', text)
                    self.assertEqual(text.count('offsetRoot ='), 1)
                    self.assertTrue(text.endswith('__type = "CamBase"\n'))
                self.assertNotIn('STANDING_MODE', z.read(fp.OPACITY).decode())

    def test_invalid_parameters_and_export_layout_fail_closed(self):
        for value in (float('nan'), float('inf'), True, 10, '2.5'):
            with self.assertRaises(ValueError):
                fp.parameters({'head_height': value})
        with self.assertRaises(ValueError):
            fp.parameters({'typo': 1})
        for text in ('', '__type = "CamBase"\nx = 1\n', 'x = 1\nx = 2\n__type = "CamBase"\n'):
            with self.assertRaises(ValueError):
                fp.camera_patch(text, fp.parameters({}))


if __name__ == '__main__':
    unittest.main()
