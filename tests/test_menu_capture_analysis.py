import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("menu_capture_analysis", Path(__file__).resolve().parents[1]/"scripts/analyze_menu_capture.py")
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


def image(box, extra=False):
    width, height = 160, 100
    pixels = bytearray(width*height*3)
    def rectangle(bounds):
        left, top, right, bottom = bounds
        for y in range(top, bottom):
            for x in range(left, right):
                if y in (top, bottom-1) or x in (left, right-1):
                    pixels[(y*width+x)*3:(y*width+x)*3+3] = b"\xff\xe0\xa0"
    rectangle(box)
    if extra:
        rectangle([20, 60, 100, 90])
    return width, height, bytes(pixels)


class MenuCaptureAnalysisTests(unittest.TestCase):
    def test_detection_measures_shift_and_rejects_ambiguous_or_absent_menu(self):
        self.assertEqual(analysis.selected_panel(image([40, 20, 120, 50]), [0, 0, 160, 100], 60, 100), [40, 20, 120, 50])
        width, height, pixels = image([40, 20, 120, 50])
        dim = (width, height, pixels.replace(b"\xff\xe0\xa0", b"\x5a\x46\x2a"))
        self.assertEqual(analysis.selected_panel(dim, [0, 0, 160, 100], 60, 100), [40, 20, 120, 50])
        with self.assertRaises(ValueError):
            analysis.selected_panel(image([40, 20, 120, 50], True), [0, 0, 160, 100], 60, 100)
        with self.assertRaises(ValueError):
            analysis.selected_panel((160, 100, bytes(160*100*3)), [0, 0, 160, 100], 60, 100)

    def test_real_analysis_path_fails_left_shift_and_passes_centred_pair(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1]/"build") as folder:
            root = Path(folder)
            projection = [0.0]*16
            projection[0] = projection[5] = 1.0
            eyes = [{"eye": i, "render_id": 40+i, "width": 160, "height": 100, "projection": projection} for i in range(2)]
            (root/"pair.json").write_text(json.dumps({"eyes": eyes}))
            for box, expected in (([0, 20, 80, 50], False), ([40, 20, 120, 50], True)):
                for name in ("left", "right"):
                    width, height, pixels = image(box)
                    (root/(name+".ppm")).write_bytes(f"P6\n{width} {height}\n255\n".encode()+pixels)
                result = analysis.analyze(root, [0, 0, 160, 100], 60, 100, 3)
                self.assertEqual(result["texture_centred"], expected)
                self.assertTrue((root/"left.png").read_bytes().startswith(b"\x89PNG"))
            eyes[1]["render_id"] += 1
            (root/"pair.json").write_text(json.dumps({"eyes": eyes}))
            with self.assertRaises(ValueError):
                analysis.analyze(root, [0, 0, 160, 100], 60, 100, 3)

    def test_projection_offset_is_measured_separately_from_texture_centre(self):
        projection = [0.0]*16
        projection[0] = projection[5] = 1.0
        projection[8] = .2
        result = analysis.ray_angles({"width": 100, "height": 100, "projection": projection}, [40, 40, 60, 60])
        self.assertEqual(result["texture_offset_percent"], [0, 0])
        self.assertAlmostEqual(result["eye_ray_degrees"][0], -11.309932474)

    def test_stereo_check_requires_convergence_instead_of_identical_texture_centres(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1]/"build") as folder:
            root = Path(folder)
            eyes = []
            for i in range(2):
                projection = [0.0]*16
                projection[0] = projection[5] = 1.0
                projection[8] = -.2 if i else .2
                eye = [0.0]*16
                eye[0] = eye[5] = eye[10] = eye[15] = 1.0
                eye[12] = .032 if i else -.032
                eyes.append({"eye": i, "render_id": 10+i, "width": 160, "height": 100, "projection": projection,
                             "eye_to_head": eye, "screen_menu": True, "menu_distance_metres": 2})
            (root/"pair.json").write_text(json.dumps({"eyes": eyes}))
            for boxes, expected in (([[57, 20, 137, 50], [23, 20, 103, 50]], True),
                                    ([[40, 20, 120, 50], [40, 20, 120, 50]], False)):
                for name, box in zip(("left", "right"), boxes):
                    width, height, pixels = image(box)
                    (root/(name+".ppm")).write_bytes(f"P6\n{width} {height}\n255\n".encode()+pixels)
                result = analysis.analyze(root, [0, 0, 160, 100], 60, 100, 3)
                self.assertEqual(result["accepted"], expected)
