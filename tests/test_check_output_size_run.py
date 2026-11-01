from pathlib import Path
import json
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import check_output_size_run as checker  # noqa: E402


def log(swap, mvec):
    return [
        f'[13:04:38.406] [I] [tid:1] Sm2Probe: d3d thread init: swap chain 3840x2160 format=28 buffers=3',
        f'[13:04:42.275] [I] [tid:1] Sm2Probe: first present: swap chain {swap[0]}x{swap[1]} format=28 buffers=3',
        '[13:04:42.275] [I] [tid:1] Sm2Probe: first present: swap chain scaling=0 alpha=0 window client 3840x2160',
        # Title-screen frame (viewport 0), then a headset frame; the headset frame decides.
        '[13:05:00.000] [I] [tid:1] StreamlineProbe: frame=10 viewport=0 version=2 result=0 mvec=[-0.0005208333,-0.0009259259] near=0.1',
        f'[13:06:00.000] [I] [tid:1] StreamlineProbe: frame=99 viewport=1397568001 version=2 result=0 age=0.004s jitter=[0.1,-0.3] mvec=[{-1 / mvec[0]},{-1 / mvec[1]}] near=0.1',
    ]


def handoff(directory, copy, colour):
    (directory / 'headset_handoff_1.json').write_text(json.dumps({
        'colour': {'events': [{'rtvs': [{'width': colour[0], 'height': colour[1], 'format': 28},
                                        {'width': colour[0], 'height': colour[1], 'format': 29}]}]},
        'pairs': [{'eyes': [{'copy': {'width': copy[0], 'height': copy[1]}}, {'copy': {'width': copy[0], 'height': copy[1]}}]}]}))


class CheckOutputSizeRunTests(unittest.TestCase):
    def run_check(self, swap, copy, colour, mvec, size):
        with tempfile.TemporaryDirectory() as d:
            handoff(Path(d), copy, colour)
            return {name: ok for name, ok, _ in checker.check(log(swap, mvec), Path(d), size)}

    def test_joes_zoomed_run_fails_on_colour_targets_and_input_aspect(self):
        # 21 Oct campaign, device mode: 1944x2160 swap chain and copies, renderer still at 4K.
        result = self.run_check((1944, 2160), (1944, 2160), (3840, 2160), (1280, 720), (1944, 2160))
        self.assertEqual(result, {'swap chain': True, 'eye copies': True, 'colour targets': False, 'upscaler input': False})

    def test_whole_eye_at_the_requested_size_passes(self):
        result = self.run_check((1944, 2160), (1944, 2160), (1944, 2160), (648, 720), (1944, 2160))
        self.assertTrue(all(result.values()), result)

    def test_plain_4k_run_passes(self):
        result = self.run_check((3840, 2160), (3840, 2160), (3840, 2160), (1280, 720), (3840, 2160))
        self.assertTrue(all(result.values()), result)

    def test_swap_chain_or_copy_mismatch_fails(self):
        self.assertFalse(self.run_check((3840, 2160), (1944, 2160), (1944, 2160), (648, 720), (1944, 2160))['swap chain'])
        self.assertFalse(self.run_check((1944, 2160), (3840, 2160), (1944, 2160), (648, 720), (1944, 2160))['eye copies'])

    def test_missing_evidence_fails(self):
        with tempfile.TemporaryDirectory() as d:
            result = {name: ok for name, ok, _ in checker.check([], Path(d), (1944, 2160))}
        self.assertFalse(any(result.values()), result)

    def test_input_aspect_tolerance(self):
        self.assertTrue(self.run_check((1944, 2160), (1944, 2160), (1944, 2160), (1296, 1440), (1944, 2160))['upscaler input'])
        self.assertFalse(self.run_check((1944, 2160), (1944, 2160), (1944, 2160), (1296, 1300), (1944, 2160))['upscaler input'])

    def test_size_argument_is_strict(self):
        self.assertEqual(checker.parse_size('1944x2160'), (1944, 2160))
        for bad in ('1944X2160', '1944', ' 1944x2160', '01944x2160'):
            with self.assertRaises(Exception):
                checker.parse_size(bad)


if __name__ == '__main__':
    unittest.main()
