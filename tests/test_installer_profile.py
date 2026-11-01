"""The installer's play profile must set exactly the environment the dev launcher sets for the
same switches, so a player gets what headset playtests got."""
from pathlib import Path
import json
import tempfile
import unittest

from installer_support import FIXTURES, REPO, TEMP, run_ps, run_ps_json

BASE = json.loads((REPO / 'installer' / 'launch.json').read_text(encoding='utf-8'))

# Profile change -> the run-ordinary-headset.ps1 switches that select the same thing.
CASES = {
    'shipped': ({}, []),
    'openvr': ({'runtime': 'OpenVR'}, ['-Runtime', 'OpenVR']),
    'no_gestures': ({'gestures': False, 'grenade_aim': False}, ['-NoGestures']),
    'haptics_gain': ({'haptics_gain': 0.5}, ['-HapticsGain', '0.5']),
    'trace_and_native_shadows': ({'trace': True, 'native_shadows': True}, ['-Trace', '-NativeShadows']),
    'guard_guide': ({'guard_guide': True}, ['-GuardGuide']),
    'photo_native_pose': ({'photo_native_pose': True}, ['-PhotoNativePose']),
    'thumbs_up_photo': ({'thumbs_up': 'photo'}, ['-ThumbsUp', 'photo']),
    'thumbs_up_off': ({'thumbs_up': 'off'}, ['-ThumbsUp', 'off']),
    'opt_outs': ({'chainsword_rev': False, 'gaze_camera': False, 'grenade_aim': False,
                  'compact_hud': False, 'upright_torso': False, 'keep_head_visible': True, 'haptics': False},
                 ['-NoChainswordRev', '-NoGazeCamera', '-NoGrenadeAim', '-NoCompactHud',
                  '-NoUprightTorso', '-KeepHeadVisible', '-NoHaptics']),
}


def dev_switches(extra):
    # The HUD and upright torso come with -Controls (27 Oct); the launcher's opt-outs pass through.
    switches = ['-Controls'] + ([] if '-NoGestures' in extra else ['-Gestures'])
    return switches + [s for s in extra if s != '-NoGestures']


class ProfileParityTests(unittest.TestCase):
    def test_plain_controls_gestures_launch_is_what_players_get(self):
        # The drift that cost three playtests their HUD (25-27 Oct): a plain `-Controls -Gestures`
        # launch, with no other switch, must set exactly the shipped installer/launch.json environment.
        TEMP.mkdir(parents=True, exist_ok=True)
        dev = run_ps_json(REPO / 'scripts' / 'run-ordinary-headset.ps1', '-Controls', '-Gestures', '-Tag', 'shipped', '-PrintEnv')
        shipped = run_ps_json(FIXTURES / 'profile_env.ps1', REPO / 'installer' / 'launch.json', REPO / 'config',
                              dev['SM2VR_ORDINARY_DUMP_DIR'])
        self.assertEqual(dict(sorted(shipped['env'].items())), dev)
        for name in ('SM2VR_NATIVE_HUD', 'SM2VR_UPRIGHT_TORSO', 'SM2VR_GAZE_CAMERA', 'SM2VR_GRENADE_AIM', 'SM2VR_HAPTICS'):
            self.assertEqual(dev.get(name), '1', name)

    def test_every_player_toggle_has_a_parity_case(self):
        # A launch.json key without a case here could change the installer's env with no launcher switch to match.
        flipped = {key for changes, _ in CASES.values() for key in changes}
        self.assertEqual(set(BASE) - {'schema', 'controls', 'snap_degrees'} - flipped, set())

    def test_profile_matches_dev_launcher(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        for name, (changes, extra) in CASES.items():
            with self.subTest(name), tempfile.TemporaryDirectory(dir=TEMP) as work:
                dev = run_ps_json(REPO / 'scripts' / 'run-ordinary-headset.ps1', *dev_switches(extra),
                                  '-Tag', 'installer_parity', '-PrintEnv')
                profile = Path(work) / 'launch.json'
                profile.write_text(json.dumps({**BASE, **changes}), encoding='utf-8')
                ours = run_ps_json(FIXTURES / 'profile_env.ps1', profile, REPO / 'config',
                                   dev['SM2VR_ORDINARY_DUMP_DIR'])
                self.assertEqual(dict(sorted(ours['env'].items())), dev)
                shading = Path(dev['SM2VR_ORDINARY_SHADING_CONTROL']).read_text(encoding='ascii').strip()
                self.assertEqual(ours['files'][dev['SM2VR_ORDINARY_SHADING_CONTROL']], shading)

    def test_chase_view_matches_dev_launcher_chase_cam(self):
        # The chase play button must set what -ChaseCam playtests got: no motion controls or gestures,
        # and the game's HUD unless the profile turns it off.
        TEMP.mkdir(parents=True, exist_ok=True)
        for name, (changes, extra) in {'shipped': ({}, []),
                                       'openvr': ({'runtime': 'OpenVR'}, ['-Runtime', 'OpenVR']),
                                       'trace': ({'trace': True}, ['-Trace']),
                                       'no_hud': ({'compact_hud': False}, ['-NoCompactHud'])}.items():
            with self.subTest(name), tempfile.TemporaryDirectory(dir=TEMP) as work:
                dev = run_ps_json(REPO / 'scripts' / 'run-ordinary-headset.ps1', '-ChaseCam', *extra,
                                  '-Tag', 'installer_parity', '-PrintEnv')
                profile = Path(work) / 'launch.json'
                profile.write_text(json.dumps({**BASE, **changes}), encoding='utf-8')
                ours = run_ps_json(FIXTURES / 'profile_env.ps1', profile, REPO / 'config',
                                   dev['SM2VR_ORDINARY_DUMP_DIR'], '-Chase')
                self.assertEqual(dict(sorted(ours['env'].items())), dev)
                self.assertEqual(ours['env']['SM2VR_CHASE_CAM'], '1')
                self.assertNotIn('SM2VR_CONTROLLERS', ours['env'])
                self.assertEqual(ours['env'].get('SM2VR_NATIVE_HUD'), None if name == 'no_hud' else '1')

    def test_env_off_removes_only_the_named_variables(self):
        # Diagnostic bisection (-EnvOff): the launch is otherwise the normal one.
        TEMP.mkdir(parents=True, exist_ok=True)
        script = REPO / 'scripts' / 'run-ordinary-headset.ps1'
        full = run_ps_json(script, '-Controls', '-Gestures', '-Tag', 'env_off', '-PrintEnv')
        less = run_ps_json(script, '-Controls', '-Gestures', '-Tag', 'env_off', '-PrintEnv',
                           '-EnvOff', 'SM2VR_STEREO, SM2VR_ORDINARY_NATIVE_CREATE')
        self.assertEqual({k: v for k, v in full.items() if k not in ('SM2VR_STEREO', 'SM2VR_ORDINARY_NATIVE_CREATE')}, less)
        self.assertEqual((full['SM2VR_STEREO'], full['SM2VR_ORDINARY_NATIVE_CREATE']), ('1', '1'))
        refused = run_ps(script, '-PrintEnv', '-EnvOff', 'PATH')
        self.assertNotEqual(refused.returncode, 0, 'only SM2VR_* names may be removed')
        more = run_ps_json(script, '-Controls', '-Gestures', '-Tag', 'env_off', '-PrintEnv', '-EnvSet', 'SM2VR_DIAG_NO_NATIVE_REFERENCE=1')
        self.assertEqual(more, {**full, 'SM2VR_DIAG_NO_NATIVE_REFERENCE': '1'})
        self.assertNotEqual(run_ps(script, '-PrintEnv', '-EnvSet', 'PATH=x').returncode, 0, 'only SM2VR_* names may be set')

    def test_inconsistent_profiles_are_refused(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        for changes in ({'controls': False}, {'gestures': False}, {'runtime': 'openxr'}, {'snap_degrees': 0}, {'haptics_gain': 3},
                        {'thumbs_up': 'Photo'}, {'thumbs_up': 'photo', 'controls': False, 'gestures': False, 'grenade_aim': False,
                         'upright_torso': False, 'compact_hud': False, 'haptics': False}):
            with self.subTest(changes), tempfile.TemporaryDirectory(dir=TEMP) as work:
                profile = Path(work) / 'launch.json'
                profile.write_text(json.dumps({**BASE, **changes}), encoding='utf-8')
                result = run_ps(FIXTURES / 'profile_env.ps1', profile, REPO / 'config', work)
                self.assertNotEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__':
    unittest.main()
