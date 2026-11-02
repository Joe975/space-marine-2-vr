from pathlib import Path
import subprocess
import shutil
import os
import tempfile
import unittest
import json
from pwsh import pwsh


class LauncherUiModeTests(unittest.TestCase):
    def test_single_frame_runtime_selection_is_exclusive_and_preserves_render_path(self):
        source=Path(__file__).resolve().parents[1]/'scripts/run-ordinary-headset.ps1'
        with tempfile.TemporaryDirectory() as directory:
            scripts=Path(directory)/'scripts';scripts.mkdir()
            launcher=scripts/source.name;shutil.copyfile(source,launcher)
            (scripts/'run-modded.ps1').write_text('''
$vars=@{}; Get-ChildItem Env:SM2VR_* | ForEach-Object { $vars[$_.Name]=$_.Value }
$vars | ConvertTo-Json -Compress
''',encoding='utf-8')
            for runtime,selected,other in [('OpenVR','SM2VR_OPENVR','SM2VR_OPENXR'),('OpenXR','SM2VR_OPENXR','SM2VR_OPENVR')]:
                result=subprocess.run([pwsh(),'-NoProfile','-File',str(launcher),'-Runtime',runtime,'-Profile','-DirectLaunch'],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stderr)
                actual=json.loads(result.stdout)
                self.assertEqual(actual[selected],'1');self.assertNotIn(other,actual)
                for flag in ('SM2VR_ORDINARY_HEADSET','SM2VR_ORDINARY_SECOND_CAMERA','SM2VR_ORDINARY_PAIR_RETENTION','SM2VR_STEREO'):
                    self.assertEqual(actual[flag],'1')
                self.assertNotIn('SM2VR_CONTROLLERS',actual)
                self.assertTrue(Path(actual['SM2VR_PROFILE']).is_absolute())
                self.assertEqual(actual['SM2VR_PROFILE_MAX_ROWS'],'10000000')

    def test_headset_tracker_guard_default_on_and_stereo_warmup_switches(self):
        source=Path(__file__).resolve().parents[1]/'scripts/run-ordinary-headset.ps1'
        with tempfile.TemporaryDirectory() as directory:
            scripts=Path(directory)/'scripts'
            scripts.mkdir()
            launcher=scripts/source.name
            shutil.copyfile(source,launcher)
            (scripts/'run-modded.ps1').write_text('''
@{guard=$env:SM2VR_TRACKER_GUARD; warmup=$env:SM2VR_STEREO_WARMUP_JOBS} | ConvertTo-Json -Compress
''',encoding='utf-8')
            for flags,guard,warmup in (([],None,None),(['-NoTrackerGuard'],'0',None),(['-StereoWarmupJobs','2400'],None,'2400')):
                env=dict(os.environ,SM2VR_TRACKER_GUARD='stale',SM2VR_STEREO_WARMUP_JOBS='stale')
                result=subprocess.run([pwsh(),'-NoProfile','-File',str(launcher),*flags],env=env,capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stderr)
                actual=json.loads(result.stdout)
                self.assertEqual((actual['guard'],actual['warmup']),(guard,warmup))
    def test_injected_stereo_suspension_is_an_explicit_bounded_diagnostic(self):
        source=Path(__file__).resolve().parents[1]/'scripts/run-ordinary-headset.ps1'
        with tempfile.TemporaryDirectory() as directory:
            scripts=Path(directory)/'scripts';scripts.mkdir()
            launcher=scripts/source.name;shutil.copyfile(source,launcher)
            (scripts/'run-modded.ps1').write_text('''
$vars=@{}; Get-ChildItem Env:SM2VR_* | ForEach-Object { $vars[$_.Name]=$_.Value }
$vars | ConvertTo-Json -Compress
''',encoding='utf-8')
            run=lambda *flags:subprocess.run([pwsh(),'-NoProfile','-File',str(launcher),'-DirectLaunch',*flags],capture_output=True,text=True,timeout=10)
            plain=run();self.assertEqual(plain.returncode,0,plain.stderr)
            self.assertNotIn('SM2VR_DIAG_STEREO_SUSPEND_S',json.loads(plain.stdout))
            forced=run('-InjectStereoSuspendSeconds','30');self.assertEqual(forced.returncode,0,forced.stderr)
            self.assertEqual(json.loads(forced.stdout)['SM2VR_DIAG_STEREO_SUSPEND_S'],'30')
            self.assertNotIn('SM2VR_DIAG_RECORDS_MOVE_S',json.loads(forced.stdout))
            moved=run('-InjectRecordsMoveSeconds','45');self.assertEqual(moved.returncode,0,moved.stderr)
            self.assertEqual(json.loads(moved.stdout)['SM2VR_DIAG_RECORDS_MOVE_S'],'45')
            refused=run('-InjectRecordsMoveSeconds','2');self.assertNotEqual(refused.returncode,0)
            self.assertIn('-InjectRecordsMoveSeconds is 0 (off) or at least 5',refused.stderr)
            for bad in ('3','-1'):
                refused=run('-InjectStereoSuspendSeconds',bad)
                self.assertNotEqual(refused.returncode,0)
                self.assertIn('-InjectStereoSuspendSeconds is 0 (off) or at least 5',refused.stderr)

    def test_headset_defaults_to_light_union_with_shadow_refresh_and_diagnostic_starts_native(self):
        source=Path(__file__).resolve().parents[1]/'scripts/run-ordinary-headset.ps1'
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            scripts=root/'scripts'
            scripts.mkdir()
            launcher=scripts/source.name
            shutil.copyfile(source,launcher)
            # Exercise the real wrapper with a recording child, without game files.
            (scripts/'run-modded.ps1').write_text('''
@{mode=(Get-Content -LiteralPath $env:SM2VR_ORDINARY_SHADING_CONTROL).Trim(); path=$env:SM2VR_ORDINARY_SHADING_CONTROL} | ConvertTo-Json -Compress
''',encoding='utf-8')
            for flags,expected in (([],'light_union_shadow_refresh'),(['-ShadingControl'],'none'),
                                   (['-NativeShadows'],'light_union')):
                result=subprocess.run([pwsh(),'-NoProfile','-File',str(launcher),*flags],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stderr)
                actual=json.loads(result.stdout)
                self.assertEqual(actual['mode'],expected)
                self.assertTrue(Path(actual['path']).is_absolute())
            for flags,message in ((['-NativeShadows','-ShadingControl'],'-NativeShadows is implied by -ShadingControl'),):
                conflict=subprocess.run([pwsh(),'-NoProfile','-File',str(launcher),*flags],capture_output=True,text=True,timeout=10)
                self.assertNotEqual(conflict.returncode,0)
                self.assertIn(message,conflict.stderr)

    def test_headset_controls_gestures_and_trace_wiring(self):
        source=Path(__file__).resolve().parents[1]/'scripts/run-ordinary-headset.ps1'
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            scripts=root/'scripts'
            scripts.mkdir()
            launcher=scripts/source.name
            shutil.copyfile(source,launcher)
            (root/'build'/'vr_controls').mkdir(parents=True)
            (root/'build'/'vr_controls'/'SM2VR_FirstPerson_Test.pak').write_bytes(b'pk')
            app=root/'installer'/'app';app.mkdir(parents=True)
            shutil.copyfile(source.parents[1]/'installer/app/sm2-install.ps1',app/'sm2-install.ps1')
            pack_report=root/'build/vr_controls/SM2VR_FirstPerson_Test.json'
            pack_report.write_text(json.dumps({'input_layout':2}),encoding='utf-8')
            # The child records the SM2VR environment and the data pack it was given.
            (scripts/'run-modded.ps1').write_text('''
param([int]$Seconds,[string]$Tag,[switch]$NoUiAutomation,[string]$DataPak,[string]$GameArgs,$LaunchFlags)
$vars=@{}; Get-ChildItem Env:SM2VR_* | ForEach-Object { $vars[$_.Name]=$_.Value }
@{pak=$DataPak; vars=$vars; args=$GameArgs} | ConvertTo-Json -Compress -Depth 3
''',encoding='utf-8')
            def run(*flags):
                result=subprocess.run([pwsh(),'-NoProfile','-File',str(launcher),*flags],capture_output=True,text=True,timeout=10)
                return result
            stale=run('-Controls')
            self.assertNotEqual(stale.returncode,0)
            self.assertIn('Controls pack is out of date',stale.stdout+stale.stderr)
            pack_report.write_text(json.dumps({'input_layout':3}),encoding='utf-8')
            plain=json.loads(run().stdout)
            self.assertTrue(plain['pak'].endswith('vr_menus\\SM2VR_FirstPerson_Test.pak'))
            self.assertNotIn('SM2VR_CHASE_CAM',plain['vars'])
            self.assertNotEqual(run('-ChaseCam').returncode,0,'-ChaseCam refuses to launch without its pack')
            (root/'build'/'chase_cam').mkdir(parents=True)
            (root/'build'/'chase_cam'/'SM2VR_FirstPerson_Test.pak').write_bytes(b'pk')
            chase=json.loads(run('-ChaseCam').stdout)
            self.assertTrue(chase['pak'].endswith('chase_cam\\SM2VR_FirstPerson_Test.pak'),'chase cam installs the stock-camera pack')
            self.assertEqual(chase['vars']['SM2VR_CHASE_CAM'],'1')
            self.assertNotIn('SM2VR_CONTROLLERS',chase['vars'])
            refused=run('-ChaseCam','-Controls')
            self.assertNotEqual(refused.returncode,0)
            self.assertIn('-ChaseCam is gamepad-only',refused.stdout+refused.stderr)
            self.assertNotIn('SM2VR_CONTROLLERS',plain['vars'])
            self.assertNotIn('SM2VR_TRACE',plain['vars'])
            self.assertNotIn('SM2VR_HIDE_HEAD',plain['vars'])
            self.assertNotIn('SM2VR_UPRIGHT_TORSO',plain['vars'])
            self.assertNotIn('SM2VR_EYE_DUMP_DIR',plain['vars'])
            self.assertNotIn('SM2VR_PLAYER_DISTANCE_FADE',plain['vars'])
            fade=json.loads(run('-PlayerDistanceFade').stdout)
            self.assertEqual(fade['vars']['SM2VR_PLAYER_DISTANCE_FADE'],'1')
            self.assertEqual(Path(fade['vars']['SM2VR_PLAYER_DISTANCE_FADE_CONTROL']).read_text().strip(),'opaque')
            snapshots=json.loads(run('-EyeSnapshots').stdout)
            self.assertEqual(snapshots['vars']['SM2VR_EYE_DUMP_DIR'],snapshots['vars']['SM2VR_ORDINARY_DUMP_DIR'])
            full_result=run('-Controls','-Gestures','-Trace','-DirectLaunch')
            self.assertEqual(full_result.returncode,0,full_result.stdout+full_result.stderr)
            full=json.loads(full_result.stdout)
            self.assertTrue(full['pak'].endswith('vr_controls\\SM2VR_FirstPerson_Test.pak'))
            self.assertEqual(full['vars']['SM2VR_SWING_MELEE'],'1','confirmed sword loadout enables light/power motion attacks')
            for name in ('SM2VR_CONTROLLERS','SM2VR_HANDS','SM2VR_BODY_FOLLOW','SM2VR_WEAPON_AIM',
                         'SM2VR_HOLSTERS','SM2VR_GUARD','SM2VR_LEFT_PUSH','SM2VR_GRENADE_GESTURE','SM2VR_TRACE','SM2VR_ORDINARY_HEADSET','SM2VR_HIDE_HEAD'):
                self.assertEqual(full['vars'][name],'1',name)
            for name in ('SM2VR_ACTION_MANIFEST','SM2VR_HANDS_CALIBRATION','SM2VR_WEAPON_CONFIG','SM2VR_SWING_CONFIG',
                         'SM2VR_GUARD_CONFIG','SM2VR_GRENADE_CONFIG'):
                self.assertTrue(Path(full['vars'][name]).is_absolute(),name)
            self.assertIn('enableModdersAutostartFlow',full['args'])
            self.assertEqual(full['vars']['SM2VR_OPENXR'],'1','OpenXR is the default runtime')
            self.assertNotIn('SM2VR_OPENVR',full['vars'])
            self.assertTrue(Path(full['vars']['SM2VR_OPENXR_GRIP_CALIBRATION']).is_absolute())
            openvr=json.loads(run('-Runtime','OpenVR','-Controls').stdout)['vars']
            self.assertEqual(openvr['SM2VR_OPENVR'],'1');self.assertNotIn('SM2VR_OPENXR',openvr)
            self.assertNotIn('SM2VR_OPENXR_GRIP_CALIBRATION',openvr)
            xr=run('-Runtime','OpenXR','-Controls','-Gestures','-TestDriver','-ScriptedHead')
            self.assertEqual(xr.returncode,0,xr.stderr)
            xr_vars=json.loads(xr.stdout.splitlines()[-1])['vars']
            self.assertEqual(xr_vars['SM2VR_OPENXR'],'1');self.assertNotIn('SM2VR_OPENVR',xr_vars)
            for name in ('SM2VR_CONTROLLERS','SM2VR_HANDS','SM2VR_GUARD','SM2VR_TEST_HEAD'):
                self.assertEqual(xr_vars[name],'1',name)
            grip=Path(xr_vars['SM2VR_OPENXR_GRIP_CALIBRATION'])
            self.assertTrue(grip.is_absolute());self.assertEqual(grip.name,'openxr_index_grip.calibration')
            self.assertNotIn('SM2VR_OPENXR_GRIP_LOG',xr_vars)
            fit_log=json.loads(run('-Runtime','OpenXR','-Controls','-GripFitLog').stdout)['vars']['SM2VR_OPENXR_GRIP_LOG']
            self.assertTrue(Path(fit_log).is_absolute());self.assertEqual(Path(fit_log).name,'openxr_grips.csv')
            self.assertNotEqual(run('-Runtime','OpenVR','-Controls','-GripFitLog').returncode,0)
            # Grenade aim is on by default with -Gestures: the native launch follows the VR gaze/hand.
            self.assertEqual(full['vars']['SM2VR_GRENADE_AIM'],'1')
            self.assertEqual(full['vars']['SM2VR_GRENADE_NATIVE_PROBE'],'1')
            aim_config=Path(full['vars']['SM2VR_GRENADE_AIM_CONFIG'])
            self.assertTrue(aim_config.is_absolute());self.assertEqual(aim_config.name,'index_grenade_aim.calibration')
            self.assertNotIn('SM2VR_GRENADE_VELOCITY_SAMPLES',full['vars'],'native velocity sampling is a diagnostic')
            self.assertNotIn('SM2VR_GRENADE_AIM_CANDIDATE',full['vars'])
            controls_only=json.loads(run('-Controls').stdout)['vars']
            for name in ('SM2VR_GRENADE_AIM','SM2VR_GRENADE_NATIVE_PROBE','SM2VR_GRENADE_AIM_CONFIG'):
                self.assertNotIn(name,controls_only,name)
            opted_out=run('-Controls','-Gestures','-NoGrenadeAim')
            self.assertEqual(opted_out.returncode,0,opted_out.stderr)
            opted_out=json.loads(opted_out.stdout)['vars']
            self.assertEqual(opted_out['SM2VR_GRENADE_GESTURE'],'1','opting out of aim keeps the grenade gesture')
            for name in ('SM2VR_GRENADE_AIM','SM2VR_GRENADE_NATIVE_PROBE','SM2VR_GRENADE_AIM_CONFIG','SM2VR_GRENADE_VELOCITY_SAMPLES'):
                self.assertNotIn(name,opted_out,name)
            self.assertNotEqual(run('-Controls','-NoGrenadeAim').returncode,0)
            probe=json.loads(run('-Controls','-Gestures','-GrenadeVelocityProbe').stdout)['vars']
            self.assertEqual(probe['SM2VR_GRENADE_VELOCITY_SAMPLES'],'1');self.assertEqual(probe['SM2VR_GRENADE_AIM'],'1')
            self.assertNotEqual(run('-Controls','-Gestures','-NoGrenadeAim','-GrenadeVelocityProbe').returncode,0)
            self.assertNotEqual(run('-Controls','-GrenadeVelocityProbe').returncode,0)
            self.assertEqual(full['vars']['SM2VR_CHAINSAW_REV'],'1','analog chainsword rev is part of the default controls set')
            self.assertNotIn('SM2VR_CHAINSAW_REV',plain['vars'],'no controls, no rev')
            no_rev=json.loads(run('-Controls','-Gestures','-NoChainswordRev').stdout)['vars']
            self.assertNotIn('SM2VR_CHAINSAW_REV',no_rev,'-NoChainswordRev opts out')
            self.assertEqual(no_rev['SM2VR_SWING_MELEE'],'1','opting out of the rev keeps the rest of the controls')
            self.assertNotEqual(run('-NoChainswordRev').returncode,0)
            # Game HUD and upright torso: on by default with -Controls since 27 October (playtests
            # launched without the old opt-in switches had no HUD); -NoCompactHud / -NoUprightTorso opt out.
            # The no-op opt-in switches are gone (docs/FLAGS.md); tests/test_launch_flags.py checks they're refused.
            self.assertEqual(full['vars']['SM2VR_NATIVE_HUD'],'1','the game HUD is part of the default controls set')
            self.assertEqual(full['vars']['SM2VR_UPRIGHT_TORSO'],'1','the upright torso is part of the default controls set')
            self.assertNotIn('SM2VR_NATIVE_HUD',plain['vars'],'no controls, no HUD')
            self.assertNotIn('SM2VR_UPRIGHT_TORSO',plain['vars'],'no controls, no upright torso')
            no_hud=json.loads(run('-Controls','-Gestures','-NoCompactHud').stdout)['vars']
            self.assertNotIn('SM2VR_NATIVE_HUD',no_hud,'-NoCompactHud opts out')
            self.assertEqual(no_hud['SM2VR_UPRIGHT_TORSO'],'1','opting out of the HUD keeps the upright torso')
            no_upright=json.loads(run('-Controls','-Gestures','-NoUprightTorso').stdout)['vars']
            self.assertNotIn('SM2VR_UPRIGHT_TORSO',no_upright,'-NoUprightTorso opts out')
            self.assertEqual(no_upright['SM2VR_NATIVE_HUD'],'1','opting out of the torso keeps the HUD')
            self.assertIn('-NoCompactHud needs -Controls',run('-NoCompactHud').stderr)
            self.assertIn('-NoUprightTorso needs -Controls',run('-NoUprightTorso').stderr)
            # Gaze camera: on by default with -Controls since 21 October; -NoGazeCamera opts out.
            self.assertEqual(full['vars']['SM2VR_GAZE_CAMERA'],'1','gaze camera steering is part of the default controls set')
            self.assertEqual(Path(full['vars']['SM2VR_GAZE_CAMERA_CONFIG']).name,'index_gaze_camera.calibration')
            self.assertTrue(Path(full['vars']['SM2VR_GAZE_CAMERA_CONFIG']).is_absolute())
            self.assertNotIn('SM2VR_GAZE_CAMERA',plain['vars'],'no controls, no gaze camera')
            no_gaze=json.loads(run('-Controls','-Gestures','-NoGazeCamera').stdout)['vars']
            self.assertNotIn('SM2VR_GAZE_CAMERA',no_gaze,'-NoGazeCamera opts out')
            self.assertNotIn('SM2VR_GAZE_CAMERA_CONFIG',no_gaze)
            self.assertEqual(no_gaze['SM2VR_SWING_MELEE'],'1','opting out of the gaze camera keeps the rest of the controls')
            # Guard guide: opt-in with -GuardGuide since 23 October (was on by default from 21 October).
            self.assertNotIn('SM2VR_GUARD_GUIDE',full['vars'],'the guard guide is not part of the default controls set')
            self.assertNotIn('SM2VR_GUARD_GUIDE_CONFIG',full['vars'])
            self.assertNotIn('SM2VR_GUARD_GUIDE',plain['vars'],'no controls, no guard guide')
            guide=json.loads(run('-Controls','-Gestures','-GuardGuide').stdout)['vars']
            self.assertEqual(guide['SM2VR_GUARD_GUIDE'],'1','-GuardGuide opts in')
            self.assertEqual(Path(guide['SM2VR_GUARD_GUIDE_CONFIG']).name,'index_guard_guide.calibration')
            self.assertTrue(Path(guide['SM2VR_GUARD_GUIDE_CONFIG']).is_absolute())
            self.assertEqual(full['vars']['SM2VR_GUARD'],'1','without the guide the guard itself still runs')
            bad_guide=run('-GuardGuide')
            self.assertNotEqual(bad_guide.returncode,0);self.assertIn('-GuardGuide needs -Controls',bad_guide.stderr)
            no_controls=run('-NoGazeCamera')
            self.assertNotEqual(no_controls.returncode,0);self.assertIn('-NoGazeCamera needs -Controls',no_controls.stderr)
            # Haptics are on by default with -Controls; -NoHaptics opts out.
            self.assertEqual((full['vars']['SM2VR_HAPTICS'],full['vars']['SM2VR_HAPTICS_GAIN']),('1','1'))
            haptics=run('-Controls','-HapticsGain','0.5')
            self.assertEqual(haptics.returncode,0,haptics.stderr)
            self.assertEqual((json.loads(haptics.stdout)['vars']['SM2VR_HAPTICS'],json.loads(haptics.stdout)['vars']['SM2VR_HAPTICS_GAIN']),('1','0.5'))
            no_haptics=json.loads(run('-Controls','-NoHaptics').stdout)['vars']
            self.assertNotIn('SM2VR_HAPTICS',no_haptics);self.assertNotIn('SM2VR_HAPTICS_GAIN',no_haptics)
            self.assertIn('-NoHaptics needs -Controls',run('-NoHaptics').stderr)
            self.assertIn('-HapticsGain must be 0..2',run('-Controls','-HapticsGain','3').stderr)
            upright=run('-Controls')
            self.assertEqual(upright.returncode,0,upright.stderr)
            self.assertEqual(json.loads(upright.stdout)['vars']['SM2VR_UPRIGHT_TORSO'],'1')
            self.assertNotIn('SM2VR_UPRIGHT_TWIST',json.loads(upright.stdout)['vars'])
            lean_only=run('-Controls','-NoUprightTwist')
            self.assertEqual(lean_only.returncode,0,lean_only.stderr)
            self.assertEqual(json.loads(lean_only.stdout)['vars']['SM2VR_UPRIGHT_TWIST'],'0')
            self.assertIn('-NoUprightTwist needs the upright torso',run('-Controls','-NoUprightTorso','-NoUprightTwist').stderr)
            visible_head=run('-Controls','-KeepHeadVisible')
            self.assertEqual(visible_head.returncode,0,visible_head.stderr)
            visible_vars=json.loads(visible_head.stdout)['vars']
            self.assertNotIn('SM2VR_HIDE_HEAD',visible_vars)
            self.assertEqual(visible_vars['SM2VR_UPRIGHT_TORSO'],'1')
            for name in ('SM2VR_HANDS','SM2VR_BODY_FOLLOW','SM2VR_WEAPON_AIM'):
                self.assertEqual(visible_vars[name],'1')
            self.assertNotEqual(run('-KeepHeadVisible').returncode,0)
            self.assertNotIn('SM2VR_BODY_RENDER_BACK_METRES',full['vars'])
            self.assertNotIn('SM2VR_BODY_RENDER_CONTROL',full['vars'])
            placement=run('-Controls','-BodyPlacementCandidate')
            self.assertEqual(placement.returncode,0,placement.stderr)
            self.assertEqual(json.loads(placement.stdout)['vars']['SM2VR_BODY_RENDER_BACK_METRES'],'0.2')
            placement_vars=json.loads(placement.stdout)['vars']
            control=Path(placement_vars['SM2VR_BODY_RENDER_CONTROL'])
            self.assertTrue(control.is_absolute())
            self.assertEqual(control.parent,Path(placement_vars['SM2VR_ORDINARY_DUMP_DIR']))
            self.assertEqual(control.read_text().strip(),'0.2')
            self.assertNotEqual(run('-BodyPlacementCandidate').returncode,0)
            self.assertNotIn('SM2VR_PLAYER_PART_VISIBILITY_CONTROL',full['vars'])
            part_visibility=run('-Controls','-PlayerPartVisibilityCandidate')
            self.assertEqual(part_visibility.returncode,0,part_visibility.stderr)
            part_vars=json.loads(part_visibility.stdout)['vars']
            part_control=Path(part_vars['SM2VR_PLAYER_PART_VISIBILITY_CONTROL'])
            self.assertTrue(part_control.is_absolute())
            self.assertEqual(part_control.parent,Path(part_vars['SM2VR_ORDINARY_DUMP_DIR']))
            self.assertEqual(part_control.read_text().strip(),'native','selected-part diagnostic always starts off')
            self.assertNotIn('SM2VR_BODY_RENDER_BACK_METRES',part_vars)
            self.assertNotEqual(run('-PlayerPartVisibilityCandidate').returncode,0)
            self.assertNotIn('SM2VR_INVALIDATE_ARM_POSE',full['vars'])
            invalidation=run('-Controls','-InvalidateArmPoseCandidate')
            self.assertEqual(invalidation.returncode,0,invalidation.stderr)
            self.assertEqual(json.loads(invalidation.stdout)['vars']['SM2VR_INVALIDATE_ARM_POSE'],'1')
            self.assertNotEqual(run('-InvalidateArmPoseCandidate').returncode,0)
            self.assertNotIn('SM2VR_RESOLVE_ARM_POSE',full['vars'])
            resolution=run('-Controls','-ResolveArmPoseCandidate')
            self.assertEqual(resolution.returncode,0,resolution.stdout+resolution.stderr)
            self.assertEqual(json.loads(resolution.stdout)['vars']['SM2VR_RESOLVE_ARM_POSE'],'1')
            self.assertNotEqual(run('-ResolveArmPoseCandidate').returncode,0)
            self.assertNotEqual(run('-Controls','-ResolveArmPoseCandidate','-InvalidateArmPoseCandidate').returncode,0)
            twist_alone=run('-NoUprightTwist')
            self.assertNotEqual(twist_alone.returncode,0)
            self.assertIn('-NoUprightTwist needs the upright torso',twist_alone.stderr)
            gestures_alone=run('-Gestures')
            self.assertNotEqual(gestures_alone.returncode,0)
            self.assertIn('-Gestures needs -Controls',gestures_alone.stderr)
            automated=run('-Controls','-TestDriver')
            self.assertEqual(automated.returncode,0,automated.stderr)
            automation=json.loads(automated.stdout.splitlines()[-1])['vars']
            self.assertTrue(Path(automation['SM2VR_TEST_DRIVER']).is_absolute())
            self.assertTrue(Path(automation['SM2VR_TEST_DRIVER']).is_dir())
            self.assertEqual(automation['SM2VR_TRACE'],'1')
            self.assertNotIn('SM2VR_TEST_HEAD',automation)
            head=run('-Controls','-TestDriver','-ScriptedHead')
            self.assertEqual(head.returncode,0,head.stderr)
            self.assertEqual(json.loads(head.stdout.splitlines()[-1])['vars']['SM2VR_TEST_HEAD'],'1')
            self.assertNotEqual(run('-TestDriver').returncode,0)
            self.assertNotEqual(run('-ScriptedHead').returncode,0)
            (root/'build'/'vr_controls'/'SM2VR_FirstPerson_Test.pak').unlink()
            missing=run('-Controls')
            self.assertNotEqual(missing.returncode,0)
            self.assertIn('Controls pack missing',missing.stderr)

    def test_game_args_reject_shell_syntax_before_install(self):
        script = str(Path(__file__).resolve().parents[1] / 'scripts/run-modded.ps1')
        for value in ('--startup & echo unexpected', '%PATH%', 'x\ny', '"quoted"', '!PATH!'):
            result = subprocess.run([pwsh(), '-NoProfile', '-File', script,
                                     '-NoUiAutomation', '-GameArgs', value], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('-GameArgs supports only simple engine option tokens', result.stderr)
            self.assertNotIn('installed ', result.stdout)

    def test_ordinary_missing_or_relative_capture_directory_refuses_before_any_install(self):
        script=str(Path(__file__).resolve().parents[1]/'scripts/run-modded.ps1')
        with tempfile.TemporaryDirectory() as directory:
            for capture in ('', '.', str(Path(directory)/'missing')):
                env=dict(os.environ,SM2VR_ORDINARY_OBSERVER='1',SM2VR_ORDINARY_DUMP_DIR=capture)
                result=subprocess.run([pwsh(),'-NoProfile','-File',script,'-NoUiAutomation'],env=env,capture_output=True,text=True,timeout=10)
                self.assertNotEqual(result.returncode,0)
                self.assertIn('Ordinary observer requires an absolute existing',result.stderr)
                self.assertNotIn('installed ',result.stdout)

    def test_no_ui_mode_refuses_scheduled_ui_actions_before_launch(self):
        script = str(Path(__file__).resolve().parents[1] / "scripts/run-modded.ps1")
        for option in ("-Keys", "-Shots"):
            result = subprocess.run([pwsh(), "-NoProfile", "-File", script,
                                     "-NoUiAutomation", option, "1"], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("-NoUiAutomation cannot be combined", result.stderr)
            self.assertNotIn("installed ", result.stdout)

    def test_video_overrides_refuse_bad_values_before_any_install(self):
        # Validation runs before the game checks, so these never reach the game folder or settings.
        script=str(Path(__file__).resolve().parents[1]/'scripts/run-modded.ps1')
        for flags,expected in ((['-Resolution','2560x1440'],'needs -WindowMode Windowed or Fullscreen'),
                               (['-Resolution','2560x1440','-WindowMode','Borderless'],'borderless'),
                               (['-Resolution','5120x2880','-WindowMode','Windowed'],'-Resolution must be between'),
                               (['-Resolution','2560 1440','-WindowMode','Windowed'],'-Resolution must look like'),
                               (['-WindowMode','Exclusive'],'-WindowMode must be one of'),
                               (['-RenderResolution','DLAA'],'-RenderResolution must be one of'),
                               (['-RenderResolution','Performance','-NoClose'],'-NoClose cannot be combined')):
            with self.subTest(flags=flags):
                result=subprocess.run([pwsh(),'-NoProfile','-File',script,'-NoUiAutomation',*flags],capture_output=True,text=True,timeout=10)
                self.assertNotEqual(result.returncode,0)
                self.assertIn(expected,result.stderr)
                self.assertNotIn('installed ',result.stdout)

    def test_headset_passes_video_overrides_and_extra_game_args_through(self):
        source=Path(__file__).resolve().parents[1]/'scripts/run-ordinary-headset.ps1'
        with tempfile.TemporaryDirectory() as directory:
            scripts=Path(directory)/'scripts';scripts.mkdir()
            launcher=scripts/source.name;shutil.copyfile(source,launcher)
            (scripts/'run-modded.ps1').write_text('''
param([int]$Seconds,[string]$Tag,[switch]$NoUiAutomation,[string]$DataPak,[string]$GameArgs,[string]$Resolution='unset',[string]$WindowMode='unset',[string]$RenderResolution='unset',$LaunchFlags)
@{args=$GameArgs;resolution=$Resolution;mode=$WindowMode;render=$RenderResolution} | ConvertTo-Json -Compress
''',encoding='utf-8')
            def run(*flags):
                result=subprocess.run([pwsh(),'-NoProfile','-File',str(launcher),*flags],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stderr)
                return json.loads(result.stdout.splitlines()[-1])
            plain=run()
            self.assertEqual((plain['resolution'],plain['mode'],plain['render'],plain['args']),('unset','unset','unset',''),'no options, no overrides')
            full=run('-Resolution','2560x1440','-WindowMode','Windowed','-RenderResolution','Quality','-DirectLaunch','-GameArgs','--set /Config/Foo/Bar 1')
            self.assertEqual((full['resolution'],full['mode'],full['render']),('2560x1440','Windowed','Quality'))
            self.assertEqual(full['args'],'--startup --set /Config/WhiteListedVariables/enableModdersAutostartFlow true --set /Config/Foo/Bar 1')
            self.assertEqual(run('-GameArgs','--set /Config/Foo/Bar 1')['args'],'--set /Config/Foo/Bar 1')

    def test_headset_output_size_sets_the_mod_and_the_settings_transaction(self):
        source=Path(__file__).resolve().parents[1]/'scripts/run-ordinary-headset.ps1'
        with tempfile.TemporaryDirectory() as directory:
            scripts=Path(directory)/'scripts';scripts.mkdir()
            launcher=scripts/source.name;shutil.copyfile(source,launcher)
            (scripts/'run-modded.ps1').write_text('''
param([int]$Seconds,[string]$Tag,[switch]$NoUiAutomation,[string]$DataPak,[string]$GameArgs,[string]$OutputSize='unset',$LaunchFlags)
@{param=$OutputSize;size=$env:SM2VR_OUTPUT_SIZE;mode=$env:SM2VR_OUTPUT_SIZE_MODE} | ConvertTo-Json -Compress
''',encoding='utf-8')
            def run(*flags):
                return subprocess.run([pwsh(),'-NoProfile','-File',str(launcher),*flags],capture_output=True,text=True,timeout=10)
            plain=json.loads(run().stdout.splitlines()[-1])
            self.assertEqual((plain['param'],plain['size'],plain['mode']),('unset',None,None))
            full=run('-OutputSize','1944x2160','-OutputSizeMode','config','-ExperimentalOutputSize')
            self.assertEqual(full.returncode,0,full.stderr)
            self.assertEqual(json.loads(full.stdout.splitlines()[-1]),{'param':'1944x2160','size':'1944x2160','mode':'config'})
            for flags,expected in ((['-OutputSizeMode','config'],'-OutputSizeMode needs -OutputSize'),
                                   (['-OutputSize','1944x2160','-OutputSizeMode','both','-ExperimentalOutputSize'],'-OutputSizeMode must be device or config'),
                                   # Diagnostic only: without the explicit switch it never reaches the game.
                                   (['-OutputSize','1944x2160'],'add -ExperimentalOutputSize'),
                                   (['-OutputSize','1944x2160','-ExperimentalOutputSize','-WindowMode','Windowed','-DesktopWindow','972x1080'],'halves the render targets'),
                                   (['-OutputSize','1944x2160','-OutputSizeMode','config'],'docs/EYE_SIZE.md'),
                                   (['-ExperimentalOutputSize'],'-ExperimentalOutputSize needs -OutputSize')):
                with self.subTest(flags=flags):
                    result=run(*flags);self.assertNotEqual(result.returncode,0);self.assertIn(expected,' '.join(result.stderr.replace('|',' ').split()));self.assertNotIn('"size"',result.stdout,'refused before run-modded')

    def test_output_size_refuses_without_the_settings_transaction_or_with_bad_values(self):
        script=str(Path(__file__).resolve().parents[1]/'scripts/run-modded.ps1')
        for env_value,flags,expected in (('1944x2160',[],'must be set together'),
                                         ('',['-OutputSize','1944x2160'],'must be set together'),
                                         ('1944x2160',['-OutputSize','1944x2161'],'must be set together'),
                                         ('1945x2160',['-OutputSize','1945x2160'],'-OutputSize must be even'),
                                         ('1944x2160',['-OutputSize','1944x2160','-Resolution','2560x1440','-WindowMode','Windowed'],'cannot be combined'),
                                         ('1944x2160',['-OutputSize','1944x2160','-NoClose'],'-NoClose cannot be combined')):
            with self.subTest(env=env_value,flags=flags):
                env=dict(os.environ);env.pop('SM2VR_OUTPUT_SIZE',None)
                if env_value:env['SM2VR_OUTPUT_SIZE']=env_value
                result=subprocess.run([pwsh(),'-NoProfile','-File',script,'-NoUiAutomation',*flags],env=env,capture_output=True,text=True,timeout=10)
                self.assertNotEqual(result.returncode,0);self.assertIn(expected,result.stderr);self.assertNotIn('installed ',result.stdout)

    def test_headset_desktop_window_is_strict_and_windowed_only(self):
        source=Path(__file__).resolve().parents[1]/'scripts/run-ordinary-headset.ps1'
        with tempfile.TemporaryDirectory() as directory:
            scripts=Path(directory)/'scripts';scripts.mkdir()
            launcher=scripts/source.name;shutil.copyfile(source,launcher)
            (scripts/'run-modded.ps1').write_text('''
@{window=$env:SM2VR_DESKTOP_WINDOW} | ConvertTo-Json -Compress
''',encoding='utf-8')
            def run(*flags):
                return subprocess.run([pwsh(),'-NoProfile','-File',str(launcher),*flags],capture_output=True,text=True,timeout=10)
            plain=run();self.assertEqual(plain.returncode,0,plain.stderr);self.assertIsNone(json.loads(plain.stdout.splitlines()[-1])['window'])
            ok=run('-Resolution','1944x2160','-WindowMode','Windowed','-DesktopWindow','972x1080')
            self.assertEqual(ok.returncode,0,ok.stderr);self.assertEqual(json.loads(ok.stdout.splitlines()[-1])['window'],'972x1080')
            for flags,expected in ((['-DesktopWindow','972x1080'],'needs -WindowMode Windowed'),
                                   (['-WindowMode','Borderless','-DesktopWindow','972x1080'],'needs -WindowMode Windowed'),
                                   (['-WindowMode','Windowed','-DesktopWindow','972X1080'],'-DesktopWindow must look like'),
                                   (['-WindowMode','Windowed','-DesktopWindow','972x1080;x'],'-DesktopWindow must look like')):
                with self.subTest(flags=flags):
                    result=run(*flags);self.assertNotEqual(result.returncode,0);self.assertIn(expected,result.stderr);self.assertNotIn('"size"',result.stdout,'refused before run-modded')


if __name__ == "__main__":
    unittest.main()
