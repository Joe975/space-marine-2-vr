import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

scripts = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(scripts))


def load(name):
    spec = importlib.util.spec_from_file_location(name, scripts / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


parity = load('analyze_eye_parity')
capture = load('capture_eye_parity')

PROJECTION = [0.77, 0, 0, 0, 0, 0.71, 0, 0, 0.19, -0.001, 0, 1, 0, 0, 0.1, 0]


def world(x):
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 6.7, 13.8, 1]


def scene(size=256, seed=1):
    """Textured test image: a checker floor with a soft gradient, like the grid room."""
    y, x = np.mgrid[0:size, 0:size]
    checker = ((x // 16 + y // 16) % 2) * 40.0
    image = 80.0 + checker + 0.2 * y
    rgb = np.repeat(image[..., None], 3, axis=2)
    return rgb.astype(np.float32)


def write_pair(folder, left, right, separation=0.0005, right_projection=PROJECTION):
    folder.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.clip(left, 0, 255).astype(np.uint8)).save(folder / 'left.ppm')
    Image.fromarray(np.clip(right, 0, 255).astype(np.uint8)).save(folder / 'right.ppm')
    pair = {'dispatch': 7, 'eyes': [
        {'eye': 0, 'render_id': 6, 'world': world(0.0), 'projection': PROJECTION},
        {'eye': 1, 'render_id': 6, 'world': world(separation), 'projection': right_projection}]}
    (folder / 'pair.json').write_text(json.dumps(pair))


class EyeParityAnalysisTests(unittest.TestCase):
    def analyze(self, left, right, **pair):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory) / 'snap'
            write_pair(folder, left, right, **pair)
            return parity.analyze_folder(folder, write_heatmap=True), (folder / 'parity_diff.png').is_file()

    def test_identical_same_pose_images_match(self):
        result, heatmap = self.analyze(scene(), scene())
        self.assertTrue(result['pose']['collapsed'])
        self.assertEqual(result['verdict'], 'match')
        self.assertEqual(result['mean_abs'], 0)
        self.assertTrue(heatmap)

    def test_subblock_edge_shift_is_not_a_shading_mismatch(self):
        left = scene()
        right = np.concatenate([left[:, :2], left[:, :-2]], axis=1)  # TAA jitter / 0.5 mm offset moves edges by a few pixels
        result, _ = self.analyze(left, right)
        self.assertGreater(result['over_threshold'], 0.01, 'raw difference sees the shifted edges')
        self.assertEqual(result['shading_over'], 0)
        self.assertEqual(result['verdict'], 'match')

    def test_missing_shadow_in_one_slot_is_a_mismatch_and_located(self):
        left, right = scene(), scene()
        left[0:64, 192:256] -= 50  # a shadow only slot 0 shades, in the top-right tile
        result, _ = self.analyze(left, right)
        self.assertEqual(result['verdict'], 'mismatch')
        self.assertGreater(result['signed_mean'], 0, 'slot 1 is brighter where slot 0 has the shadow')
        self.assertEqual(result['worst_tiles'][0]['tile'], [3, 0])
        self.assertGreater(result['worst_tiles'][0]['signed_mean'], 40)

    def test_small_isolated_object_missing_from_one_slot_is_caught(self):
        left, right = scene(), scene()
        right[100:112, 100:112] = 250  # ~1% of blocks, like the culled console screens
        result, _ = self.analyze(left, right)
        self.assertEqual(result['verdict'], 'mismatch')

    def test_native_pose_pairs_are_flagged_parallax_not_scored(self):
        result, _ = self.analyze(scene(), scene(), separation=0.064)
        self.assertFalse(result['pose']['collapsed'])
        self.assertEqual(result['verdict'], 'parallax')
        mirrored = list(PROJECTION)
        mirrored[8] = -0.19
        result, _ = self.analyze(scene(), scene(), right_projection=mirrored)
        self.assertEqual(result['verdict'], 'parallax', 'different eye projections are not a same-pose pair')

    def test_size_mismatch_is_rejected(self):
        with self.assertRaises(ValueError):
            parity.compare(scene(256), scene(128))


class FarFieldAlignmentTests(unittest.TestCase):
    """Native-pose pairs: content at infinity must line up after align_far."""
    LEFT = [0.77, 0, 0, 0, 0, 0.71, 0, 0, 0.19, 0.001, 0, 1, 0, 0, 0.1, 0]
    RIGHT = [0.77, 0, 0, 0, 0, 0.71, 0, 0, -0.19, 0.0015, 0, 1, 0, 0, 0.1, 0]

    def render(self, projection, size=256, patch=None):
        """A sky-like function of view direction, drawn through one eye's projection."""
        ndc = (np.arange(size) + 0.5) / size * 2 - 1
        tx = (ndc - projection[8]) / projection[0]
        ty = ((ndc[::-1]) - projection[9]) / projection[5]
        x, y = np.meshgrid(tx, ty)
        sky = 120 + 60 * np.sin(3 * x) * np.cos(2 * y) + 30 * ((np.floor(x * 6) + np.floor(y * 6)) % 2)
        if patch:
            sky[(np.abs(x - patch[0]) < 0.1) & (np.abs(y - patch[1]) < 0.1)] += 60
        return np.repeat(sky[..., None], 3, axis=2).astype(np.float32)

    def pair(self):
        return {'eyes': [{'eye': 0, 'world': world(0.0), 'projection': self.LEFT},
                         {'eye': 1, 'world': world(0.064), 'projection': self.RIGHT}]}

    def test_far_content_aligns_across_asymmetric_projections(self):
        left, right = self.render(self.LEFT), self.render(self.RIGHT)
        raw, _ = parity.compare(left, right)
        aligned, valid = parity.align_far(right, self.pair())
        far, _ = parity.compare(left, aligned, valid=valid)
        self.assertGreater(raw['shading_over'], 0.05, 'unaligned eyes differ by the projection offset')
        self.assertLess(far['shading_over'], parity.MISMATCH_FRACTION)
        self.assertGreater(far['coverage'], 0.5)
        self.assertLess(far['coverage'], 1.0, 'the part only one eye sees is excluded')

    def test_far_effect_in_one_eye_only_is_detected(self):
        left, right = self.render(self.LEFT), self.render(self.RIGHT, patch=(0.0, 0.1))
        aligned, valid = parity.align_far(right, self.pair())
        far, _ = parity.compare(left, aligned, valid=valid)
        self.assertGreater(far['shading_over'], parity.MISMATCH_FRACTION)
        self.assertGreater(far['signed_mean'], 0)

    def test_rotated_eyes_are_not_aligned_by_the_affine_shortcut(self):
        pair = self.pair()
        rotated = world(0.064)
        rotated[0], rotated[2], rotated[8], rotated[10] = 0.99, 0.14, -0.14, 0.99
        pair['eyes'][1]['world'] = rotated
        self.assertIsNone(parity.align_far(self.render(self.RIGHT), pair))

    def test_native_pose_folder_reports_a_far_score(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory) / 'snap'
            write_pair(folder, self.render(self.LEFT), self.render(self.RIGHT), separation=0.064, right_projection=self.RIGHT)
            pair = json.loads((folder / 'pair.json').read_text())
            pair['eyes'][0]['projection'] = self.LEFT
            (folder / 'pair.json').write_text(json.dumps(pair))
            result = parity.analyze_folder(folder)
            self.assertEqual(result['verdict'], 'parallax')
            self.assertLess(result['far']['shading_over'], parity.MISMATCH_FRACTION)


class EyeParityPlanTests(unittest.TestCase):
    def test_default_plan_fits_the_driver_and_snapshot_limits(self):
        scenario, schedule = capture.build_plan()
        self.assertLessEqual(scenario['duration_ms'], 120000)
        self.assertEqual(len(schedule), 16)
        self.assertLessEqual(len(schedule), capture.MAX_SNAPSHOTS)
        self.assertEqual(len({slot['name'] for slot in schedule}), 16)
        capture.driver.compile_script(scenario, 'eye_parity_test')

    def test_every_variant_is_captured_while_its_station_is_held(self):
        scenario, schedule = capture.build_plan()
        frames = scenario['frames']
        for slot in schedule:
            held = [f for f in frames if f['head'][3:5] == [slot['yaw'], slot['pitch']]]
            self.assertEqual(len(held), 2)
            self.assertLessEqual(held[0]['ms'], slot['apply_ms'])
            self.assertLess(slot['apply_ms'], slot['request_ms'])
            self.assertLessEqual(slot['request_ms'] + 2000, slot['end_ms'], 'snapshot poll (1 s) needs room')
            self.assertLessEqual(slot['end_ms'], held[1]['ms'])
        for frame in frames:
            self.assertLessEqual(frame['left'][1], -0.5, 'hands stay lowered out of the view')

    def test_baseline_precedes_refresh_at_each_station(self):
        _, schedule = capture.build_plan()
        for station in {slot['station'] for slot in schedule}:
            modes = [slot['mode'] for slot in schedule if slot['station'] == station]
            self.assertLess(modes.index('light_union'), modes.index('light_union_shadow_refresh'))

    def test_invalid_plans_are_refused_before_queueing(self):
        with self.assertRaises(ValueError):
            capture.build_plan(variants=(('left', 'bogus'),))
        with self.assertRaises(ValueError):
            capture.build_plan(variants=(('sideways', 'light_union'),))
        with self.assertRaises(ValueError):
            capture.build_plan(stations=tuple((f's{i}', 0, 0) for i in range(7)))
        with self.assertRaises(ValueError):
            capture.build_plan(stations=(('a', 0, 0), ('a', 10, 0)))

    def test_run_applies_controls_requests_snapshots_and_restores(self):
        scenario, schedule = capture.build_plan(stations=(('fwd', 0, 5),),
                                                variants=(('left', 'light_union'), ('native', 'light_union_shadow_refresh')))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dump, mailbox = root / 'dump', root / 'driver'
            dump.mkdir()
            mailbox.mkdir()
            (dump / 'shading.txt').write_text('light_union\n')
            (dump / 'eye_source.txt').write_text('native\n')
            status = mailbox / 'status.json'
            seen = []
            state = {'elapsed': 0, 'now': 0.0}

            class FakeClock:
                @staticmethod
                def monotonic():
                    return state['now']

                @staticmethod
                def sleep(seconds):
                    state['now'] += seconds
                    request = dump / 'request.txt'
                    if request.exists():
                        name = request.read_text().strip()
                        request.unlink()
                        seen.append((name, (dump / 'eye_source.txt').read_text().strip(),
                                     (dump / 'shading.txt').read_text().strip()))
                        write_pair(dump / name, scene(64), scene(64))
                    state['elapsed'] += 250
                    done = state['elapsed'] > scenario['duration_ms']
                    status.write_text(json.dumps({'id': 'parity_test', 'state': 'completed' if done else 'running',
                                                  'elapsed_ms': state['elapsed']}))

            status.write_text(json.dumps({'id': 'parity_test', 'state': 'preroll', 'elapsed_ms': 0}))
            ok, receipt = capture.run(dump, mailbox, scenario, schedule, clock=FakeClock, request_id='parity_test')
            self.assertTrue(ok)
            self.assertTrue((mailbox / 'request.txt').read_text().startswith('SM2VR_TEST 1 parity_test scripted_head'))
            self.assertEqual(seen, [('fwd_left_union', 'left', 'light_union'),
                                    ('fwd_native_refresh', 'native', 'light_union_shadow_refresh')])
            self.assertEqual((dump / 'shading.txt').read_text().strip(), 'light_union', 'original mode restored')
            self.assertEqual((dump / 'eye_source.txt').read_text().strip(), 'native')
            results = capture.score(dump, receipt)
            self.assertEqual([r['verdict'] for r in results], ['match', 'match'])
            self.assertTrue((dump / 'eye_parity.json').is_file())
            with Image.open(dump / 'eye_parity_sheet.png') as sheet:
                self.assertEqual(sheet.size[0], 3 * 360)

    def test_tone_variants_write_tone_source_and_restore_it(self):
        scenario, schedule = capture.build_plan(stations=(('r75', -75, 0),),
                                                variants=(('left', 'light_union_shadow_refresh', 'native'),
                                                          ('left', 'light_union_shadow_refresh', 'bloom')))
        self.assertEqual([slot['name'] for slot in schedule], ['r75_left_refresh_tnative', 'r75_left_refresh_tbloom'])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dump, mailbox = root / 'dump', root / 'driver'
            dump.mkdir()
            mailbox.mkdir()
            (dump / 'shading.txt').write_text('light_union\n')
            (dump / 'eye_source.txt').write_text('native\n')
            with self.assertRaises(SystemExit):
                capture.run(dump, mailbox, scenario, schedule)
            (dump / 'tone_source.txt').write_text('native\n')
            seen, state = [], {'elapsed': 0, 'now': 0.0}
            status = mailbox / 'status.json'

            class FakeClock:
                @staticmethod
                def monotonic():
                    return state['now']

                @staticmethod
                def sleep(seconds):
                    state['now'] += seconds
                    request = dump / 'request.txt'
                    if request.exists():
                        name = request.read_text().strip()
                        request.unlink()
                        seen.append((name, (dump / 'tone_source.txt').read_text().strip()))
                        write_pair(dump / name, scene(64), scene(64))
                    state['elapsed'] += 250
                    done = state['elapsed'] > scenario['duration_ms']
                    status.write_text(json.dumps({'id': 'parity_tone', 'state': 'completed' if done else 'running',
                                                  'elapsed_ms': state['elapsed']}))

            status.write_text(json.dumps({'id': 'parity_tone', 'state': 'preroll', 'elapsed_ms': 0}))
            ok, receipt = capture.run(dump, mailbox, scenario, schedule, clock=FakeClock, request_id='parity_tone')
            self.assertTrue(ok)
            self.assertEqual(seen, [('r75_left_refresh_tnative', 'native'), ('r75_left_refresh_tbloom', 'bloom')])
            self.assertEqual((dump / 'tone_source.txt').read_text().strip(), 'native', 'original tone source restored')
            self.assertEqual([r['tone'] for r in capture.score(dump, receipt)], ['native', 'bloom'])
        with self.assertRaises(ValueError):
            capture.build_plan(variants=(('left', 'light_union', 'glow'),))

    def test_run_refuses_without_eye_source_control_or_with_used_names(self):
        scenario, schedule = capture.build_plan(stations=(('fwd', 0, 5),), variants=(('left', 'light_union'),))
        with tempfile.TemporaryDirectory() as directory:
            dump = Path(directory)
            (dump / 'shading.txt').write_text('light_union\n')
            with self.assertRaises(SystemExit):
                capture.run(dump, dump, scenario, schedule)
            (dump / 'eye_source.txt').write_text('native\n')
            (dump / 'fwd_left_union').mkdir()
            with self.assertRaises(SystemExit):
                capture.run(dump, dump, scenario, schedule)
            self.assertFalse((dump / 'request.txt').exists(), 'nothing is queued after a refusal')


if __name__ == '__main__':
    unittest.main()
