import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import parry_timeline  # noqa: E402


class ParryTimelineTest(unittest.TestCase):
    def test_indicator_presses_and_outcome(self):
        lines = [
            {"t": 0.0, "type": "header"},
            {"t": 9.9, "type": "sample", "character": {"root": [0, 0, 0], "root_z_axis": [0, 0, 1]}},
            {"t": 10.0, "type": "event", "event": "attack_event", "detail": "parry actor=hormagaunt inst=1a2b pos=[3.000,0.000,3.000]"},
            {"t": 10.05, "type": "event", "event": "parry_sound", "detail": "indicator_parry ui_parry_attack_indicator object=7"},
            {"t": 10.3, "type": "event", "event": "button", "name": "LB_parry_block", "down": True},
            {"t": 10.9, "type": "event", "event": "parry_sound", "detail": "player_hit ui_hud_hit_player object=1"},
            {"t": 20.0, "type": "event", "event": "parry_sound", "detail": "indicator_dodge ui_dodge_attack_indicator object=9"},
        ]
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "trace.jsonl"
            path.write_text("\n".join(json.dumps(x) for x in lines) + "\n{cut line\n")
            rows = parry_timeline.timeline(*parry_timeline.load(path))
        self.assertEqual(len(rows), 2)
        first = rows[0]
        self.assertEqual(first["attacker"], "hormagaunt")
        self.assertAlmostEqual(first["bearing"], 45.0, places=1)  # attacker ahead and to the side
        self.assertEqual(first["presses"], [0.25])
        self.assertEqual(first["outcome"][1], "player_hit")
        self.assertEqual(rows[1]["attacker"], "9")  # no animation event: the indicator's sound object
        self.assertIsNone(rows[1]["bearing"])  # and no transforms: no bearing
        self.assertIsNone(rows[1]["outcome"])

    def test_bearing_from_sound_object_transforms(self):
        import struct

        def transform(pos, front=(0.0, 0.0, 1.0), top=(0.0, 1.0, 0.0), position_last=True):
            vectors, position = struct.pack("<6f", *front, *top), struct.pack("<3d", *pos)
            return (vectors + position if position_last else position + vectors).hex()
        lines = [
            {"t": 0.0, "type": "header"},
            # The player faces +z (front), y up; the attacker is 4 m to the player's right-ish (+x) and 4 m ahead.
            {"t": 4.0, "type": "event", "event": "parry_sound", "detail": f"parry_animation wpn_melee_chswd_parry object=e79 transform={transform((10, 0, 10))}"},
            {"t": 5.0, "type": "event", "event": "parry_sound", "detail": f"indicator_parry ui_parry_attack_indicator object=e8f transform={transform((14, 0, 14), position_last=False)}"},
            {"t": 7.0, "type": "event", "event": "parry_sound", "detail": f"player_hit ui_hud_hit_shield object=e74 transform={transform((10, 0, 10))}"},
        ]
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "trace.jsonl"
            path.write_text("\n".join(json.dumps(x) for x in lines) + "\n")
            rows = parry_timeline.timeline(*parry_timeline.load(path))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["attacker"], "e8f")
        self.assertAlmostEqual(abs(rows[0]["bearing"]), 45.0, places=1)  # either field order decodes
        self.assertAlmostEqual(rows[0]["distance"], 5.7, places=1)
        self.assertEqual(rows[0]["outcome"][1], "player_hit")
        # With a trace sample the body's facing comes from the trace (root_z_axis), not the player's object.
        lines.insert(1, {"t": 5.0, "type": "sample", "character": {"root": [10, 0, 10], "root_z_axis": [-1, 0, 0]}})
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "trace.jsonl"
            path.write_text("\n".join(json.dumps(x) for x in lines) + "\n")
            rows = parry_timeline.timeline(*parry_timeline.load(path))
        self.assertAlmostEqual(rows[0]["bearing"], 135.0, places=1)  # facing -x, attacker at +x+z: behind and to the side
        self.assertIsNone(rows[0]["bearing_at_outcome"])  # the hit note carries no attacker transform

    def test_bearing_at_outcome(self):
        import struct
        pos = lambda p: (struct.pack("<6f", 0, 0, 1, 0, 1, 0) + struct.pack("<3d", *p)).hex()
        lines = [
            {"t": 0.0, "type": "header"},
            {"t": 5.0, "type": "event", "event": "parry_sound", "detail": f"indicator_parry ui_parry_attack_indicator object=e8f transform={pos((10, 0, 30))}"},
            {"t": 7.0, "type": "sample", "character": {"root": [10, 0, 10], "root_z_axis": [0, 0, 1]}},
            # At the hit the attacker has landed beside the player, 2 units to +x: 90 degrees off the facing.
            {"t": 7.0, "type": "event", "event": "parry_sound", "detail": f"player_hit ui_hud_hit_shield object=e74 transform={pos((10, 0, 10))} attacker_transform={pos((12, 0, 10))}"},
        ]
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "trace.jsonl"
            path.write_text("\n".join(json.dumps(x) for x in lines) + "\n")
            rows = parry_timeline.timeline(*parry_timeline.load(path))
        self.assertAlmostEqual(rows[0]["bearing"], 0.0, places=1)  # straight ahead when it signalled
        self.assertAlmostEqual(rows[0]["bearing_at_outcome"], 90.0, places=1)  # beside the player when it hit


if __name__ == "__main__":
    unittest.main()
