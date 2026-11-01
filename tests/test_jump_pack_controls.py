"""The Assault's jump pack: a tap of native Y dashes, Y held a fifth of a second leaps.

Joe, 26 Oct: "a tap just dashes me forward". The game's own data (read from the installed pak,
skipped without it) says why, and that the VR controls reach both: left A sends a short Y pulse
(the dash), left trackpad up holds Y while pressed (the leap). docs/CONTROLS.md and
docs/COMBAT_CONTROLS.md describe this; the test fails if the game or the pulse length changes it.
"""
from pathlib import Path
import re
import unittest

from game_sources import game_text, require_game

ROOT = Path(__file__).resolve().parents[1]
BINDING = "ssl/actions/commands/bindings/jetpack_activate_action_command_binding.sso"
CONTROLLER = "ssl/abilities/ability_controllers/ability_controller_desc_jetpack_pve.sso"
LIBRARY = "ssl/abilities/ability_library.sso"
KEYMAP = "ssl/input/keymap_layer_game.sso"
PAD_Y, PAD_RB = 11, 13  # gamepad_xenon key numbers (CastAbilityUltimate, MeleeLight)


def setUpModule():
    require_game()


def node(text, name):
    """One blueprint node's block (nodes sit at a nine-space indent)."""
    match = re.search(r"\n {9}%s\s+=\s+\{\n(.*?)\n {9}\}" % re.escape(name), text, re.S)
    if not match:
        raise AssertionError(f"no blueprint node {name}")
    return match.group(1)


def outputs(block):
    return re.findall(r'"(\w+)/\w+"', block)


def pad_keys(text, command):
    block = re.search(r"\n {9}%s\s+=\s+\{(.*?)\n {9}\}" % command, text, re.S).group(1)
    return {int(k) for k in re.findall(r'keys\s+=\s+\[\s*(\d+)\s*\]\s+deviceType\s+=\s+"gamepad_xenon"', block)}


def tap_pulse_ms():
    header = (ROOT / "mod/src/VrTapHold.h").read_text(encoding="utf-8")
    return int(re.search(r"PULSE=std::chrono::milliseconds\((\d+)\)", header).group(1))


class JumpPackControls(unittest.TestCase):
    def test_y_tap_dashes_and_hold_leaps(self):
        binding = game_text(BINDING)
        self.assertIn('inputLink   =   "Game.CastAbilityUltimate"', binding)
        hold = float(re.search(r"holdingTimeSec\s+=\s+([\d.]+)", binding).group(1))
        self.assertEqual(hold, 0.2)
        on_hold = re.search(r"commandDescOnHold\s+=\s+\{(.*?)\n\}", binding, re.S).group(1)
        on_release = re.search(r"commandDescOnRelease\s+=\s+\{(.*?)\n\}", binding, re.S).group(1)
        self.assertIn('castType   =   "CUSTOM"', on_hold)
        self.assertIn('castType   =   "ADDITIONAL"', on_release)

        self.assertIn('__type   =   "AbilityControllerDescJetpackPve"',
                      re.search(r"\n   ability_jetpack_pve\s+=\s+\{(.*?)\n   \}", game_text(LIBRARY), re.S).group(1))
        pve = game_text(CONTROLLER)
        # Held: the CUSTOM cast tags the Marine as thrusting, then flying, and starts the jump.
        custom = node(pve, "AbilityTaskOnCastStarted")
        self.assertIn('castType   =   "CUSTOM"', custom)
        self.assertIn("AbilityTaskChangeStatusTag", outputs(custom))
        self.assertIn("AbilityTaskChangeStatusTag_1", outputs(node(pve, "AbilityTaskChangeStatusTag")))
        self.assertIn('"AbilityTaskJetJump/StartJump"', node(pve, "AbilityTaskChangeStatusTag_1"))
        self.assertIn('"BlueprintNodeAbilityTaskJetJump"', node(pve, "AbilityTaskJetJump"))
        # Tapped: the release's ADDITIONAL cast is a dodge (the dash), unless already flying.
        additional = node(pve, "AbilityTaskOnCastStarted_1")
        self.assertIn('castType   =   "ADDITIONAL"', additional)
        self.assertIn('"AbilityTaskDataSetTargetTransform_4/Execute"', node(pve, "AbilityTaskTriggerAbilityEffect_1"))
        self.assertIn('"CommandDodge/Execute"', node(pve, "AbilityTaskDataSetTargetTransform_4"))
        self.assertIn('"AbilityTaskOnCastStarted_1/Unsubscribe"', node(pve, "AbilityTaskSubscribeOnStatusTag_3"))

        # The VR side: left A's tap pulse is shorter than the hold, so it dashes; a leap needs Y
        # held past the hold, which left trackpad up does for as long as it is pressed.
        self.assertLess(tap_pulse_ms(), hold * 1000)

    def test_pad_keys(self):
        keymap = game_text(KEYMAP)
        self.assertEqual(pad_keys(keymap, "CastAbilityUltimate"), {PAD_Y})
        # In the air a light attack (RB, what a sword swing sends) is the ground pound.
        self.assertEqual(pad_keys(keymap, "JetpackMeleeAttack"), {PAD_RB})
        self.assertEqual(pad_keys(keymap, "MeleeLight"), {PAD_RB})
        self.assertIn('inputLink   =   "Game.JetpackMeleeAttack"', game_text("ssl/actions/action_controller_project.sso"))


if __name__ == "__main__":
    unittest.main()
