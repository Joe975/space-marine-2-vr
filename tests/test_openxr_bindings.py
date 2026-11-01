import json
import re
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

# SteamVR binding (source path, mode, input) -> the OpenXR Index path for the same component.
SUFFIX = {
    ("joystick", "position"): "",
    ("joystick", "click"): "/click",
    ("button", "click"): "/click",
    ("trigger", "pull"): "/value",
    ("trigger", "touch"): "/touch",
    ("force_sensor", "force"): "/force",
    ("trackpad", "position"): "",
}


def steamvr_index_bindings():
    bindings = json.loads((ROOT / "config" / "bindings_index.json").read_text())
    out = set()
    for source in bindings["bindings"]["/actions/sm2vr"]["sources"]:
        path = source["path"].replace("/input/grip", "/input/squeeze")
        for name, value in source["inputs"].items():
            action = value["output"].rsplit("/", 1)[1]
            out.add((action, path + SUFFIX[(source["mode"], name)]))
    return out


def openxr_index_bindings():
    header = (ROOT / "mod" / "src" / "OpenXrInput.h").read_text()
    lines = [line for line in header.splitlines() if line.rstrip().endswith("// BINDING")]
    out = set()
    for line in lines:
        match = re.fullmatch(r'\s*\{(\w+),"([^"]+)"\}, // BINDING', line)
        if not match:
            raise AssertionError("unparseable BINDING line: " + line)
        out.add((match.group(1), match.group(2)))
    return out, len(lines)


class OpenXrBindingTests(unittest.TestCase):
    def test_index_table_matches_steamvr_bindings(self):
        openxr, count = openxr_index_bindings()
        self.assertEqual(count, len(openxr), "duplicate OpenXR bindings")
        self.assertEqual(openxr, steamvr_index_bindings())

    def test_every_manifest_action_is_known_to_openxr(self):
        manifest = json.loads((ROOT / "config" / "vr_actions.json").read_text())
        names = {item["name"].rsplit("/", 1)[1] for item in manifest["actions"] if item["type"] != "skeleton"}
        header = (ROOT / "mod" / "src" / "OpenXrInput.h").read_text()
        declared = set(re.findall(r'\{"(\w+)",Kind::', header))
        self.assertEqual(declared, names)


if __name__ == "__main__":
    unittest.main()
