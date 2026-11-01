"""Build a disposable menu-test pack with a mouse shortcut for native pause.

The normal VR pack and player bindings are unchanged. In this diagnostic pack only,
a left mouse press also activates the game's existing IngameMenu action, allowing
Computer Use to enter pause when synthetic keyboard pulses aren't sampled by SM2.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

INPUT = "ssl/input/keymap_layer_game.sso"


def pause_shortcut(text):
    text = text.replace("\r", "")
    pattern = r"(?ms)^ {9}IngameMenu\s*=\s*\{\n.*?^ {9}\}"
    matches = list(re.finditer(pattern, text))
    if len(matches) != 1:
        raise ValueError("expected exactly one native IngameMenu action")
    match = matches[0]
    block = match.group()
    if not re.search(r'__type\s*=\s*"InputActionHold"', block) or re.search(r'deviceType\s*=\s*"mouse"', block):
        raise ValueError("unexpected native pause action")
    shortcut = '''
                  {
                     shortcuts   =   [
                        {
                           keys   =   [
                              0
                           ]
                           deviceType   =   "mouse"
                        }
                     ]
                  },'''
    block, count = re.subn(r"(multiDeviceShortcuts\s*=\s*\[)", lambda m: m[0] + shortcut, block)
    if count != 1:
        raise ValueError("expected one pause shortcut list")
    return text[:match.start()] + block + text[match.end():]


def build(base, source, output):
    with zipfile.ZipFile(base) as z:
        if len(z.infolist()) > 64 or any(i.file_size > 2_000_000 for i in z.infolist()) or INPUT in z.namelist():
            raise ValueError("expected a bounded normal VR pack without input overrides")
        entries = {i.filename: z.read(i) for i in z.infolist()}
    with zipfile.ZipFile(source) as z:
        original = z.read(INPUT)
    entries[INPUT] = pause_shortcut(original.decode("utf-8")).encode("utf-8")
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as z:
        for name, data in sorted(entries.items()):
            z.writestr(zipfile.ZipInfo(name, (2026, 10, 15, 0, 0, 0)), data)
    result = {"diagnostic_only": True, "base_sha256": hashlib.sha256(Path(base).read_bytes()).hexdigest(),
              "input_source_sha256": hashlib.sha256(original).hexdigest(),
              "pak_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
              "extra_binding": "left mouse also opens native pause; do not use for playtests"}
    output.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("base", "source", "out"):
        parser.add_argument("--" + key, type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.base, args.source, args.out), indent=2))
