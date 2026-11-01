from pathlib import Path
import json
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import compare_eye_snapshots as compare  # noqa: E402


def scene(width, height):
    """A deterministic 4K-like eye: gradients plus features that differ left to right."""
    x = np.linspace(0, 1, width)[None, :]
    y = np.linspace(0, 1, height)[:, None]
    grey = 0.5 + 0.25 * np.sin(14 * x + 3 * y) + 0.2 * (x > 0.7) - 0.15 * ((x - 0.3) ** 2 + (y - 0.6) ** 2 < 0.02)
    return Image.fromarray((np.clip(grey, 0, 1) * 255).astype(np.uint8)).convert('RGB')


class CompareEyeSnapshotsTests(unittest.TestCase):
    def setUp(self):
        self.reference = scene(3840, 2160)

    def test_whole_eye_matches_the_resize(self):
        test = self.reference.resize((1944, 2160), Image.BILINEAR)
        resized, cropped = compare.score(self.reference, test)
        self.assertLess(resized + 2.0, cropped)

    def test_zoomed_left_crop_is_detected(self):
        # Joe's run: the 1944-wide eye was the left 1944 columns of a 3840-wide image.
        test = self.reference.crop((0, 0, 1944, 2160))
        resized, cropped = compare.score(self.reference, test)
        self.assertLess(cropped, resized)

    def test_command_line_and_pose_check(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            self.reference.save(d / 'ref.ppm')
            self.reference.resize((1944, 2160)).save(d / 'good.ppm')
            self.reference.crop((0, 0, 1944, 2160)).save(d / 'zoomed.ppm')
            eye = {'world': [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.1, 6.7, 13.5, 1], 'projection': [0.77] + [0] * 15}
            moved = dict(eye, world=eye['world'][:12] + [0.2, 6.7, 13.5, 1])
            (d / 'a.json').write_text(json.dumps({'eyes': [eye, eye]}))
            (d / 'b.json').write_text(json.dumps({'eyes': [eye, eye]}))
            (d / 'c.json').write_text(json.dumps({'eyes': [moved, eye]}))
            self.assertEqual(compare.main([str(d / 'ref.ppm'), str(d / 'good.ppm'), '--pair-json-ref', str(d / 'a.json'), '--pair-json-test', str(d / 'b.json')]), 0)
            self.assertEqual(compare.main([str(d / 'ref.ppm'), str(d / 'zoomed.ppm')]), 1)
            self.assertEqual(compare.main([str(d / 'ref.ppm'), str(d / 'good.ppm'), '--pair-json-ref', str(d / 'a.json'), '--pair-json-test', str(d / 'c.json')]), 1)


if __name__ == '__main__':
    unittest.main()
