import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("build_vr_menus", Path(__file__).resolve().parents[1] / "scripts" / "build_vr_menus.py")
menus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(menus)
FIXTURE = '''sslModule = "ui.fusion.ui_fusion_container"
childs = {
   button = {
      pos = { x = 96 y = 540 __type = "Vector3d" }
      inputLink = "UI.Accept"
   }
}
animation = { type = { tracks = [] } }
__type = "UiFusionContainer"
'''


class MenuPackTests(unittest.TestCase):
    def test_shipped_pause_preset_centres_native_button_column(self):
        preset = json.loads((Path(__file__).resolve().parents[1]/"config/vr_menus.json").read_text())
        params = menus.parameters(preset)
        # Native pause asset: holder x=96, default button width=400; live GPU borders
        # confirm the resulting column. The previous 0.23 anchor missed by 4.56%.
        centre = menus.offsets(params, "pause")[0]+params["scale"]*(96+400/2)/1920
        self.assertLess(abs(centre-.5), .01)

    def test_real_pack_path_consumes_settings_and_preserves_base_and_children(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1] / "build") as folder:
            root = Path(folder)
            base, source, output = (root / n for n in ("base.pak", "source.pak", "output.pak"))
            with zipfile.ZipFile(base, "w") as z:
                z.writestr("ssl/camera/cam_modes/cam_base.sso", b"accepted camera\r\n")
                z.writestr("presets/data/motion_blur.s3dprs", b"accepted disabled blur")
            with zipfile.ZipFile(source, "w") as z:
                for name in menus.MENUS.values():
                    z.writestr(name, FIXTURE.replace('"UiFusionContainer"\n', '"UiCanvas"\n') if name == menus.MENUS["controls"] else FIXTURE)
                z.writestr("ssl/ui/assets/hud/crosshair.sso", b"must not touch")
            preset = {"scale": 0.6, "content_anchor_x": {"settings": 0.267}}
            menus.build(base, source, output, preset)
            first = output.read_bytes()
            with zipfile.ZipFile(output) as z:
                self.assertEqual(z.read("ssl/camera/cam_modes/cam_base.sso"), b"accepted camera\r\n")
                self.assertEqual(z.read("presets/data/motion_blur.s3dprs"), b"accepted disabled blur")
                self.assertEqual(set(z.namelist()), set(menus.MENUS.values()) | {"ssl/camera/cam_modes/cam_base.sso", "presets/data/motion_blur.s3dprs"})
                settings = z.read(menus.MENUS["settings"]).decode()
                self.assertIn("x = 652.416", settings)
                self.assertIn("x = 60.0", settings)
                self.assertIn('inputLink = "UI.Accept"', settings)
                self.assertIn('pos = { x = 96 y = 540 __type = "Vector3d" }', settings)
                # The in-mission objectives screen (Tab / View) is shrunk and centred too.
                scoreboard = z.read("ssl/ui/assets/story_score/ui_story_scoreboard_view.sso").decode()
                self.assertIn("x = 384.0", scoreboard)
                self.assertIn("x = 60.0", scoreboard)
                self.assertIn('inputLink = "UI.Accept"', scoreboard)
            menus.build(base, source, output, preset)
            self.assertEqual(first, output.read_bytes(), "deterministic output")
            menus.build(base, source, output, {"scale": 0.7})
            self.assertNotEqual(first, output.read_bytes(), "real pack consumes scale knob")
            manifest = json.loads(output.with_suffix(".json").read_text())
            self.assertEqual(manifest["placements"]["settings"]["scale_percent"], 70)
            previous = output.read_bytes()
            menus.build(base, source, output, {"scale": 0.7, "centre_x": 0.55, "centre_y": 0.55,
                                             "content_anchor_x": {"pause": 0.4}})
            self.assertNotEqual(previous, output.read_bytes(), "centre and anchor knobs change real assets")
            placement = json.loads(output.with_suffix(".json").read_text())["placements"]
            self.assertEqual(placement["pause"]["origin_design_pixels"], [518.4, 216.0])
            self.assertEqual(placement["settings"]["origin_design_pixels"], [384.0, 216.0])
            self.assertEqual(placement["scoreboard"]["origin_design_pixels"], [384.0, 216.0])

    def test_layout_bounds_and_invalid_presets_fail(self):
        for preset in ({"scale": float("nan")}, {"scale": True}, {"unknown": 1},
                       {"centre_x": 0.1}, {"content_anchor_x": {"hud": 0.5}},
                       {"content_anchor_x": {"pause": float("inf")}}):
            with self.assertRaises(ValueError):
                menus.parameters(preset)

    def test_unexpected_or_animated_root_refused(self):
        params = menus.parameters({})
        for text in (FIXTURE.replace('"UiFusionContainer"\n', '"UiOther"\n'),
                     FIXTURE.replace('tracks = []', 'tracks = [{ fullName = "root"\n   pos = {} }]')):
            with self.assertRaises(ValueError):
                menus.patch(text, params, "pause")


if __name__ == "__main__":
    unittest.main()
