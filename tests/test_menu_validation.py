import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("menu_validation", Path(__file__).resolve().parents[1]/"scripts/build_menu_validation.py")
validation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validation)
FIXTURE = '''sections = {
   General = { commands = {
         IngameMenu   =   {
            cmd = { multiDeviceShortcuts = [
               { shortcuts = [ { keys = [ 1 ] deviceType = "keyboard" } ] }
            ] }
            __type   =   "InputActionHold"
         }
         Fire = { unchanged = True }
   } }
}
'''


class MenuValidationTests(unittest.TestCase):
    def test_disposable_pack_changes_only_pause_binding(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1]/"build") as folder:
            root = Path(folder)
            base, source, out = [root/name for name in ("base.pak", "source.pak", "out.pak")]
            with zipfile.ZipFile(base, "w") as z:
                z.writestr("ssl/ui/assets/pause/ui_pause_screen_view.sso", b"existing centred menu")
            with zipfile.ZipFile(source, "w") as z:
                z.writestr(validation.INPUT, FIXTURE)
            validation.build(base, source, out)
            first = out.read_bytes()
            with zipfile.ZipFile(out) as z:
                self.assertEqual(z.read("ssl/ui/assets/pause/ui_pause_screen_view.sso"), b"existing centred menu")
                patched = z.read(validation.INPUT).decode()
                self.assertEqual(patched.count('deviceType   =   "mouse"'), 1)
                self.assertIn('keys   =   [\n                              0\n', patched)
                self.assertNotIn('shortcuts = [ { keys = [ 0 ]', patched)
                self.assertIn('keys = [ 1 ] deviceType = "keyboard"', patched)
                self.assertIn('Fire = { unchanged = True }', patched)
            validation.build(base, source, out)
            self.assertEqual(first, out.read_bytes())

    def test_unknown_native_action_refused(self):
        for text in (FIXTURE.replace("IngameMenu", "SomethingElse"), FIXTURE+FIXTURE,
                     FIXTURE.replace('"InputActionHold"', '"InputActionTap"'), validation.pause_shortcut(FIXTURE)):
            with self.assertRaises(ValueError):
                validation.pause_shortcut(text)
