import json
from pathlib import Path
import unittest


class ControllerBindingsTests(unittest.TestCase):
    def test_index_binding_contract(self):
        folder = Path(__file__).resolve().parents[1] / "config"
        manifest = json.loads((folder / "vr_actions.json").read_text())
        actions = {item["name"]: item["type"] for item in manifest["actions"]}
        self.assertEqual(len(actions), len(manifest["actions"]))
        self.assertEqual(manifest["action_sets"], [{"name": "/actions/sm2vr", "usage": "leftright"}])
        binding_file = manifest["default_bindings"][0]
        self.assertEqual(binding_file["controller_type"], "knuckles")
        bindings = json.loads((folder / binding_file["binding_url"]).read_text())
        self.assertEqual(bindings["controller_type"], "knuckles")
        sources = bindings["bindings"]["/actions/sm2vr"]["sources"]
        outputs = set()
        for source in sources:
            self.assertTrue(source["path"].startswith("/user/hand/"))
            for kind, mapping in source["inputs"].items():
                action = mapping["output"]
                self.assertIn(action, actions)
                self.assertEqual(actions[action], "vector2" if kind == "position" else "vector1" if kind in ("pull", "force") else "boolean")
                outputs.add(action)
        skeleton_bindings = bindings["bindings"]["/actions/sm2vr"]["skeleton"]
        self.assertEqual(len(skeleton_bindings), 2)
        by_name = {item["name"]: item for item in manifest["actions"]}
        for hand, binding in zip(("left", "right"), skeleton_bindings):
            action = f"/actions/sm2vr/in/{hand}_fingers"
            self.assertEqual(binding, {"path": f"/user/hand/{hand}/input/skeleton/{hand}", "output": action})
            self.assertEqual(by_name[action]["type"], "skeleton")
            self.assertEqual(by_name[action]["skeleton"], f"/skeleton/hand/{hand}")
            outputs.add(action)
        self.assertEqual(outputs, set(actions))
        positions = {s["path"]: s["inputs"].get("position", {}).get("output") for s in sources}
        self.assertEqual(positions["/user/hand/left/input/thumbstick"], "/actions/sm2vr/in/move")
        self.assertEqual(positions["/user/hand/right/input/thumbstick"], "/actions/sm2vr/in/turn")
        routes = {(s["path"], s["mode"], kind): mapping["output"] for s in sources for kind, mapping in s["inputs"].items()}
        expected = [
            ("right", "b", "button", "click", "interact"),
            ("right", "trigger", "trigger", "pull", "fire"),
            ("right", "trigger", "trigger", "touch", "fire_touch"),
            ("left", "trigger", "trigger", "pull", "aim"),
            ("right", "grip", "force_sensor", "force", "melee"),
            ("left", "grip", "force_sensor", "force", "parry"),
            ("right", "trackpad", "force_sensor", "force", "grenade"),
            ("left", "trackpad", "trackpad", "position", "utility"),
            ("left", "trackpad", "force_sensor", "force", "utility_press"),
            ("right", "thumbstick", "joystick", "click", "finisher"),
        ]
        for hand, part, mode, kind, action in expected:
            self.assertEqual(routes[(f"/user/hand/{hand}/input/{part}", mode, kind)], f"/actions/sm2vr/in/{action}")


if __name__ == "__main__":
    unittest.main()
