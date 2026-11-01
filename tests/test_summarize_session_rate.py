from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from summarize_session_rate import suggest_resolution, summarize  # noqa: E402

LOG = '''[20:13:56.368] [I] [tid:1] Sm2Probe: d3d thread init: swap chain 3840x2160 format=28 buffers=3
[20:14:01.384] [I] [tid:1] VrSession: status=OpenXR runtime: SteamVR/OpenXR; waiting for session readiness epoch=5 submitted_pairs=0 per_eye_hz=0.0 failed_transfers=0 last_render_id=0 oldest_pose_age_ms=0.00 submit_cpu_ms=0.00
[20:19:12.895] [I] [tid:1] VrSession: status=OpenXR focused epoch=5 submitted_pairs=86 per_eye_hz=20.0 failed_transfers=0 last_render_id=1 oldest_pose_age_ms=140.00 submit_cpu_ms=0.10
[20:19:17.895] [I] [tid:1] VrSession: status=OpenXR focused epoch=5 submitted_pairs=186 per_eye_hz=30.0 failed_transfers=0 last_render_id=2 oldest_pose_age_ms=120.00 submit_cpu_ms=0.10
[20:19:20.000] [I] [tid:1] Sm2Probe: device reset: swap chain 2560x1440 format=28 buffers=3
[20:19:22.895] [I] [tid:1] VrSession: status=OpenXR focused epoch=5 submitted_pairs=386 per_eye_hz=40.0 failed_transfers=0 last_render_id=3 oldest_pose_age_ms=100.00 submit_cpu_ms=0.10
[20:19:27.895] [I] [tid:1] ArmLocalBone: per_eye_hz=99.0 not a session line
'''.splitlines()


class SummarizeSessionRateTests(unittest.TestCase):
    def test_rates_exclude_idle_seconds_and_report_last_swap_chain(self):
        result = summarize(LOG)
        self.assertEqual(result['samples'], 3)
        self.assertEqual(result['idle_samples'], 1)
        self.assertEqual(result['pairs_per_second']['median'], 30.0)
        self.assertEqual(result['pairs_per_second']['min'], 20.0)
        self.assertEqual(result['pairs_per_second']['max'], 40.0)
        self.assertEqual(result['pose_age_ms']['median'], 120.0)
        self.assertEqual((result['swap_chain']['width'], result['swap_chain']['height']), (2560, 1440))
        self.assertEqual(result['swap_chain']['reason'], 'device reset')

    def test_time_window(self):
        result = summarize(LOG, start='20:19:15', end='20:19:23')
        self.assertEqual(result['samples'], 2)
        self.assertEqual(result['pairs_per_second']['median'], 35.0)

    def test_runtime_recommended_eye_and_window_client(self):
        lines = [
            '[08:00:00.000] [I] [tid:1] VrSession: status=OpenXR runtime: SteamVR/OpenXR; recommended eye 2016x2240 (max 4096x4096); waiting for session readiness epoch=1 submitted_pairs=0 per_eye_hz=0.0 failed_transfers=0 last_render_id=0 oldest_pose_age_ms=0.00 submit_cpu_ms=0.00',
            '[08:00:01.000] [I] [tid:1] Sm2Probe: first present: swap chain 1944x2160 format=28 buffers=3',
            '[08:00:01.000] [I] [tid:1] Sm2Probe: first present: swap chain scaling=0 alpha=0 window client 1280x720',
        ]
        result = summarize(lines)
        self.assertEqual(result['runtime_recommended_eye'],
                         {'width': 2016, 'height': 2240, 'max_width': 4096, 'max_height': 4096, 'suggested_resolution': '1944x2160'})
        self.assertEqual(result['swap_chain_window'],
                         {'reason': 'first present', 'scaling': 0, 'alpha': 0, 'client_width': 1280, 'client_height': 720})
        self.assertEqual((result['swap_chain']['width'], result['swap_chain']['height']), (1944, 2160))
        self.assertEqual(result['idle_samples'], 1, 'the recommended size does not hide the status line')

    def test_suggest_resolution_keeps_aspect_within_caps(self):
        self.assertEqual(suggest_resolution(2016, 2240), '1944x2160')
        self.assertEqual(suggest_resolution(1832, 1920), '1832x1920', 'fits already: unchanged')
        self.assertEqual(suggest_resolution(4400, 2200), '3840x1920')
        self.assertEqual(suggest_resolution(1833, 1921), '1832x1920', 'even dimensions')
        self.assertIsNone(suggest_resolution(0, 2240))

    def test_no_submissions(self):
        result = summarize(LOG[:2])
        self.assertEqual(result['samples'], 0)
        self.assertNotIn('pairs_per_second', result)
        self.assertEqual(result['swap_chain']['width'], 3840)


if __name__ == '__main__':
    unittest.main()
