"""The wiring of a swap-chain reset in Sm2Probe.cpp, which needs the whole mod and so has no native
test. 31 Oct 19:00 (docs/FREEZE_REHOOK_2026-10-27.md): each piece below was fine alone; what failed was
what on_device_reset and the re-arm did or left out. The fixture cases gpu_ordinary_pair_retention
reset_kept and reset_forgotten show what these lines do."""
from pathlib import Path
import re
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / 'mod/src/Sm2Probe.cpp').read_text(encoding='utf-8')


def body(signature):
    """The text of a top-level function: from its signature to the closing brace in column 0."""
    start = SOURCE.index(signature)
    return SOURCE[start:re.compile(r'^}', re.M).search(SOURCE, start).start()]


class DeviceResetWiring(unittest.TestCase):
    def test_reset_keeps_the_render_target_views(self):
        reset = body('void Sm2Probe::on_device_reset()')
        self.assertIn('m_render_targets.device_reset();', reset)
        self.assertNotIn('m_render_targets.clear()', reset, 'clearing it refuses every later eye (hazards=32)')

    def test_rearm_lifts_what_the_reset_poisoned(self):
        rearm = body('void Sm2Probe::rearm_ordinary_headset()')
        steps = ['m_ordinary_pair_retention.restart(', 'NativeOrdinaryPacket::get().restart()',
                 'StreamlineHistoryProbe::get().restart_private()', 'NativeTrackedOrdinaryCamera::get().restart()']
        at = [rearm.find(step) for step in steps]
        self.assertNotIn(-1, at, 'without restart_private the first pair is refused (packet refusal 9)')
        self.assertEqual(at, sorted(at), 'the producer starts last, as in the cold restart')

    def test_cold_restart_waits_for_dlss(self):
        cold = body('void Sm2Probe::cold_restart_ordinary_headset()')
        self.assertRegex(cold, r'm_cold_restart\.observe\([^;]*OrdinaryDlssPresence::get\(\)\.held\(\)', 'a restart without DLSS only spends the budget')

    def test_the_hold_is_fed_and_reported(self):
        self.assertIn('set_dlss_gate([]{return OrdinaryDlssPresence::get().held();})', SOURCE)
        feed = SOURCE.index('OrdinaryDlssPresence::get().complete(')
        self.assertLess(SOURCE.rindex('NativeCallCensus::get().pending(NativeCallCensus::Hook::sdk_evaluate)', 0, feed),
                        SOURCE.rindex('NativeCallCensus::get().complete(', 0, feed), 'read the job\'s evaluations before complete() takes them')
        self.assertIn('report_ordinary_dlss();', body('void Sm2Probe::capture_ordinary_pair(bool after_dispatch)'))

    def test_the_window_reads_the_lines_the_mod_writes(self):
        launch = (Path(__file__).resolve().parents[1] / 'installer/app/sm2-launch.ps1').read_text(encoding='utf-8')
        for line in ('OrdinaryDlssHold: held=true', 'OrdinaryDlssHold: held=false'):
            self.assertIn(line, SOURCE)
            self.assertIn(line, launch)


if __name__ == '__main__':
    unittest.main()
