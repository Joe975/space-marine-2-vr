import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('vr_diagnostics', Path(__file__).resolve().parents[1] / 'scripts/build_vr_diagnostics.py')
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)

# The game's presets/data/default_camera_effects/lense_distortion.s3dprs (30 Oct 2026).
LENS = ('preset   =   {\n   params   =   {\n      enabled   =   True\n      params   =   {\n         floatParam00   =   0.01\n'
        '         floatParam01   =   0\n         floatParam02   =   1\n         floatParam03   =   0\n         floatParam04   =   2\n'
        '         floatParam05   =   2\n         floatParam06   =   1.2\n      }\n   }\n   __type__   =   "screen_effect_13"\n}\n')
PRESET = 'preset = {\n motionBlurParams = {\n  enabled = True\n  scaleCamera = 0.25\n  scaleObject = 0.5\n  maxOffset = 0.15\n }\n __type__ = "motion_blur"\n}\n'


class DiagnosticPackTests(unittest.TestCase):
    def test_disable_and_preserve_other_fields(self):
        result = diagnostics.disable_motion_blur(PRESET)
        self.assertIn('enabled = False', result)
        self.assertIn('scaleCamera = 0', result)
        self.assertIn('scaleObject = 0', result)
        self.assertIn('maxOffset = 0.15', result)
        self.assertEqual(result, diagnostics.disable_motion_blur(result))

    def test_reject_unexpected_layout(self):
        for broken in (PRESET.replace('motion_blur', 'dof'), PRESET.replace('enabled = True', ''),
                       PRESET.replace('enabled = True', 'enabled = True\n enabled = False')):
            with self.assertRaises(ValueError):
                diagnostics.disable_motion_blur(broken)

    def test_dof_off_only_in_dof_params(self):
        dof = ('preset   =   {\n   dofParams   =   {\n      enabled   =   True\n      dynamicFocus   =   {\n'
               '         enabled   =   True\n      }\n      dist_focus   =   3\n   }\n   __type__   =   "dof"\n}\n')
        result = diagnostics.disable_dof(dof)
        self.assertIn('   dofParams   =   {\n      enabled   =   False\n', result)
        self.assertIn('         enabled   =   True\n', result)  # dynamicFocus untouched
        self.assertIn('dist_focus   =   3', result)
        self.assertIsNone(diagnostics.disable_dof(result))  # already off: no entry needed
        self.assertIsNone(diagnostics.disable_dof('preset   =   {\n   __type__   =   "dof"\n}\n'))  # empty preset
        for broken in (dof.replace('"dof"', '"hdr"'), dof.replace('   }\n   __type__', '   __type__'),
                       dof.replace('      dist_focus', '      enabled   =   True\n      dist_focus')):
            with self.assertRaises(ValueError):
                diagnostics.disable_dof(broken)

    def test_lens_distortion_off_strengths_kept(self):
        # 30 Oct: the default camera's lens distortion pushed each eye's scene outward (double crosshair).
        result = diagnostics.disable_lens_distortion(LENS)
        self.assertIn('   params   =   {\n      enabled   =   False\n', result)
        self.assertIn('floatParam00   =   0.01', result)
        self.assertIn('floatParam06   =   1.2', result)
        self.assertEqual(result, diagnostics.disable_lens_distortion(result))
        for broken in (LENS.replace('screen_effect_13', 'hdr'), LENS.replace('      enabled   =   True\n', ''),
                       LENS.replace('   }\n   __type__', '   __type__')):
            with self.assertRaises(ValueError):
                diagnostics.disable_lens_distortion(broken)

    def test_pack_is_deterministic_and_sources_unchanged(self):
        workspace_temp = Path(__file__).resolve().parents[1] / 'build/test_temp'
        workspace_temp.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workspace_temp) as temp:
            root = Path(temp)
            camera, presets = root / 'camera.pak', root / 'presets.pak'
            with zipfile.ZipFile(camera, 'w') as z:
                z.writestr('ssl/camera/cam_modes/cam_base.sso', b'camera fixture')
            with zipfile.ZipFile(presets, 'w') as z:
                for name in diagnostics.PRESETS:
                    z.writestr(name, PRESET)
                z.writestr('presets/data/dof/small_range.s3dprs', 'preset = {\n dofParams = {\n  enabled = True\n }\n __type__ = "dof"\n}\n')
                z.writestr('presets/data/dof/empty.s3dprs', 'preset = {\n __type__ = "dof"\n}\n')
                z.writestr(diagnostics.LENS_DISTORTION, LENS)
            originals = camera.read_bytes(), presets.read_bytes()
            a, b = root / 'a.pak', root / 'b.pak'
            diagnostics.build(camera, presets, a)
            diagnostics.build(camera, presets, b)
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(originals, (camera.read_bytes(), presets.read_bytes()))
            with zipfile.ZipFile(a) as z:
                self.assertEqual(z.read('ssl/camera/cam_modes/cam_base.sso'), b'camera fixture')
                for name in diagnostics.PRESETS:
                    self.assertIn(b'enabled = False', z.read(name))
                self.assertIn(b'enabled = False', z.read('presets/data/dof/small_range.s3dprs'))
                self.assertNotIn('presets/data/dof/empty.s3dprs', z.namelist())
                self.assertIn(b'      enabled   =   False\n', z.read(diagnostics.LENS_DISTORTION))
