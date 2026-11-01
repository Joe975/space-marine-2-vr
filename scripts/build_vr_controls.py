"""Build VR holsters, independent parry/power inputs and sprint settings.

D-pad left retains native EmoteWheel. Both holsters use state-checked weapon cycle
(down). Up selects the gameplay held-strike binding from right-hand power
swings; LB keeps parry and the stock CounterAttack (blue-attack counters, 30 Oct).
Native SSO multiline layouts are preserved and unexpected layouts refused.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

INPUT = "ssl/input/keymap_layer_game.sso"
PRESET = "ssl/input/new_controls_preset.sso"
SPRINT = "ssl/characters/player/marine/pc_marine_base_authority.cls"
EDITABLE = "ssl/main/user/editable_inputs/editable_input_library.sso"
KEYMAP = "ssl/input/keymap.sso"
PRESET_LIBRARY = "ssl/input/controls_preset_library.sso"
PROJECT_ACTIONS = "ssl/actions/action_controller_project.sso"
MELTA_HINT = "ssl/ui/screens/hud/action_hint_panel/rules/ui_action_hint_melta_detonation.sso"
MELTA_PROMPT = "Detonate (VR: hold right stick click)"
HUB_SCREEN = "ssl/ui/fusion/hud/hud_layout/code/hub/ui_hub_screen.sso"
SQUAD_PANEL = "ssl/ui/fusion/hud/squad_panel/code/ui_hub_squad_panel_widget.sso"
SQUAD_PROMPT = "Edit Squad (VR: left trackpad up)"
TIPS = "ssl/tutorial/tutorial_tips_library.sso"
ICONS = "ssl/ui/fusion/assets/switchers/ui_control_icon_hint_switcher.sso"
HUD_VIEW = "ssl/ui/fusion/hud/hud_layout/ui_hud_pve_view.sso"
STIM_PRESS = "Press the right side of the left trackpad"
STIM_TOUCH = "Touch: right stick up."
# A pickup's reminder tip fills {STRING} with the native button, which is D-pad right for a stim and X
# for a melta bomb; neither is a control on either headset. Plain text, without the game's colour tags:
# only plain literals have been seen on screen. The pack can't branch by headset, so the words name both
# (docs/BUTTON_HINTS.md).
PICKUP_TIPS = {
    "UI_REMINDER_PICKUP_HEALING_DESC": f"{STIM_PRESS} to use Medicae Stimms and restore some Health. You can carry up to 2. {STIM_TOUCH}",
    "UI_REMINDER_PICKUP_HEALING_USE_DESC": f"Low Health! {STIM_PRESS} to use Medicae Stimms. {STIM_TOUCH}",
    "UI_REMINDER_PICKUP_HEALINGSTIM_DESC": f"{STIM_PRESS} to apply Stimms that slowly restore Contested Health. You can carry up to 2. {STIM_TOUCH}",
    "UI_REMINDER_PICKUP_MELTA_DESC": "Grip at your belt to grab the bomb, then let go mid-swing to throw it. Hold the right stick click to detonate. Melta Bombs deal catastrophic Damage.",
}
STIM_GLYPH = ('"textures/ui/nav_panel/xbox/gamepad_xbox_dpad_right.asset/gamepad_xbox_dpad_right.texture.asset"',
              '"textures/ui/nav_panel/ps/gamepad_tpad_right.asset/gamepad_tpad_right.texture.asset"')
TIP_NATIVE, TIP_VR = (1300, 540), (1390, 630)
SOURCE_SHA256 = {
    INPUT: "fada0ad131cecaa0d504c76b30b16b8dc4396d0c649bfcb772047ea03c28290c",
    PRESET: "58bd7902a06ef95a21336abd35ba7d7b8a7af11ac9436bed189b8fd23eedf954",
    EDITABLE: "62647c64f77ba3b7ac4d98f62c38a0d4114ba88521b8e01b7e1f717c902452e4",
    KEYMAP: "94fca69cb12b69ffb604f7397c3a3dcac9f1b6e7e16c950ec11a0898b0ac2537",
    PRESET_LIBRARY: "b3ee90cd843c71158e03b0bf989c9d65b02195fb5c3ebcd41727e9ac7d0732a0",
    PROJECT_ACTIONS: "f81b79312fc6b9af7f8192a38d1e423c5df9cccef87bce1c899075f7439273cd",
    MELTA_HINT: "ea18e124fbc3dfd68b9e96cf635eaa7db18454df533caaaea17fcd320b1eb833",
    HUB_SCREEN: "f89ebeb23612a4191faaf24cb85b3265af4d86a9905153f271df66195e42750b",
    SQUAD_PANEL: "7c9bbf6d9351ea075d2b2a6f0b21687b8099511b50dcd1b5b246cbb729637586",
    TIPS: "0b7126b627d42c44799ea13e8206a1672b003040b85e16ab34938e2aefede4e8",
    ICONS: "f5a321edb3c47f8dd48b76c04e07baea0db704765e0fd6885a4e790a247eb9a7",
    HUD_VIEW: "2f13aa45e5f358d8115403ee2523a8ccbeb3131150b33a63999e7e3d4934a99d",
}
PRIMARY_KEY, SECONDARY_KEY = 2, 4
PADS = ("gamepad_xenon", "gamepad_xbox_one")
SHORTCUT = re.compile(r'keys\s*=\s*\[\s*(\d+)\s*\]\s*deviceType\s*=\s*"(\w+)"')
# Which native buttons the DLL and this pack agree on. Raise it whenever a change needs both: a DLL
# with a pack of another layout puts interact on the trackpad and grenades on Right B. The launchers
# refuse the mismatch (installer/app/sm2-install.ps1, Test-Sm2PackLayout).
INPUT_LAYOUT = 2
# Hints name the native key, so Right B has to be native B for a B prompt to mean the button with B
# on it (docs/BUTTON_HINTS.md). Interact and reload move to B, equipment to X; hold times stay.
NATIVE_B, NATIVE_X = 9, 10
RIGHT_B_PRESET = (("Interact", NATIVE_X, NATIVE_B), ("Reload", NATIVE_X, NATIVE_B), ("UseEquipment", NATIVE_B, NATIVE_X))
RIGHT_B_LAYER = (("Use", NATIVE_X, NATIVE_B), ("AbortHoldable", NATIVE_X, NATIVE_B), ("Reload", NATIVE_X, NATIVE_B),
                 ("UseEquipment", NATIVE_B, NATIVE_X), ("CastAbilityRoutine", NATIVE_B, NATIVE_X))
RIGHT_B_KEYMAP = (("UseReviveToken", NATIVE_X, NATIVE_B), ("AbilityRoutineDetonate", NATIVE_B, NATIVE_X))


class Layout:
    """Indentation of one SSO file: actions, their shortcut list and its entries."""
    def __init__(self, action, entries):
        self.action = action
        self.block = r"(?ms)^ {%d}%%s\s*=\s*\{\n.*?^ {%d}\}" % (action, action)
        self.any_action = re.compile(r"(?ms)^ {%d}(\w+)\s*=\s*\{\n.*?^ {%d}\}" % (action, action))
        self.entry = re.compile(r"(?ms)^ {%d}\{\n.*?^ {%d}\}" % (entries, entries))
        self.list = re.compile(r"(?ms)(^ {%d}multiDeviceShortcuts\s*=\s*\[\n)(.*?)(^ {%d}\])" % (entries - 3, entries - 3))


LAYER = Layout(9, 18)
PRESET_LAYOUT = Layout(3, 15)
SOCIAL_LAYOUT = Layout(15, 24)
# Back-compatible names used by tests.
BLOCK, ENTRY = LAYER.block, LAYER.entry


def entry(key, device):
    return (" " * 18 + "{\n" + " " * 21 + "shortcuts   =   [\n" + " " * 24 + "{\n" + " " * 27 + "keys   =   [\n"
            + " " * 30 + f"{key}\n" + " " * 27 + "]\n" + " " * 27 + f'deviceType   =   "{device}"\n'
            + " " * 24 + "}\n" + " " * 21 + "]\n" + " " * 18 + "}")


def shortcuts(block, layout=LAYER):
    """Return (list match, [(entry text, key, device)]) for one action block."""
    lists = list(layout.list.finditer(block))
    if len(lists) != 1:
        raise ValueError("expected one multiDeviceShortcuts list")
    body = lists[0].group(2)
    entries = []
    for match in layout.entry.finditer(body):
        found = SHORTCUT.findall(match.group())
        if len(found) != 1:
            raise ValueError("expected one single-key shortcut per entry")
        entries.append((match.group(), int(found[0][0]), found[0][1]))
    leftover = layout.entry.sub("", body).replace(",", "").strip()
    if not entries or leftover:
        raise ValueError("unexpected shortcut list layout")
    return lists[0], entries


def replace_entries(block, found, items):
    return block[:found.start(2)] + ",\n".join(items) + "\n" + block[found.end(2):]


def rewrite(text, name, keep, add=()):
    matches = list(re.finditer(LAYER.block % name, text))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one native {name} action")
    block = matches[0].group()
    if not re.search(r'__type\s*=\s*"InputActionHold"', block):
        raise ValueError(f"unexpected native {name} action type")
    found, entries = shortcuts(block)
    items = [e[0] for e in entries if keep(e[1], e[2])] + [entry(key, device) for key, device in add]
    block = replace_entries(block, found, items)
    return text[:matches[0].start()] + block + text[matches[0].end():], entries


def patch(text):
    text = text.replace("\r", "")
    for name, key in (("SelectPrimary", PRIMARY_KEY),):
        text, entries = rewrite(text, name, lambda k, d: True, [(key, device) for device in PADS])
        if any(device != "keyboard" for _, _, device in entries):
            raise ValueError(f"{name} already has non-keyboard bindings; refusing to stack edits")
    text, entries = rewrite(text, "UseAttentionFocus", lambda k, d: not (d in PADS and k == PRIMARY_KEY))
    if not any(d in PADS and k == PRIMARY_KEY for _, k, d in entries):
        raise ValueError("UseAttentionFocus no longer uses gamepad key 2; re-check the native layout")
    # Validate the intermediate holster edit before patch_melee reserves up.
    for key, name in ((PRIMARY_KEY, "SelectPrimary"), (SECONDARY_KEY, None)):
        users = {a.group(1) for a in LAYER.any_action.finditer(text)
                 if any(int(k) == key and d in PADS for k, d in SHORTCUT.findall(a.group()))}
        if users != ({name} if name else set()):
            raise ValueError(f"gamepad key {key} is shared with {sorted(users - {name})}")
    return text


def patch_preset(text):
    """Drop Xbox-pad bindings of the holster keys from every preset action."""
    text = text.replace("\r", "")
    keys, pieces, last, stripped = {PRIMARY_KEY}, [], 0, []
    for action in PRESET_LAYOUT.any_action.finditer(text):
        block = action.group()
        if not any(d in PADS and int(k) in keys for k, d in SHORTCUT.findall(block)):
            continue
        found, entries = shortcuts(block, PRESET_LAYOUT)
        kept = [e[0] for e in entries if not (e[2] in PADS and e[1] in keys)]
        # An empty shortcut list is an untested layout; keep at least one binding.
        if not kept:
            raise ValueError(f"stripping {action.group(1)} would leave no bindings")
        pieces += [text[last:action.start()], replace_entries(block, found, kept)]
        last = action.end()
        stripped.append(action.group(1))
    if "AttentionFocus" not in stripped:
        raise ValueError("preset AttentionFocus no longer uses the holster keys; re-check the native layout")
    return "".join(pieces) + text[last:], stripped


def patch_melee(text):
    """Keep LB exclusively parry; reserve unused VR up for native power.

    Holsters already use CycleWeaponNext (down), so the legacy direct-primary
    up alias can be removed. Preserve mouse/keyboard/DS4 and native action types.
    The stock CounterAttack stays on LB (30 Oct). Removing it (the 19 Oct fix for "the left grip does
    a power attack") left a Hormagaunt's blue attack uncounterable in every motion-controls launch
    (PLAYTEST_2026-10-30_0822).
    """
    text, entries = rewrite(text, "SelectPrimary", lambda k, d: not (d in PADS and k == 2))
    if {(k, d) for _, k, d in entries if d in PADS} != {(2, d) for d in PADS}:
        raise ValueError("expected the legacy VR primary alias before reserving up")
    text, entries = rewrite(text, "MeleeHeavy", lambda k, d: d not in PADS, [(2, d) for d in PADS])
    if {(k, d) for _, k, d in entries if d in PADS} != {(12, d) for d in PADS}:
        raise ValueError("unexpected native MeleeHeavy binding; expected LB")
    users = {a.group(1) for a in LAYER.any_action.finditer(text)
             if any(int(k) == 2 and d in PADS for k, d in SHORTCUT.findall(a.group()))}
    if users != {"MeleeHeavy"}:
        raise ValueError(f"power key is shared with unexpected actions: {sorted(users)}")
    return text


def patch_project_actions(text):
    """Bind the actual gameplay controller to chainsword's native held strike.

    Game.MeleeHeavy only had a consumer in the debug binder. The shipped project
    binder consumes MeleeLight's tap/hold events; chainsword Heavy1 is selected
    by LIGHT_HOLD, not the unused debug HEAVY_TAP command. A bounded up pulse
    selects that held strike through its own binding without holding light RB.
    """
    text = text.replace("\r", "")
    if "VrSwordPower" in text:
        raise ValueError("VR power binding already installed")
    block_pattern = r"(?ms)^ {6}MeleeLight\s*=\s*\{\n.*?^ {6}\}"
    matches = list(re.finditer(block_pattern, text))
    if len(matches) != 1 or 'inputLink   =   "Game.MeleeLight"' not in matches[0].group() or '__type   =   "MeleeActionCommandBinding"' not in matches[0].group():
        raise ValueError("unexpected project MeleeLight command binding")
    power = matches[0].group().replace("MeleeLight   =", "VrSwordPower   =", 1).replace('"Game.MeleeLight"', '"Game.MeleeHeavy"')
    setting = ('         meleeStrikeTapCommandDesc   =   {\n'
               '            type   =   {\n'
               '               strikeType   =   "LIGHT_HOLD"\n'
               '               __type   =   "CommandMeleeStrike"\n'
               '            }\n'
               '            __type   =   "CommandDesc"\n'
               '         }\n')
    power = power.replace('         __type   =   "MeleeActionCommandBinding"', setting+'         __type   =   "MeleeActionCommandBinding"')
    return text[:matches[0].end()] + "\n" + power + text[matches[0].end():]


def patch_fire(text):
    """Keep the native contextual GunStrike on RT (Joe, 22 Oct).

    The game offers a gun strike only in its window after a perfect parry or dodge; the next
    trigger pull then performs it, and outside that window RT is a plain shot. The 19 Oct
    pack removed the Xbox RT alias against "left pistol firing can also attack"; in the two
    19 Oct sessions with it, 1 of 44 shots without a melee press was followed by an attack
    animation (a contextual gun strike), the other 43 were plain shots
    (docs/validation/melee-evening-2026-10-21.md). This only checks the native binding.
    """
    users = {a.group(1) for a in LAYER.any_action.finditer(text)
             if any(int(k) == 15 and d in PADS for k, d in SHORTCUT.findall(a.group()))}
    if not {"Fire", "GunStrike"} <= users:
        raise ValueError(f"expected native Fire and GunStrike on Xbox RT, found {sorted(users)}")
    return text


# These links have explicit motion-control ownership. Preset activation copies
# bindings to child links too, after keymap_layer_game has loaded. Leaving Parry
# editable restored CounterAttack to LB; stripping only its base alias was inert.
LOCKED_EDITABLE = {
    "Fire": ("Game.Fire", "Game.AbortMelee", "Game.GunStrike"),
    "Melee": ("Game.MeleeLight", "Game.JetpackMeleeAttack", "Game.JetpackFallingAttack"),
    "Parry": ("Game.MeleeParry", "Game.CounterAttack"),
    "SocialWheel": ("SocialWheel.UseSocialWheel",),
    "AttentionFocus": ("Game.UseAttentionFocus",),
}


def patch_editable(text):
    """Keep VR-owned combat links out of preset/profile fan-out.

    Other editable actions remain byte-identical. The matching preset entries
    are removed as well, so a stock preset cannot address an absent description.
    Keyboard, mouse and DS4 keep their original base keymap bindings.
    """
    text = text.replace("\r", "")
    for name, links in LOCKED_EDITABLE.items():
        matches = list(re.finditer(PRESET_LAYOUT.block % name, text))
        if len(matches) != 1:
            raise ValueError(f"expected exactly one editable {name} description")
        block = matches[0].group()
        if tuple(re.findall(r'inputLink\s*=\s*"([^"]+)"', block)) != links or not re.search(r'__type\s*=\s*"EditableInputDescription"', block):
            raise ValueError(f"unexpected editable {name} link fan-out")
        text = text[:matches[0].start()] + text[matches[0].end():]
    return text


def patch_locked_presets(text, indent=3):
    """Remove only the five motion-owned actions from default/variant presets."""
    layout = Layout(indent, indent+12)
    text = text.replace("\r", "")
    for name in LOCKED_EDITABLE:
        text = re.sub(layout.block % name, "", text)
    return text


def patch_social(text):
    text = text.replace("\r", "")
    matches = list(re.finditer(SOCIAL_LAYOUT.block % "UseSocialWheel", text))
    if len(matches) != 1:
        raise ValueError("expected one native SocialWheel.UseSocialWheel")
    block = matches[0].group()
    found, entries = shortcuts(block, SOCIAL_LAYOUT)
    if {(k, d) for _, k, d in entries if d in PADS} != {(2, "gamepad_xenon")}:
        raise ValueError("unexpected native social-wheel Xbox shortcut")
    block = replace_entries(block, found, [e[0] for e in entries if e[2] not in PADS])
    return text[:matches[0].start()] + block + text[matches[0].end():]


def patch_sprint(text):
    # The player class overrides prop_sprint.prop's defaults with 70deg/0.85.
    # Patching only that generic file left native sprint unchanged in live tests.
    # Scope this to the marine player; VR enforces its own 40deg forward gate
    # before camera-heading conversion. Match the mapper's effective force gate.
    text = text.replace("\r", "")
    blocks = list(re.finditer(r"(?ms)^(?P<i> +)prop_sprint[ \t]*=[ \t]*\{.*?^(?P=i)\}", text))
    if len(blocks) != 1:
        raise ValueError("expected one marine player sprint property")
    block = blocks[0]
    settings = list(re.finditer(r"(?ms)^(?P<i> +)sprintSettings[ \t]*=[ \t]*\{.*?^(?P=i)\}", block.group()))
    if len(settings) != 1 or not re.search(r'__type\s*=\s*"SprintSettings"', settings[0].group()):
        raise ValueError("unexpected marine player sprint settings")
    original = settings[0].group()
    changed, angles = re.subn(r"(?m)^([ \t]*maxAngle[ \t]*=[ \t]*)70[ \t]*$", r"\g<1>180", original)
    changed, forces = re.subn(r"(?m)^([ \t]*minInputForce[ \t]*=[ \t]*)0\.85[ \t]*$", r"\g<1>0.4", changed)
    if angles != 1 or forces != 1:
        raise ValueError("unexpected native marine sprint angle/force defaults")
    a, b = block.start()+settings[0].start(), block.start()+settings[0].end()
    return text[:a]+changed+text[b:]


def retarget(text, layout, moves):
    """Move the Xbox shortcuts of named actions from one key to another; every other byte stays.

    moves: (action, old key, new key). The preset fans out to child links after the keymap loads,
    so an action needs the same move in both or the later one wins.
    """
    text = text.replace("\r", "")
    for name, old, new in moves:
        matches = list(re.finditer(layout.block % name, text))
        if len(matches) != 1:
            raise ValueError(f"expected exactly one native {name} action")
        block = matches[0].group()
        found, entries = shortcuts(block, layout)
        if {(k, d) for _, k, d in entries if d in PADS} != {(old, d) for d in PADS}:
            raise ValueError(f"unexpected native {name} Xbox binding; expected key {old}")
        items = [re.sub(r"(keys\s*=\s*\[\s*)\d+", rf"\g<1>{new}", e[0]) if e[2] in PADS else e[0] for e in entries]
        block = replace_entries(block, found, items)
        text = text[:matches[0].start()] + block + text[matches[0].end():]
    return text


def patch_melta_hint(text):
    # The prompt's glyph is the native pad's X, and neither Index has one (detonate is the right stick
    # click, held). Joe, 31 Oct: "B is detonate, but what does B mean?" The text names the real control.
    # It says click: "right stick down" read as pressing the stick in (17:49).
    changed, count = re.subn(r'(hintText\s*=\s*)"UI_ACTION_DETONATE"', r'\1"%s"' % MELTA_PROMPT, text)
    if count != 1:
        raise ValueError("unexpected melta detonation hint")
    return changed


def patch_squad_prompt(text):
    # Edit Squad is native Y. Joe, 31 Oct 20:51:20: "there's a Y for edit squad, but I don't have that",
    # which is how he lost his bots. Left B closes the pause menu, so Y there is left trackpad up. The
    # closing prompt, UI_SQUAD_CLOSE_EDIT_MODE, stays: inside the squad editor it is the game's own text.
    changed, count = re.subn(r'(navigationText\s*=\s*)"UI_SQUAD_EDIT_SQUAD"', r'\1"%s"' % SQUAD_PROMPT, text)
    if count != 1:
        raise ValueError("unexpected Edit Squad prompt")
    return changed


def patch_pickup_tips(text):
    # Joe, 31 Oct 19:01:11: "I don't know how to use stims or pickups". No {STRING} is left, so the tip
    # has the shape of the stock Guardian Relic tip: words, and completeInputs still naming the input.
    for key, words in PICKUP_TIPS.items():
        text, count = re.subn(r'(description\s*=\s*)"%s"' % key, r'\1"%s"' % words, text)
        if count != 1:
            raise ValueError(f"unexpected reminder tip {key}")
    return text


def patch_stim_glyph(text):
    # The stim hint is native D-pad right, and a D-pad is no control on any headset. The same row serves
    # every D-pad right glyph, in play and on menus; the left trackpad's right side sends D-pad right in
    # both. The art is the game's own PlayStation touchpad, the same size as the row it replaces.
    block = r"(?ms)^ {9}XB_DPAD_RIGHT\s*=\s*\{\n.*?^ {9}\}"
    found = list(re.finditer(block, text))
    if len(found) != 1 or STIM_GLYPH[0] not in found[0].group():
        raise ValueError("unexpected D-pad right icon row")
    return text[:found[0].start()] + found[0].group().replace(*STIM_GLYPH) + text[found[0].end():]


def patch_tip_position(text):
    # The tip's stock place straddles what the headset shows: its top and left at the lens edge, 27 to
    # 51 degrees out, its lower right cut away to the corners panel. Here all of it is on that panel,
    # in one piece (config/index_native_hud_corners.calibration); tests hold it inside that rectangle.
    pattern = (r'(?ms)(^   tips_attach\s*=\s*\{\n.*?offsetMin\s*=\s*\{\n\s*x\s*=\s*)%d(\n\s*y\s*=\s*)%d(\n)' % TIP_NATIVE)
    changed, count = re.subn(pattern, r"\g<1>%d\g<2>%d\g<3>" % TIP_VR, text)
    if count != 1:
        raise ValueError("unexpected tips attach point")
    return changed


def build(base, source, output, expected_sources=SOURCE_SHA256):
    with zipfile.ZipFile(base) as z:
        # About 100 entries since the base pack switches every depth-of-field preset off (64 of them).
        if len(z.infolist()) > 256 or any(i.file_size > 2_000_000 for i in z.infolist()) or {INPUT, PRESET} & set(z.namelist()):
            raise ValueError("expected a bounded VR pack without input overrides")
        entries = {i.filename: z.read(i) for i in z.infolist()}
    with zipfile.ZipFile(source) as z:
        original, preset, sprint = z.read(INPUT), z.read(PRESET), z.read(SPRINT)
        editable, keymap = z.read(EDITABLE), z.read(KEYMAP)
        library = z.read(PRESET_LIBRARY)
        project_actions = z.read(PROJECT_ACTIONS)
        melta_hint = z.read(MELTA_HINT)
        hub_screen, squad_panel = z.read(HUB_SCREEN), z.read(SQUAD_PANEL)
        tips, icons, hud_view = z.read(TIPS), z.read(ICONS), z.read(HUD_VIEW)
    for name, data in ((INPUT, original), (PRESET, preset), (EDITABLE, editable), (KEYMAP, keymap), (PRESET_LIBRARY, library), (PROJECT_ACTIONS, project_actions), (MELTA_HINT, melta_hint),
                       (HUB_SCREEN, hub_screen), (SQUAD_PANEL, squad_panel), (TIPS, tips), (ICONS, icons), (HUD_VIEW, hud_view)):
        if hashlib.sha256(data).hexdigest() != expected_sources[name]:
            raise ValueError(f"native source changed: {name}; re-audit preset activation before rebuilding")
    entries[INPUT] = retarget(patch_fire(patch_melee(patch(original.decode("utf-8")))), LAYER, RIGHT_B_LAYER).encode("utf-8")
    patched_preset, stripped = patch_preset(preset.decode("utf-8"))
    entries[PRESET] = retarget(patch_locked_presets(patched_preset), PRESET_LAYOUT, RIGHT_B_PRESET).encode("utf-8")
    entries[EDITABLE] = patch_editable(editable.decode("utf-8")).encode("utf-8")
    entries[KEYMAP] = retarget(patch_social(keymap.decode("utf-8")), SOCIAL_LAYOUT, RIGHT_B_KEYMAP).encode("utf-8")
    entries[PRESET_LIBRARY] = patch_locked_presets(library.decode("utf-8"), 9).encode("utf-8")
    entries[PROJECT_ACTIONS] = patch_project_actions(project_actions.decode("utf-8")).encode("utf-8")
    entries[SPRINT] = patch_sprint(sprint.decode("utf-8")).encode("utf-8")
    entries[MELTA_HINT] = patch_melta_hint(melta_hint.decode("utf-8")).encode("utf-8")
    entries[HUB_SCREEN] = patch_squad_prompt(hub_screen.decode("utf-8")).encode("utf-8")
    entries[SQUAD_PANEL] = patch_squad_prompt(squad_panel.decode("utf-8")).encode("utf-8")
    entries[TIPS] = patch_pickup_tips(tips.decode("utf-8").replace("\r", "")).encode("utf-8")
    entries[ICONS] = patch_stim_glyph(icons.decode("utf-8").replace("\r", "")).encode("utf-8")
    entries[HUD_VIEW] = patch_tip_position(hud_view.decode("utf-8").replace("\r", "")).encode("utf-8")
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as z:
        for name, data in sorted(entries.items()):
            z.writestr(zipfile.ZipInfo(name, (2026, 10, 17, 0, 0, 0)), data)
    result = {"base_sha256": hashlib.sha256(Path(base).read_bytes()).hexdigest(),
              "input_source_sha256": hashlib.sha256(original).hexdigest(),
              "preset_source_sha256": hashlib.sha256(preset).hexdigest(),
              "sprint_source_sha256": hashlib.sha256(sprint).hexdigest(),
              "editable_source_sha256": hashlib.sha256(editable).hexdigest(),
              "keymap_source_sha256": hashlib.sha256(keymap).hexdigest(),
              "preset_library_source_sha256": hashlib.sha256(library).hexdigest(),
              "project_actions_source_sha256": hashlib.sha256(project_actions).hexdigest(),
              "melta_hint_source_sha256": hashlib.sha256(melta_hint).hexdigest(),
              "hub_screen_source_sha256": hashlib.sha256(hub_screen).hexdigest(),
              "squad_panel_source_sha256": hashlib.sha256(squad_panel).hexdigest(),
              "tips_source_sha256": hashlib.sha256(tips).hexdigest(),
              "icons_source_sha256": hashlib.sha256(icons).hexdigest(),
              "hud_view_source_sha256": hashlib.sha256(hud_view).hexdigest(),
              "melta_prompt": MELTA_PROMPT,
              "squad_prompt": SQUAD_PROMPT,
              "pickup_tips": PICKUP_TIPS,
              "input_layout": INPUT_LAYOUT,
              "sprint": "marine player class 70deg/0.85 -> 180deg/0.4; VR forward gate 40deg, click toggle",
              "pak_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
              "bindings": {"MeleeHeavy": "gamepad key 2 -> project VrSwordPower LIGHT_HOLD; LB parry and the stock CounterAttack",
                           "SelectPrimary": "keyboard only; VR holsters use native cycle (down)",
                           "EmoteWheel": f"native gamepad key {SECONDARY_KEY}; no SelectSecondary alias",
                           "UseAttentionFocus": "layer gamepad key 2 removed; keyboard kept",
                           "fixed_motion_ownership": list(LOCKED_EDITABLE),
                           "GunStrike": "native Xbox RT alias kept (contextual: after a perfect parry/dodge the next trigger pull)",
                           "RightB": "interact and reload on native B, equipment and detonate on native X",
                           "preset_xbox_keys_removed_from": stripped}}
    output.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for key in ("base", "source", "out"):
        parser.add_argument("--" + key, type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.base, args.source, args.out), indent=2))
