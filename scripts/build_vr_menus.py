"""Build an owned VR test pack with selectively centred screen menu assets.

The installed archives are read-only. Gameplay HUD and world markers are excluded.
The existing camera/diagnostic pack is copied byte-for-byte into the output archive.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import zipfile

spec = importlib.util.spec_from_file_location("menu_sso_fields", Path(__file__).with_name("build_first_person.py"))
sso = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sso)

MENUS = {
    "pause": "ssl/ui/assets/pause/ui_pause_screen_view.sso",
    "settings": "ssl/ui/assets/settings/ui_settings_view.sso",
    "controls": "ssl/ui/assets/settings_controls/ui_layout_control_screen_view.sso",
    "hub_map": "ssl/ui/assets/hub_map/ui_hub_map_view.sso",
    "planet": "ssl/ui/assets/hub_selectors/select_planet/ui_select_planet_view.sso",
    "campaign": "ssl/ui/assets/hub_selectors/select_story/ui_select_story_mission_view.sso",
    "operations": "ssl/ui/assets/hub_selectors/select_pve/ui_select_pve_mode_view.sso",
    # In-mission objectives/squad/mutators screen (Tab / gamepad View, keymap "Scoreboard";
    # PvE router state "Scoreboard" -> Screens.StoryScoreboard). Its content spans the full canvas.
    "scoreboard": "ssl/ui/assets/story_score/ui_story_scoreboard_view.sso",
}
# The verified native movie design canvas is 1920x1080 (UiMovie virtual +0x110).
CANVAS = (1920, 1080)
SCHEMA = {"scale": (0.6, 0.35, 1.0, "fraction of authored menu size"),
          "centre_x": (0.5, 0.1, 0.9, "fraction of screen width"),
          "centre_y": (0.5, 0.1, 0.9, "fraction of screen height")}


def parameters(overrides):
    if not isinstance(overrides, dict) or set(overrides) - {*SCHEMA, "content_anchor_x"}:
        raise ValueError("unknown menu parameter")
    result = {k: v[0] for k, v in SCHEMA.items()}
    result.update({k: v for k, v in overrides.items() if k in SCHEMA})
    anchors = overrides.get("content_anchor_x", {})
    if not isinstance(anchors, dict) or set(anchors) - MENUS.keys():
        raise ValueError("unknown menu content anchor")
    for key, value in result.items():
        _, low, high, _ = SCHEMA[key]
        if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or not low <= value <= high:
            raise ValueError(f"{key} must be finite in [{low}, {high}]")
    for value in anchors.values():
        if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or not 0.1 <= value <= 0.9:
            raise ValueError("content anchor must be finite in [0.1, 0.9]")
    result["content_anchor_x"] = dict(anchors)
    for key in MENUS:
        offset = offsets(result, key)
        if any(v < -1e-8 or v + result["scale"] > 1 + 1e-8 for v in offset):
            raise ValueError(f"{key} menu would extend beyond the screen")
    return result


def offsets(params, key):
    return (params["centre_x"] - params["scale"] * params["content_anchor_x"].get(key, 0.5),
            params["centre_y"] - params["scale"] * 0.5)


def patch(text, params, key):
    text = text.replace("\r\n", "\n").replace("\r", "")
    # An animated root transform could override the preset; refuse instead of silently
    # losing it. Descendant transforms and all animation/color/input data remain native.
    for match in re.finditer(r'fullName\s*=\s*"root"', text):
        end = text.find("fullName", match.end())
        track = text[match.end():end if end >= 0 else len(text)]
        if re.search(r"(?m)^\s+(pos|pivot|scale|rotation3d)\s*=", track):
            raise ValueError("animated root transform needs an explicit adaptation")
    expected_type = "UiCanvas" if key == "controls" else "UiFusionContainer"
    if not re.search(r'(?m)^__type\s*=\s*"' + expected_type + r'"\s*$', text):
        raise ValueError(f"unexpected {key} menu root type")
    x, y = offsets(params, key)
    return sso.replace_fields(text, {
        "pos": sso.vector(x=round(x * CANVAS[0], 6), y=round(y * CANVAS[1], 6)),
        "pivot": sso.vector(),
        # Native UiContainer scale is percentage-based, not a unit multiplier.
        "scale": sso.object_text("Vector2d", x=params["scale"] * 100, y=params["scale"] * 100),
    })


def build(base_pack, source, output, overrides):
    params = parameters(overrides)
    entries, hashes, placements = {}, {}, {}
    with zipfile.ZipFile(base_pack) as base:
        for entry in base.infolist():
            if entry.filename in entries or entry.file_size > 2_000_000 or not entry.filename.startswith(("ssl/camera/", "presets/data/")):
                raise ValueError("expected an unchanged camera/diagnostic base pack")
            entries[entry.filename] = base.read(entry)
    if not entries:
        raise ValueError("empty base pack")
    with zipfile.ZipFile(source) as original:
        for key, path in MENUS.items():
            if original.getinfo(path).file_size > 2_000_000:
                raise ValueError("unexpected menu asset size")
            data = original.read(path)
            hashes[path] = hashlib.sha256(data).hexdigest()
            entries[path] = patch(data.decode("utf-8"), params, key).encode("utf-8")
            x, y = offsets(params, key)
            placements[key] = {"origin_design_pixels": [round(x * CANVAS[0], 6), round(y * CANVAS[1], 6)],
                               "scale_percent": params["scale"] * 100}
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, data in sorted(entries.items()):
            archive.writestr(zipfile.ZipInfo(name, (2026, 10, 14, 0, 0, 0)), data)
    manifest = {"parameters": params, "schema": SCHEMA, "placements": placements,
                "base_pack_sha256": hashlib.sha256(Path(base_pack).read_bytes()).hexdigest(),
                "source_entry_sha256": hashes, "pak_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                "entries": sorted(entries), "purpose": "Selective VR screen menu placement; HUD/markers unchanged."}
    output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-pack", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--preset", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.base_pack, args.source, args.out, json.loads(args.preset.read_text())), indent=2))
