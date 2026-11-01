import importlib.util
from pathlib import Path
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("viewer_flicker", ROOT / "scripts" / "viewer_flicker.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
H, W = module.H, module.W


def game_frames(n, edit=None):
    """n game frames, each captured twice (60 fps capture of a 30 fps game): a texture brightening smoothly."""
    x = np.arange(W, dtype=np.float32)
    frames = []
    for t in range(n):
        g = np.tile(40 + 0.4 * x + 0.5 * t, (H, 1))
        rgb = np.repeat(g[..., None], 3, axis=2)
        if edit:
            edit(t, rgb)
        frames += [rgb, rgb.copy()]
    return np.stack(frames)


class ViewerFlickerTest(unittest.TestCase):
    def test_repeated_capture_frames_count_once(self):
        gray = module.luma(game_frames(30))
        self.assertEqual(len(module.unique_frames(gray)), 30)

    def test_blip_on_a_still_view_is_not_taken_for_a_repeat(self):
        frames = np.repeat(np.tile(40 + 0.4 * np.arange(W, dtype=np.float32), (H, 1))[None, ..., None], 3, axis=3)
        still = np.repeat(frames, 20, axis=0).copy()
        still[10, 0:11, 0:20] += 60  # one cell lights for one frame, nothing else moves
        gray = module.luma(still)
        keep = module.unique_frames(gray)
        self.assertEqual(list(keep), [0, 10, 11])
        up, down, _ = module.blips(gray[keep])
        self.assertEqual(int(up.sum()), 1)

    def test_smooth_change_has_no_blips_or_flashes(self):
        rgb = game_frames(40)
        stats = module.analyse(rgb, [])
        self.assertEqual(stats["first person"]["blips"], 0)
        self.assertEqual(stats["first person"]["flashes"], [])

    def test_one_frame_light_on_and_off_are_blips(self):
        def edit(t, rgb):
            if t == 12:
                rgb[0:11, 0:20] += 60  # a light on for one game frame
            if t == 25:
                rgb[44:55, 100:120] -= 30  # a light off for one game frame
        gray = module.luma(game_frames(40, edit))
        up, down, _ = module.blips(gray[module.unique_frames(gray)])
        self.assertEqual(int(up.sum()), 1)
        self.assertEqual(int(down.sum()), 1)
        self.assertEqual(int(np.argmax(up)) + 1, 12)
        self.assertEqual(int(np.argmax(down)) + 1, 25)

    def test_lasting_change_is_not_a_blip(self):
        def edit(t, rgb):
            if t >= 12:
                rgb[0:11, 0:20] += 60  # a light that stays on
        gray = module.luma(game_frames(40, edit))
        up, down, _ = module.blips(gray[module.unique_frames(gray)])
        self.assertEqual(int(up.sum() + down.sum()), 0)

    def test_red_rectangle_for_one_game_frame_is_a_flash_and_lasting_red_is_not(self):
        def edit(t, rgb):
            if t == 20:
                rgb[0:40, 0:80, 0] = 140  # unlit red block
                rgb[0:40, 0:80, 1:] = 20
            if 30 <= t:
                rgb[60:, :, 0] = 150  # blood that stays
                rgb[60:, :, 1:] = 30
        rgb = game_frames(50, edit)
        found = module.flashes(module.red_excess(rgb))
        self.assertEqual([i // 2 for i in found], [20])

    def test_viewer_intervals_from_log(self):
        lines = [
            "[20:01:51.627] [I] [tid:1] VrViewer: state=releasing engaged=false",
            "[20:01:52.144] [I] [tid:1] VrViewer: state=active engaged=true",
            "[20:01:53.929] [I] [tid:1] VrViewer: state=active engaged=true",
            "[20:02:05.840] [I] [tid:1] VrViewer: state=off engaged=false",
            "[20:02:06.000] [I] [tid:1] OrdinaryHeadset: consumed=1",
            "[20:04:30.457] [I] [tid:1] VrViewer: state=active engaged=true",
        ]
        start = module.seconds(19, 52, 8)
        got = module.viewer_intervals(lines, start, end=900.0)
        self.assertEqual(len(got), 2)
        self.assertAlmostEqual(got[0][0], 584.144, places=3)
        self.assertAlmostEqual(got[0][1], 597.840, places=3)
        self.assertEqual(got[1][1], 900.0)

    def test_analyse_splits_camera_mode_from_first_person(self):
        def edit(t, rgb):
            if t in (8, 30):
                rgb[0:11, 0:20] += 60
        rgb = game_frames(40, edit)
        stats = module.analyse(rgb, [(0.5, 1.5)], offset=0.0)  # game frame 30 is at 1.0 s
        self.assertEqual(stats["camera mode"]["blips"], 1)
        self.assertEqual(stats["first person"]["blips"], 1)
        self.assertIn("camera mode", module.report(stats))


    def test_mirror_stream_splits_eyes_and_uses_its_clock(self):
        import tempfile
        def edit(t, rgb):
            if t == 10:
                rgb[0:11, 0:20] += 60  # left half only: a blip in the left eye
        left_eye = game_frames(30, edit)[::2].astype(np.uint8)  # the stream writes each new frame once
        right_eye = game_frames(30)[::2].astype(np.uint8)
        both = np.concatenate([left_eye, right_eye], axis=2)
        with tempfile.TemporaryDirectory() as d:
            prefix = Path(d) / "run"
            both.tofile(prefix.with_suffix(".rgb"))
            prefix.with_suffix(".json").write_text(f'{{"width": {2 * W}, "height": {H}, "eye_width": {W}, "downscale": 16, "format": "rgb24"}}')
            prefix.with_suffix(".times").write_text("".join(f"{i * 33.3:.1f} 20:05:{10 + i // 30:02d}.{(i % 30) * 33:03d}\n" for i in range(30)))
            left, right, times = module.load_mirror(prefix)
            # The chunked luma (for 1/8- and 1/4-size streams) matches converting the whole stream.
            luma_left, luma_right, luma_times = module.mirror_luma(prefix, chunk=7)
            np.testing.assert_allclose(luma_left, module.luma(left.astype(np.float32)), atol=1e-4)
            np.testing.assert_allclose(luma_right, module.luma(right.astype(np.float32)), atol=1e-4)
            np.testing.assert_array_equal(luma_times, times)
            del luma_left, luma_right
        self.assertEqual(left.shape, (30, H, W, 3))
        self.assertAlmostEqual(times[1] - times[0], 0.033, places=3)
        camera = [(module.seconds(20, 5, 0), module.seconds(20, 6, 0))]
        self.assertEqual(module.analyse(left.astype(np.float32), camera, times=times)["camera mode"]["blips"], 1)
        self.assertEqual(module.analyse(right.astype(np.float32), camera, times=times)["camera mode"]["blips"], 0)

    def test_one_eye_blips_leave_out_events_both_eyes_see(self):
        def scene(t, rgb, shift):
            if t == 8:
                rgb[22:33, 40 + shift:60 + shift] += 60  # a spark both eyes see, a little shifted
            if t == 20 and shift:
                rgb[55:66, 100:120] -= 30  # a light the right eye alone loses for one frame
        left = module.luma(game_frames(30, lambda t, rgb: scene(t, rgb, 0))[::2])
        right = module.luma(game_frames(30, lambda t, rgb: scene(t, rgb, 22))[::2])
        only_left, only_right, frames = module.one_eye_blips(left, right)
        self.assertEqual((only_left, only_right, frames), (0, 1, 28))
        self.assertEqual(module.one_eye_blips(left, left)[:2], (0, 0))

    def test_one_eye_changes_count_frames_where_only_one_eye_moved(self):
        still = np.repeat(module.luma(game_frames(1)[:1]), 21, axis=0)  # a still view, 21 captures
        right = still.copy()
        right[5, 0:11, 0:20] += 30  # the right eye alone changes at 5 and back at 6
        right[12:, 55:66, 100:120] -= 20  # and lastingly at 12
        left = still.copy()
        left[15:] += 5  # both change together at 15 (lighting both eyes see)
        right[15:] += 5
        times = np.arange(21) * 0.5
        self.assertEqual(module.one_eye_changes(left, right, times), (0, 3, 10.0))
        self.assertEqual(module.one_eye_changes(left, left, times)[:2], (0, 0))


if __name__ == "__main__":
    unittest.main()
