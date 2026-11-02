import importlib.util
import hashlib
from pathlib import Path
import re
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("vr_controls", ROOT / "scripts/build_vr_controls.py")
controls = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controls)
from game_sources import game_text, require_game

# The installed game's own control files (read from its pak, not kept in the repo).
FIXTURE = PRESET = EDITABLE = SOCIAL = LIBRARY = PROJECT = NATIVE_PRESET = MELTA_HINT = HUB_SCREEN = SQUAD_PANEL = TIPS = ICONS = HUD_VIEW = None
COMBAT_ACTIONS = {"CounterAttack", "CycleWeaponNext", "Fire", "GunStrike", "MeleeHeavy", "MeleeLight",
                  "MeleeParry", "SelectPrimary", "SelectSecondary", "UseAttentionFocus"}
RIGHT_B_ACTIONS = {"Game.Use", "Game.AbortHoldable", "Game.HoldUse", "Game.DropSupplies", "Game.ExitSmartDefenceMG",
                   "Game.RemoveSpitterEffect", "Game.Reload", "Game.UseReviveToken"}
EQUIPMENT_ACTIONS = {"Game.UseEquipment", "Game.CastAbilityRoutine", "Game.AbilityRoutineDetonate"}


def setUpModule():
    global FIXTURE, PRESET, EDITABLE, SOCIAL, LIBRARY, PROJECT, NATIVE_PRESET, MELTA_HINT, HUB_SCREEN, SQUAD_PANEL, TIPS, ICONS, HUD_VIEW
    require_game()
    FIXTURE = game_text(controls.INPUT)
    PRESET = NATIVE_PRESET = game_text(controls.PRESET)
    EDITABLE = game_text(controls.EDITABLE)
    SOCIAL = game_text(controls.KEYMAP)
    LIBRARY = game_text(controls.PRESET_LIBRARY)
    PROJECT = game_text(controls.PROJECT_ACTIONS)
    MELTA_HINT = game_text(controls.MELTA_HINT)
    HUB_SCREEN = game_text(controls.HUB_SCREEN)
    SQUAD_PANEL = game_text(controls.SQUAD_PANEL)
    TIPS = game_text(controls.TIPS)
    ICONS = game_text(controls.ICONS)
    HUD_VIEW = game_text(controls.HUD_VIEW)
SPRINT = '''props = {
   prop_sprint = {
      sprintSettings = {
         sslModule = "locomotion.prop_sprint"
         maxAngle = 70
         minInputForce = 0.85
         __type = "SprintSettings"
      }
      sprintEnabledAnimLink = { link = "Primaris.Movement.SprintEnable" }
   }
   other_property = {
      maxAngle = 70
      minInputForce = 0.85
   }
}
'''


def preset_bindings(text, name):
    block = re.search(controls.PRESET_LAYOUT.block % name, text).group()
    return sorted((device, int(key)) for key, device in controls.SHORTCUT.findall(block))


def bindings(text, name):
    block = re.search(controls.BLOCK % name, text).group()
    return sorted((device, int(key)) for key, device in controls.SHORTCUT.findall(block))


def activate_preset(layer, social, descriptions, preset):
    """Resolve native descriptions and child fan-out, after loading base commands.

    The previous test compared authored base bindings and missed this later
    overwrite. Keep the resolver independent of the production patch functions.
    """
    # Only the on-foot combat and holster actions: the game layer's vehicle and jump-pack
    # actions share these keys in contexts the VR controls don't touch.
    result = {"Game."+a.group(1): {(d, int(k)) for k, d in controls.SHORTCUT.findall(a.group())}
              for a in controls.LAYER.any_action.finditer(layer)
              if a.group(1) in COMBAT_ACTIONS or "Game."+a.group(1) in RIGHT_B_ACTIONS|EQUIPMENT_ACTIONS}
    # Only the social wheel itself: keymap.sso's other layers (menus, lobby) share its layout.
    result.update({"SocialWheel."+a.group(1): {(d, int(k)) for k, d in controls.SHORTCUT.findall(a.group())}
                   for a in controls.SOCIAL_LAYOUT.any_action.finditer(social) if a.group(1) == "UseSocialWheel"})
    # The two Right B actions that live in keymap.sso, not the game layer.
    result.update({"Game."+a.group(1): {(d, int(k)) for k, d in controls.SHORTCUT.findall(a.group())}
                   for a in controls.SOCIAL_LAYOUT.any_action.finditer(social) if a.group(1) in ("UseReviveToken", "AbilityRoutineDetonate")})
    wrappers = {a.group(1): a.group() for a in controls.PRESET_LAYOUT.any_action.finditer(preset)}
    for a in controls.PRESET_LAYOUT.any_action.finditer(descriptions):
        wrapper = wrappers.get(a.group(1))
        if wrapper is None:
            continue
        keys = {(d, int(k)) for k, d in controls.SHORTCUT.findall(wrapper)}
        for link in re.findall(r'inputLink\s*=\s*"([^"]+)"', a.group()):
            # Native preset binds the specified devices; other devices retain
            # their base shortcuts. This distinction catches preset unbinding
            # leaving SocialWheel's stock Xbox-up fallback alive.
            devices = {d for d, k in keys}
            result[link] = {(d, k) for d, k in result.get(link, set()) if d not in devices} | keys
    return result


class VrControlsTests(unittest.TestCase):
    def test_effective_preset_keeps_combat_actions_independent(self):
        # Use full verbatim native preset, descriptions and variant library.
        native_preset = NATIVE_PRESET
        old = activate_preset(controls.patch_melee(controls.patch(FIXTURE)), SOCIAL,
                              EDITABLE, controls.patch_preset(native_preset)[0])
        self.assertIn(("gamepad_xenon",2), old["SocialWheel.UseSocialWheel"],
                      "reproduce native social-wheel fallback after preset stripping")
        layer = controls.patch_fire(controls.patch_melee(controls.patch(FIXTURE)))
        editable = controls.patch_editable(EDITABLE)
        preset = controls.patch_locked_presets(controls.patch_preset(native_preset)[0])
        effective = activate_preset(layer, controls.patch_social(SOCIAL), editable, preset)
        for device in controls.PADS:
            def actions(key):
                return {name for name, keys in effective.items() if (device,key) in keys}
            # Joe, 22 Oct: RT keeps the native contextual gun strike (after a perfect parry or dodge).
            self.assertEqual(actions(15), {"Game.Fire", "Game.GunStrike"})
            # 30 Oct: LB is parry and the stock counter again (a Hormagaunt's blue attack needs the counter).
            self.assertEqual(actions(12), {"Game.MeleeParry", "Game.CounterAttack"})
            self.assertEqual(actions(13), {"Game.MeleeLight"})
            self.assertEqual(actions(2), {"Game.MeleeHeavy"})
        # Unrelated configurable locomotion, equipment, menus and abilities survive.
        for name in ("Sprint", "UseEquipment", "Menu", "Ability", "EmoteWheel"):
            self.assertIn(re.search(controls.PRESET_LAYOUT.block % name, EDITABLE).group(), editable)
        for name in controls.LOCKED_EDITABLE:
            self.assertIsNone(re.search(controls.PRESET_LAYOUT.block % name, editable))
            self.assertIsNone(re.search(controls.PRESET_LAYOUT.block % name, preset))
            self.assertIsNone(re.search(controls.Layout(9,21).block % name,
                                       controls.patch_locked_presets(LIBRARY,9)))
        for text in (EDITABLE.replace('Game.CounterAttack','Game.Unknown'), EDITABLE+EDITABLE):
            with self.assertRaises(ValueError): controls.patch_editable(text)

    def effective_right_b(self):
        layer = controls.retarget(controls.patch_fire(controls.patch_melee(controls.patch(FIXTURE))), controls.LAYER, controls.RIGHT_B_LAYER)
        preset = controls.retarget(controls.patch_locked_presets(controls.patch_preset(NATIVE_PRESET)[0]), controls.PRESET_LAYOUT, controls.RIGHT_B_PRESET)
        social = controls.retarget(controls.patch_social(SOCIAL), controls.SOCIAL_LAYOUT, controls.RIGHT_B_KEYMAP)
        return activate_preset(layer, social, controls.patch_editable(EDITABLE), preset)

    def test_right_b_actions_reach_native_b_and_equipment_native_x_after_the_preset(self):
        # Without the preset move, activation copies the old key back over the keymap's.
        effective = self.effective_right_b()
        for device in controls.PADS:
            def actions(key):
                return {name for name, keys in effective.items() if (device, key) in keys}
            self.assertEqual(actions(controls.NATIVE_B), RIGHT_B_ACTIONS)
            self.assertEqual(actions(controls.NATIVE_X), EQUIPMENT_ACTIONS)
        native = activate_preset(FIXTURE, SOCIAL, EDITABLE, NATIVE_PRESET)
        self.assertIn(("gamepad_xenon", controls.NATIVE_X), native["Game.Use"], "the stock game: interact on native X")
        self.assertIn(("gamepad_xenon", controls.NATIVE_B), native["Game.UseEquipment"])
        # Only the Xbox pads move; the keyboard, mouse and DualShock shortcuts stay.
        for name in RIGHT_B_ACTIONS|EQUIPMENT_ACTIONS:
            self.assertEqual({(d, k) for d, k in effective[name] if d not in controls.PADS},
                             {(d, k) for d, k in native[name] if d not in controls.PADS}, name)

    def test_right_b_leaves_menus_cutscenes_and_every_other_entry_alone(self):
        social = controls.patch_social(SOCIAL)
        moved = controls.retarget(social, controls.SOCIAL_LAYOUT, controls.RIGHT_B_KEYMAP)
        def stripped(text):
            for name, _, _ in controls.RIGHT_B_KEYMAP:
                text = re.sub(controls.SOCIAL_LAYOUT.block % name, name, text)
            return text
        self.assertEqual(stripped(moved), stripped(social), "only the two named commands change")
        for name in ("SkipCinematic", "ChangeTeam", "Back", "Reject"):
            self.assertIn(re.search(controls.SOCIAL_LAYOUT.block % name, social).group(), moved)
        layer = controls.patch_fire(controls.patch_melee(controls.patch(FIXTURE)))
        moved = controls.retarget(layer, controls.LAYER, controls.RIGHT_B_LAYER)
        for name in ("RestartBench", "ToggleBenchUi", "Dodge", "CastAbilityUltimate"):
            self.assertIn(re.search(controls.BLOCK % name, layer).group(), moved)

    def test_retarget_refuses_a_stacked_or_unexpected_move(self):
        once = controls.retarget(FIXTURE, controls.LAYER, controls.RIGHT_B_LAYER)
        for text, moves in ((once, controls.RIGHT_B_LAYER), (FIXTURE + FIXTURE, controls.RIGHT_B_LAYER),
                            (FIXTURE, (("NoSuchAction", 9, 10),)), (FIXTURE, (("Use", controls.NATIVE_B, controls.NATIVE_X),))):
            with self.assertRaises(ValueError):
                controls.retarget(text, controls.LAYER, moves)

    def test_power_key_isolated_from_parry_weapon_selection_and_ping(self):
        patched=controls.patch_melee(controls.patch(FIXTURE))
        self.assertEqual(bindings(patched,"SelectPrimary"),[("keyboard",3)])
        self.assertEqual([(d,k) for d,k in bindings(patched,"MeleeHeavy") if d in controls.PADS],[("gamepad_xbox_one",2),("gamepad_xenon",2)])
        self.assertEqual([(d,k) for d,k in bindings(patched,"MeleeHeavy") if d not in controls.PADS],
                         [(d,k) for d,k in bindings(FIXTURE,"MeleeHeavy") if d not in controls.PADS])
        # 30 Oct: the stock CounterAttack stays on LB (blue-attack counters).
        self.assertIn(("gamepad_xenon",12),bindings(patched,"CounterAttack"))
        for name in ("MeleeParry","MeleeLight","CycleWeaponNext","CounterAttack"):
            self.assertEqual(bindings(patched,name),bindings(FIXTURE,name))
        with self.assertRaises(ValueError): controls.patch_melee(patched)

    def test_power_key_reaches_actual_gameplay_strike_binding(self):
        # The previous authored keymap had no gameplay command consuming up.
        self.assertNotIn('"Game.MeleeHeavy"', PROJECT)
        patched = controls.patch_project_actions(PROJECT)
        block = re.search(r'(?ms)^ {6}VrSwordPower\s*=\s*\{\n.*?^ {6}\}', patched).group()
        self.assertIn('inputLink   =   "Game.MeleeHeavy"', block)
        self.assertIn('__type   =   "MeleeActionCommandBinding"', block)
        self.assertIn('strikeType   =   "LIGHT_HOLD"', block)
        self.assertNotIn('HEAVY_TAP', block, 'debug-only heavy kind is not the chainsword held-strike route')
        # Apart from the added native command, actual gameplay bindings are identical.
        self.assertEqual(patched.replace('\n'+block,'',1), PROJECT)
        with self.assertRaises(ValueError): controls.patch_project_actions(patched)
        with self.assertRaises(ValueError): controls.patch_project_actions(PROJECT.replace('Game.MeleeLight','Game.Invalid'))

    def test_sprint_patches_player_override_and_preserves_unrelated_settings(self):
        patched = controls.patch_sprint(SPRINT)
        self.assertIn('maxAngle = 180', patched)
        self.assertIn('minInputForce = 0.4', patched)
        self.assertEqual(patched, SPRINT.replace('maxAngle = 70','maxAngle = 180',1).replace('minInputForce = 0.85','minInputForce = 0.4',1))
        for wrong in (patched, SPRINT + SPRINT, SPRINT.replace('maxAngle = 70', 'maxAngle = 45',1),
                      SPRINT.replace('minInputForce = 0.85','minInputForce = 0.8',1),
                      'sprintSettings = { maxAngle = 40 minInputForce = 0.4 __type = "SprintSettings" }'):
            with self.assertRaises(ValueError): controls.patch_sprint(wrong)

    def test_holster_keys_bound_and_conflict_removed(self):
        patched = controls.patch(FIXTURE)
        self.assertEqual(bindings(patched, "SelectPrimary"),
                         [("gamepad_xbox_one", 2), ("gamepad_xenon", 2), ("keyboard", 3)])
        self.assertEqual(bindings(patched, "SelectSecondary"),
                         [("keyboard", 2)])
        self.assertEqual(bindings(patched, "UseAttentionFocus"), [("gamepad_ds4", 2), ("keyboard", 20)])
        self.assertEqual(bindings(patched, "CycleWeaponNext"), bindings(FIXTURE, "CycleWeaponNext"))
        cycle = re.search(controls.BLOCK % "CycleWeaponNext", FIXTURE).group()
        self.assertIn(cycle, patched, "untouched actions must stay byte-identical")

    def test_inserted_entries_use_native_multiline_layout(self):
        # The native keyboard entry, with key/device substituted, must equal the new entry.
        block = re.search(controls.BLOCK % "SelectPrimary", FIXTURE).group()
        native = controls.ENTRY.search(block).group()
        expected = native.replace("3\n", "2\n").replace('"keyboard"', '"gamepad_xenon"')
        self.assertEqual(controls.entry(2, "gamepad_xenon"), expected)
        patched = re.search(controls.BLOCK % "SelectPrimary", controls.patch(FIXTURE)).group()
        self.assertIn(native + ",\n" + expected + ",\n", patched)
        self.assertRegex(patched, r"\}\n {15}\]\n {12}\}\n {12}__type")

    def test_refuses_stacked_or_unexpected_layouts(self):
        once = controls.patch(FIXTURE)
        primary = re.search(controls.BLOCK % "SelectPrimary", FIXTURE).group()
        inline = FIXTURE.replace(primary, primary.replace(controls.ENTRY.search(primary).group(),
                                 ' ' * 18 + '{ shortcuts = [ { keys = [ 3 ] deviceType = "keyboard" } ] }'))
        for text in (once, FIXTURE + FIXTURE, inline,
                     FIXTURE.replace('"InputActionHold"', '"InputActionTap"')):
            with self.assertRaises(ValueError):
                controls.patch(text)

    def test_preset_drops_xbox_holster_keys_from_other_actions(self):
        # Playtest 17 Oct: holster grabs also pinged squad mates (AttentionFocus) because
        # the preset re-binds key 2 over the keymap layer.
        patched, stripped = controls.patch_preset(PRESET)
        self.assertEqual(stripped, ["SocialWheel", "AttentionFocus"])
        self.assertEqual(preset_bindings(patched, "AttentionFocus"), [("gamepad_ds4", 2)])
        self.assertEqual(preset_bindings(patched, "SocialWheel"), [("gamepad_ds4", 2)])
        self.assertEqual(preset_bindings(patched, "EmoteWheel"), [("gamepad_ds4", 4), ("gamepad_xenon", 4)])
        fire = re.search(controls.PRESET_LAYOUT.block % "Fire", PRESET).group()
        self.assertIn(fire, patched, "unrelated preset actions must stay byte-identical")
        self.assertIn("time   =   0.25", re.search(controls.PRESET_LAYOUT.block % "EmoteWheel", patched).group())
        with self.assertRaises(ValueError):
            controls.patch_preset(patched)

    def test_preset_refuses_to_empty_a_binding_list(self):
        only_xbox = re.sub(r'"gamepad_ds4"', '"gamepad_xenon"', PRESET)
        with self.assertRaises(ValueError):
            controls.patch_preset(only_xbox)

    def test_melta_prompt_names_the_control_that_detonates(self):
        # 31 Oct 16:14:04: "B is detonate, but what does B mean?" At 17:49 "right stick down" read as
        # pressing the stick in, so it says click and hold.
        patched = controls.patch_melta_hint(MELTA_HINT)
        self.assertIn('"Detonate (VR: hold right stick click)"', patched)
        self.assertEqual(patched.replace('"Detonate (VR: hold right stick click)"', '"UI_ACTION_DETONATE"'), MELTA_HINT)
        with self.assertRaises(ValueError):
            controls.patch_melta_hint(MELTA_HINT.replace("UI_ACTION_DETONATE", "UI_ACTION_OTHER"))

    def test_edit_squad_prompt_names_the_control_that_sends_y(self):
        # 31 Oct 20:51:20: "there's a Y for edit squad, but I don't have that". Y is left trackpad up there.
        for text in (HUB_SCREEN, SQUAD_PANEL):
            patched = controls.patch_squad_prompt(text)
            self.assertIn('"Squad: left pad up"', patched)
            self.assertEqual(patched.replace('"Squad: left pad up"', '"UI_SQUAD_EDIT_SQUAD"'), text)
            with self.assertRaises(ValueError):
                controls.patch_squad_prompt(text.replace("UI_SQUAD_EDIT_SQUAD", "UI_SQUAD_OTHER"))

    def test_edit_squad_prompt_stays_on_the_corners_panel(self):
        # 1 Nov: the 33-character prompt grew left over x 0.70 and showed "(Y) Edit Sq" at the lens edge.
        self.assertLessEqual(len(controls.SQUAD_PROMPT), controls.SQUAD_PROMPT_MAX)
        self.assertIn("left", controls.SQUAD_PROMPT)  # A press up on the right pad is a grenade.

    def test_stim_stays_on_native_dpad_right_through_the_pack(self):
        # Stim is the left trackpad's right side (VrControllerInput.h): native key 5, the game's
        # Game.CastAbilityInjector. The game picks the Xbox device: stock binds only gamepad_xenon.
        pad = ("gamepad_xenon", 5)
        descriptions = controls.patch_editable(EDITABLE)
        links = re.search(controls.PRESET_LAYOUT.block % "UseInjector", descriptions).group()
        self.assertEqual(re.findall(r'inputLink\s*=\s*"([^"]+)"', links), ["Game.CastAbilityInjector"])
        preset = controls.retarget(controls.patch_locked_presets(controls.patch_preset(NATIVE_PRESET)[0]), controls.PRESET_LAYOUT, controls.RIGHT_B_PRESET)
        self.assertIn(pad, preset_bindings(preset, "UseInjector"))
        layer = controls.retarget(controls.patch_fire(controls.patch_melee(controls.patch(FIXTURE))), controls.LAYER, controls.RIGHT_B_LAYER)
        self.assertIn(pad, bindings(layer, "CastAbilityInjector"))
        self.assertEqual({a.group(1) for a in controls.LAYER.any_action.finditer(layer)
                          if ("5", pad[0]) in controls.SHORTCUT.findall(a.group())}, {"CastAbilityInjector"})

    def test_pickup_tips_name_real_controls(self):
        # 31 Oct 19:01:11: a stim tip says "Press <D-pad right>", and no controller has one.
        patched = controls.patch_pickup_tips(TIPS)
        for name in ("HealingInjector", "HealingInjectorUse", "HealingStim"):
            tip = re.search(r"(?ms)^ {9}%s\s*=\s*\{\n.*?^ {9}\}" % name, patched).group()
            self.assertIn("Press the right side of the left trackpad", tip)
            self.assertIn("Touch: right stick up.", tip)
            self.assertIn('inputLink   =   "Game.CastAbilityInjector"', tip, "using the stim still closes the tip")
        for key, words in controls.PICKUP_TIPS.items():
            self.assertIn(f'"{key}"', TIPS)
            self.assertNotIn(f'"{key}"', patched)
            self.assertNotRegex(words, r'[{}\[\]"]', "no slot, colour tag or quote: plain words")
            self.assertLess(len(words), 180, "about the longest stock reminder")
            patched = patched.replace(f'"{words}"', f'"{key}"')
        self.assertEqual(patched, TIPS, "nothing else in the library changes")
        with self.assertRaises(ValueError):
            controls.patch_pickup_tips(controls.patch_pickup_tips(TIPS))

    def test_the_stim_glyph_is_a_trackpad_and_only_that_row_changes(self):
        patched = controls.patch_stim_glyph(ICONS)
        row = re.search(r"(?ms)^ {9}XB_DPAD_RIGHT\s*=\s*\{\n.*?^ {9}\}", patched).group()
        self.assertIn("gamepad_tpad_right", row)
        self.assertNotIn("gamepad_xbox_dpad_right", row)
        self.assertIn(controls.STIM_GLYPH[1], re.search(r"(?ms)^ {9}PS_TPAD_RIGHT\s*=\s*\{\n.*?^ {9}\}", ICONS).group(), "the game's own art")
        self.assertEqual(patched.replace(*reversed(controls.STIM_GLYPH)).replace(*controls.STIM_GLYPH), ICONS.replace(*controls.STIM_GLYPH))
        self.assertEqual(len(patched.split("\n")), len(ICONS.split("\n")))
        with self.assertRaises(ValueError):
            controls.patch_stim_glyph(patched)

    def test_a_tip_lands_whole_on_the_corners_panel(self):
        # The headset shows bottom_right_* on the low panel and cuts that rectangle out of the full
        # canvas, so a tip half inside it shows in two pieces.
        canvas_width, canvas_height, tip_width, tip_height = 1920, 1080, 510, 170
        values = dict(line.split("=") for line in (ROOT / "config/index_native_hud_corners.calibration").read_text().splitlines()
                      if "=" in line and not line.startswith("#"))
        left, top = float(values["bottom_right_x"]) * canvas_width, float(values["bottom_right_y"]) * canvas_height
        right = left + float(values["bottom_right_width"]) * canvas_width
        bottom = top + float(values["bottom_right_height"]) * canvas_height
        def inside(x, y):
            return left <= x and x + tip_width <= right and top <= y and y + tip_height <= bottom
        self.assertFalse(inside(*controls.TIP_NATIVE), "the game's place is split by the panel")
        self.assertTrue(inside(*controls.TIP_VR))
        patched = controls.patch_tip_position(HUD_VIEW)
        block = re.search(r"(?ms)^   tips_attach\s*=\s*\{\n.*?^   \}", patched).group()
        self.assertRegex(block, r"x\s*=\s*%d\n\s*y\s*=\s*%d\n" % controls.TIP_VR)
        self.assertEqual(patched.replace(block, ""), HUD_VIEW.replace(re.search(r"(?ms)^   tips_attach\s*=\s*\{\n.*?^   \}", HUD_VIEW).group(), ""))
        with self.assertRaises(ValueError):
            controls.patch_tip_position(patched)

    def test_pack_preserves_base_entries_and_is_deterministic(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            base, source, out = (root / n for n in ("base.pak", "source.pak", "out.pak"))
            with zipfile.ZipFile(base, "w") as z:
                z.writestr("ssl/camera/cam_modes/cam_base.sso", b"first person camera")
                # The real base pack carries about 100 entries: every depth-of-field preset switched off.
                for i in range(100):
                    z.writestr(f"presets/data/dof/fixture_{i:03}.s3dprs", b"dof off")
            with zipfile.ZipFile(source, "w") as z:
                z.writestr(controls.INPUT, FIXTURE)
                z.writestr(controls.PRESET, PRESET)
                z.writestr(controls.SPRINT, SPRINT)
                z.writestr(controls.EDITABLE, EDITABLE)
                z.writestr(controls.KEYMAP, SOCIAL)
                z.writestr(controls.PRESET_LIBRARY, LIBRARY)
                z.writestr(controls.PROJECT_ACTIONS, PROJECT)
                z.writestr(controls.MELTA_HINT, MELTA_HINT)
                z.writestr(controls.HUB_SCREEN, HUB_SCREEN)
                z.writestr(controls.SQUAD_PANEL, SQUAD_PANEL)
                z.writestr(controls.TIPS, TIPS)
                z.writestr(controls.ICONS, ICONS)
                z.writestr(controls.HUD_VIEW, HUD_VIEW)
            fixture_hashes = {name: hashlib.sha256(data.encode()).hexdigest() for name,data in
                              ((controls.INPUT,FIXTURE),(controls.PRESET,PRESET),(controls.EDITABLE,EDITABLE),(controls.KEYMAP,SOCIAL),(controls.PRESET_LIBRARY,LIBRARY),(controls.PROJECT_ACTIONS,PROJECT),(controls.MELTA_HINT,MELTA_HINT),
                               (controls.HUB_SCREEN,HUB_SCREEN),(controls.SQUAD_PANEL,SQUAD_PANEL),(controls.TIPS,TIPS),(controls.ICONS,ICONS),(controls.HUD_VIEW,HUD_VIEW))}
            with self.assertRaisesRegex(ValueError, 'native source changed'):
                controls.build(base, source, out)
            controls.build(base, source, out, fixture_hashes)
            first = out.read_bytes()
            with zipfile.ZipFile(out) as z:
                self.assertEqual(z.read("ssl/camera/cam_modes/cam_base.sso"), b"first person camera")
                self.assertEqual(z.read(controls.INPUT).decode(), controls.retarget(controls.patch_fire(controls.patch_melee(controls.patch(FIXTURE))), controls.LAYER, controls.RIGHT_B_LAYER))
                self.assertEqual(z.read(controls.PRESET).decode(), controls.retarget(controls.patch_locked_presets(controls.patch_preset(PRESET)[0]), controls.PRESET_LAYOUT, controls.RIGHT_B_PRESET))
                self.assertEqual(z.read(controls.EDITABLE).decode(), controls.patch_editable(EDITABLE))
                self.assertEqual(z.read(controls.KEYMAP).decode(), controls.retarget(controls.patch_social(SOCIAL), controls.SOCIAL_LAYOUT, controls.RIGHT_B_KEYMAP))
                self.assertEqual(z.read(controls.PROJECT_ACTIONS).decode(), controls.patch_project_actions(PROJECT))
                self.assertEqual(z.read(controls.SPRINT).decode(), controls.patch_sprint(SPRINT))
                self.assertEqual(z.read(controls.MELTA_HINT).decode(), controls.patch_melta_hint(MELTA_HINT))
                self.assertEqual(z.read(controls.HUB_SCREEN).decode(), controls.patch_squad_prompt(HUB_SCREEN))
                self.assertEqual(z.read(controls.SQUAD_PANEL).decode(), controls.patch_squad_prompt(SQUAD_PANEL))
                self.assertEqual(z.read(controls.TIPS).decode(), controls.patch_pickup_tips(TIPS))
                self.assertEqual(z.read(controls.ICONS).decode(), controls.patch_stim_glyph(ICONS))
                self.assertEqual(z.read(controls.HUD_VIEW).decode(), controls.patch_tip_position(HUD_VIEW))
                self.assertNotIn('ssl/locomotion/prop_sprint.prop', z.namelist(), 'NPC/generic sprint defaults are not overridden')
            controls.build(base, source, out, fixture_hashes)
            self.assertEqual(first, out.read_bytes())
            with self.assertRaises(ValueError):
                controls.build(out, source, root / "again.pak")


if __name__ == "__main__":
    unittest.main()
