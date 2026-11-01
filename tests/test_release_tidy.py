"""Release tidy (docs/RELEASE_TIDY_REVIEW_2026-10-26.md): debug state and hooks the player build no
longer pays for. These read mod/src, because Sm2Probe's startup needs the game to run; the pure
parts have native tests (named per check)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'mod' / 'src' / 'Sm2Probe.cpp'


def function_body(text, signature):
    """The brace-balanced body of the first function whose definition starts with signature."""
    start = text.index(signature)
    open_brace = text.index('{', start)
    depth = 0
    for i in range(open_brace, len(text)):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        if depth == 0:
            return text[open_brace + 1:i]
    raise AssertionError(f'unbalanced braces after {signature}')


def guarded(text, needle, flag='m_status_detail'):
    """True if the code at needle sits under `flag`: on its own line, or in a block whose opening
    line names it. Format strings' braces are balanced, so counting braces backwards is safe."""
    pos = text.index(needle)
    lines = [text[text.rfind('\n', 0, pos) + 1:pos]]
    depth = 0
    for j in range(pos - 1, -1, -1):
        if text[j] == '}':
            depth += 1
        elif text[j] == '{':
            if depth:
                depth -= 1
            else:
                lines.append(text[text.rfind('\n', 0, j) + 1:j])
                if lines[-1].lstrip().startswith(('void Sm2Probe::', 'std::optional<std::string> Sm2Probe::')):
                    break
    return any(flag in line for line in lines)


# Per-object lines of the 5 s status: no script reads them from a plain run (see the review).
TRACE_ONLY = ['"BodyCandidate:', '"BodyRootTracker:', '"BodyRootWords:', '"WeaponAimRead:', '"WeaponAimScene:',
              '"WeaponTarget:', '"WeaponTrackerWords:', '"WeaponParent:', '"ArmLocalBone:', '"VrHandPose:',
              '"VrFingerCurl:', '"WorldMarkerCamera:', '"VrCamera:', 'log_matrix("inverse_projection"',
              'log_matrix("clip_to_previous"', 'log_matrix("previous_to_clip"', '"EngineCameraProbe: caller={:#x} second=']
# Status lines scripts/ reads from any run: run_report.py, summarize_session_rate.py, check_camera_log.py,
# check_output_size_run.py, analyze_performance_scene.py, check_game_build_run.py, grenade_aim_report.py,
# check_marker_log.py, observe_player_animation.py / observe_player_render_parts.py (ArmOwner).
ALWAYS = ['"Sm2Probe: presents=', '"CinematicState:', '"PoolReleaseGuard:', '"TrackerHandleGuard:', '"BodyProbe:',
          '"WeaponProbe:', '"WeaponAim:', '"ArmProbe:', '"ArmOwner:', '"VrSession: status=', '"RigidVelocity:',
          '"WorldMarkers:', '"WorldMarkerProjection:', '"StreamlineProbe: state=', '"StreamlineProbe: frame=',
          'log_matrix("projection"', '"EngineCameraProbe: state=', '"NativeCameraSource:',
          '"EngineCameraProbe: caller={:#x} calls=', '"VrProjection: state=', '"PlayerDistanceFade:']


class StatusDetail(unittest.TestCase):
    def setUp(self):
        self.text = PROBE.read_text(encoding='utf-8')

    def test_per_object_lines_need_trace(self):
        # 26 Oct review: 85% of the 5 s status was per-object dumps (279 arm bones, tracker words):
        # 490 lines in one burst on the present thread, 62 MB of log an hour.
        for needle in TRACE_ONLY:
            with self.subTest(needle=needle):
                self.assertTrue(guarded(self.text, needle), f'{needle} logs without SM2VR_TRACE')

    def test_lines_scripts_read_always_log(self):
        for needle in ALWAYS:
            with self.subTest(needle=needle):
                self.assertFalse(guarded(self.text, needle), f'{needle} is read by scripts/ and must log without trace')

    def test_detail_follows_the_trace_switch(self):
        body = function_body(self.text, 'std::optional<std::string> Sm2Probe::on_initialize()')
        self.assertRegex(body, r'm_status_detail=GetEnvironmentVariableA\("SM2VR_TRACE"')

    def test_run_report_reads_a_plain_run_like_a_traced_one(self):
        import importlib.util
        import sys
        import tempfile
        spec = importlib.util.spec_from_file_location('run_report', ROOT / 'scripts' / 'run_report.py')
        tool = importlib.util.module_from_spec(spec)
        sys.modules['run_report'] = tool
        spec.loader.exec_module(tool)
        plain = ('[20:00:05.000] [I] [tid:1] Sm2Probe: presents=200 fps=40.0 ui_draws=0\n'
                 '[20:00:05.000] [I] [tid:1] CinematicState: available=true scripted=false paused=false published=0x20000 requested=0x0 samples=2 failures=0 transitions=2\n'
                 '[20:00:05.000] [I] [tid:1] ArmProbe: updates=10 matched=10 publications=5 captures=5 rejected=0 pose_valid=true\n'
                 '[20:00:06.000] [E] [tid:1] OrdinaryHeadsetStopped: dispatch=10 packet_refusal=0 bloom_refusal=6 scene_calls=2\n')
        detail = ('[20:00:05.000] [I] [tid:1] ArmLocalBone: rig_index=1 skin_index=1 name=CENTRE parent=0 right=[1,0,0] up=[0,1,0] forward=[0,0,1] position=[0,0,0]\n'
                  '[20:00:05.000] [I] [tid:1] BodyRootWords: object=1 offset=0x0 words=[0,0,0,0]\n')
        reports = []
        with tempfile.TemporaryDirectory() as temp:
            for name, text in (('plain', plain), ('traced', plain.replace('\n', '\n' + detail, 3))):
                log = Path(temp) / f'{name}.log'
                log.write_text(text, encoding='utf-8')
                report = tool.read(log)
                reports.append(tool.describe(report, tool.load_table(tool.TABLE)).split('\n', 1)[1])  # minus the path line
        self.assertEqual(reports[0], reports[1])


class NoShadowIk(unittest.TestCase):
    def test_arm_probe_does_not_solve_for_a_log_line(self):
        # Every player pose publication (about 45/s, under the arm-probe lock) built 279 world
        # matrices and solved both arms toward a fixed offset, only for the ArmShadowIK line.
        probe = (ROOT / 'mod' / 'src' / 'EngineArmProbe.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'mod' / 'src' / 'EngineArmProbe.h').read_text(encoding='utf-8')
        for name, text in (('EngineArmProbe.cpp', probe), ('EngineArmProbe.h', header), ('Sm2Probe.cpp', PROBE.read_text(encoding='utf-8'))):
            with self.subTest(file=name):
                self.assertNotIn('shadow_solved', text)
                self.assertNotIn('ArmShadowIK', text)
        self.assertNotIn('solve_named(', probe)


class OpacityHooks(unittest.TestCase):
    def test_opacity_hooks_follow_sm2vr_player_opaque(self):
        # The setter, getter and draw hooks only act with SM2VR_PLAYER_OPAQUE=1 (the player profile
        # never sets it). The distance-fade pair is installed either way. Mechanism: native test
        # player_visibility ("distance fade installs without the opacity hooks").
        text = PROBE.read_text(encoding='utf-8')
        self.assertRegex(text, r'const bool keep_opaque=GetEnvironmentVariableA\("SM2VR_PLAYER_OPAQUE"')
        self.assertIn('EnginePlayerOpacity::get().install_in_game(GetModuleHandleW(nullptr),keep_opaque)', text)
        self.assertNotIn('SM2VR_PLAYER_OPAQUE', (ROOT / 'installer' / 'launch.json').read_text(encoding='utf-8'))


class ScreenUiHook(unittest.TestCase):
    def test_screen_ui_hook_only_for_eye_snapshots_or_the_diag_flag(self):
        # Its only consumers are its own status lines and the eye snapshots' state.json; the player
        # profile sets neither SM2VR_EYE_DUMP_DIR nor SM2VR_DIAG_SCREEN_UI.
        text = PROBE.read_text(encoding='utf-8')
        self.assertRegex(text, r'm_screen_ui_requested=!m_eye_dump_directory\.empty\(\) \|\|\s+\(GetEnvironmentVariableA\("SM2VR_DIAG_SCREEN_UI"')
        self.assertTrue(guarded(text, 'EngineScreenUi::get().install_in_game(', flag='m_screen_ui_requested'))
        self.assertTrue(guarded(text, '"ScreenUi: installed=', flag='m_screen_ui_requested'))
        self.assertTrue(guarded(text, '"ScreenUiMovie:', flag='m_screen_ui_requested'))
        launch = (ROOT / 'installer' / 'launch.json').read_text(encoding='utf-8')
        self.assertNotIn('SM2VR_DIAG_SCREEN_UI', launch)
        self.assertNotIn('SM2VR_EYE_DUMP_DIR', launch)


class OverlayAtStart(unittest.TestCase):
    def test_f11_overlay_starts_closed(self):
        # The framework starts with m_draw_ui=true and the launchers delete vr_config.txt, so the
        # panel was built and drawn every frame of every run until F11. The mod closes it once the
        # config is loaded (on_initialize_d3d_thread runs after Mods::reload_config) without saving.
        body = function_body(PROBE.read_text(encoding='utf-8'), 'std::optional<std::string> Sm2Probe::on_initialize_d3d_thread()')
        first = body.strip().splitlines()
        code = [line for line in first if not line.strip().startswith('//')]
        self.assertEqual(code[0].strip(), 'g_framework->set_draw_ui(false,false);', 'closed before anything can return early')

    def test_the_mod_does_not_swallow_f11(self):
        # F11 opens it again through the framework's own key handler; the mod's key filter must
        # leave the key alone (native check: tests/test_view_mode.cpp, "unrelated framework/menu keys").
        self.assertIn('VK_F11', (ROOT / 'tests' / 'test_view_mode.cpp').read_text(encoding='utf-8'))
        self.assertNotIn('VK_F11', PROBE.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
