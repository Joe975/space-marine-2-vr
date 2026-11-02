import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("analyze_vr_trace", Path(__file__).resolve().parents[1] / "scripts" / "analyze_vr_trace.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

HEADER = {"t": 0, "type": "header", "format": "sm2vr-trace", "version": 1, "hz": 30, "wall_clock_local": "2026-10-18T14:00:00"}


def sample(t, head=(0.0, 0.05, 0.1), residual=0.0, facing=2.0):
    return {"t": t, "type": "sample", "input": {"armed": True}, "character": {"driven": True},
            "camera": {"mode": "body", "facing_error_degrees": facing},
            "metrics": {"head_offset_metres": list(head), "left_target_offset_metres": [0.1, 0, 0], "right_target_offset_metres": None,
                        "left_residual_metres": residual, "right_residual_metres": None}}


def event(t, kind, **fields):
    return {"t": t, "type": "event", "event": kind, **fields}


def lines(records):
    return [json.dumps(r) for r in records]


class TraceAnalysisTests(unittest.TestCase):
    def test_steady_offset_has_no_drift(self):
        report, _ = module.analyse(lines([HEADER] + [sample(t / 30) for t in range(1800)]))
        seg = report["segments"][0]
        self.assertAlmostEqual(seg["head_offset"]["z"]["median"], 0.1)
        self.assertAlmostEqual(seg["drift_per_minute"]["horizontal"], 0, places=6)
        self.assertEqual(report["warnings"], [])

    def test_linear_drift_is_measured_and_warned(self):
        # 3 cm per minute forward drift over two minutes.
        records = [HEADER] + [sample(t / 30, head=(0, 0.05, 0.1 + 0.03 * (t / 30) / 60)) for t in range(3600)]
        report, _ = module.analyse(lines(records))
        seg = report["segments"][0]
        self.assertAlmostEqual(seg["drift_per_minute"]["z"], 0.03, places=4)
        self.assertAlmostEqual(seg["start_to_end"][2], 0.054, delta=0.003)
        self.assertTrue(any("drifts +3.0 cm/min" in w for w in report["warnings"]))

    def test_re_anchoring_splits_segments_so_jumps_are_not_drift(self):
        records = [HEADER] + [sample(t / 30) for t in range(900)] + [event(30.0, "tracking_reset", epoch=2)] + \
                  [sample(30 + t / 30, head=(0, 0.05, 0.3)) for t in range(900)]
        report, _ = module.analyse(lines(records))
        self.assertEqual(len(report["segments"]), 2)
        for seg in report["segments"]:
            self.assertAlmostEqual(seg["drift_per_minute"]["horizontal"], 0, places=6)
        self.assertTrue(any("segment 1: eye sits 30 cm" in w for w in report["warnings"]))

    def test_actions_and_gestures_are_counted(self):
        records = [HEADER, sample(0), sample(1),
                   event(0.2, "button", name="A_dodge_confirm", down=True), event(0.3, "button", name="A_dodge_confirm", down=False),
                   event(0.4, "button", name="LB_parry_block", down=True), event(0.5, "guard_start", hand=[0, 0, 0.3], angle_degrees=80),
                   event(0.6, "grenade_release", outcome="thrown", speed=3.0, forward_fraction=0.8),
                   event(0.7, "grenade_release", outcome="cancelled", speed=0.4, forward_fraction=0.1),
                   event(0.8, "camera_mode", mode="scripted", anchored=False)]
        report, _ = module.analyse(lines(records))
        self.assertEqual(report["presses"], {"A_dodge_confirm": 1, "LB_parry_block": 1})
        self.assertEqual(report["gestures"], {"guard_start": 1, "grenade_thrown": 1, "grenade_cancelled": 1})

    def test_tuning_warnings(self):
        records = [HEADER] + [sample(t, residual=0.05) for t in range(20)] + \
                  [event(1 + i * 0.1, "guard_rejected_angle", hand=[0, 0, 0.3], angle_degrees=30) for i in range(6)] + \
                  [event(5 + i, "grenade_release", outcome="cancelled", speed=0.5, forward_fraction=0.1) for i in range(3)]
        report, _ = module.analyse(lines(records))
        text = "\n".join(report["warnings"])
        self.assertIn("left hand: IK misses", text)
        self.assertIn("consider a lower min_angle_degrees", text)
        self.assertIn("grenade: 3 put back vs 0 thrown", text)

    def test_body_scale_readings_and_crouched_recenter_warning(self):
        records = [HEADER, sample(0), sample(1),
                   event(0.2, "body_scale", reading="accepted", player_eye_height_metres=1.45, factor=1.1338),
                   event(0.6, "body_scale", reading="crouched", player_eye_height_metres=1.45, factor=1.1338)]
        report, events = module.analyse(lines(records))
        self.assertEqual(report["body_scale"]["factor"], 1.1338)
        self.assertEqual([r["reading"] for r in report["body_scale"]["readings"]], ["accepted", "crouched"])
        self.assertTrue(any("body scale: 1 recenter(s) not measured (t=0.6 crouched)" in w for w in report["warnings"]))
        self.assertIn("eye heights 1.45; final factor 1.134", module.text(report, events, False))

    def test_room_walking_summary(self):
        def walking(t, steps, error, blocked=False, held=0.0, rebases=1, rebased=0.4):
            s = sample(t)
            s["camera"].update({"anchored": True, "room_steps": steps, "room_error_metres": error, "room_blocked": blocked,
                                "room_held_metres": held, "room_rebases": rebases, "room_rebased_metres": rebased, "room_consumed": [0, 0, 0]})
            return s
        records = [HEADER, walking(0, 10, 0.1), walking(1, 40, 0.3, blocked=True, held=0.25), walking(2, 40, 0.1, held=0.5, rebases=3, rebased=0.9)]
        report, events = module.analyse(lines(records))
        room = report["room_walking"]
        self.assertEqual((room["steps"], room["rebases"]), (30, 2))
        self.assertAlmostEqual(room["held_metres"], 0.5)
        self.assertAlmostEqual(room["largest_rebase_metres"], 0.9)
        self.assertAlmostEqual(room["blocked_share"], 1 / 3)
        self.assertAlmostEqual(room["head_from_body_metres"]["max"], 0.3)
        self.assertIn("play space walking: 30 steps", module.text(report, events, False))

    def test_room_walking_absent_from_old_traces(self):
        report, events = module.analyse(lines([HEADER, sample(0), sample(1)]))
        self.assertIsNone(report["room_walking"])
        self.assertNotIn("play space walking", module.text(report, events, False))

    def test_weapon_that_floats_after_a_switch_is_named(self):
        def with_weapons(t, pistol_distance, pistol_moved):
            s = sample(t)
            s["character"]["weapons"] = [
                {"name": "Gun_pose", "final": [0, 0, 0], "to_right_hand_metres": 0.05},
                {"name": "GUN_R", "final": [0, 0, 0] if pistol_moved else None, "to_right_hand_metres": pistol_distance}]
            s["weapon_ray"] = {"overridden": True}
            return s
        records = [HEADER] + [with_weapons(t / 10, 0.05, True) for t in range(50)] + \
                  [event(5.0, "button", name="left_secondary", down=True)] + \
                  [with_weapons(5 + t / 10, 0.6, False) for t in range(1, 40)]
        report, events = module.analyse(lines(records))
        switch = report["weapon_switches"][0]
        self.assertEqual(switch["trigger"], "left_secondary")
        self.assertAlmostEqual(switch["bones"]["GUN_R"]["before"], 0.05)
        self.assertAlmostEqual(switch["bones"]["GUN_R"]["after"], 0.6)
        self.assertEqual(switch["bones"]["GUN_R"]["moved_by_mod_after"], 0)
        self.assertAlmostEqual(switch["bones"]["Gun_pose"]["after"], 0.05)
        text = "\n".join(report["warnings"])
        self.assertIn("GUN_R is 60 cm from the right hand (was 5 cm), not moved by the mod", text)
        self.assertNotIn("Gun_pose is", text)
        self.assertEqual(report["weapon_ray"], {"samples": 89, "overridden": 1.0})
        self.assertIn("weapon switch t=5.0 s (left_secondary)", module.text(report, events, False))

    def test_takeovers_are_reported_and_left_out_of_drift(self):
        records = [HEADER]
        for t in range(1800):
            s = sample(t / 30)
            if 600 <= t < 660:  # Two seconds of a pounce: head bone pulled 40 cm away.
                s["metrics"]["head_offset_metres"] = [0.3, -0.2, 0.4]
                s["character"]["animation_takeover"] = True
            records.append(s)
        records += [event(20.0, "animation_takeover_start", head_deviation_metres=0.4, inferred=True),
                    event(22.0, "animation_takeover_end", duration_ms=2000, peak_head_deviation_metres=0.45),
                    event(30.0, "game_state", available=True, cinematic=True, paused=False),
                    event(35.0, "marker", detail="overlay button")]
        report, events = module.analyse(lines(records))
        seg = report["segments"][0]
        self.assertAlmostEqual(seg["head_offset"]["horizontal"]["max"], 0.1)
        self.assertAlmostEqual(seg["drift_per_minute"]["horizontal"], 0, places=6)
        self.assertEqual(report["game"]["takeovers"], 1)
        self.assertAlmostEqual(report["game"]["takeover_seconds"], 2.0)
        self.assertEqual(report["game"]["cinematics"], 1)
        self.assertNotIn("animation_takeover_start", report["gestures"])
        self.assertIn("1 inferred animation takeovers (2.0 s total", module.text(report, events, False))

    def test_short_segments_get_no_drift_slope(self):
        records = [HEADER] + [sample(t / 30, head=(0, 0.05, 0.1 + t * 0.01)) for t in range(90)]  # 3 s of fast change
        report, events = module.analyse(lines(records))
        self.assertIsNone(report["segments"][0]["drift_per_minute"])
        self.assertFalse(any("drifts" in w for w in report["warnings"]))
        out = module.text(report, events, False)
        self.assertIn("1 segments shorter than 15 s", out)
        self.assertNotIn("drift per minute", out)

    def test_running_lean_grouped_by_measured_speed(self):
        def moving(t, x, lean_native, lean_final, eye_z, eye_z_final):
            s = sample(t)
            s["character"].update({"root": [x, 0, 0], "units_per_metre": 1, "lean_degrees": {"native": lean_native, "final": lean_final}})
            s["metrics"]["head_offset_metres"] = [0, 0.2, eye_z]
            s["metrics"]["head_offset_final_metres"] = [0, 0.2, eye_z_final]
            return s
        standing = [moving(t / 30, 0, 1.0, 1.0, 0.15, 0.15) for t in range(60)]
        running = [moving(2 + t / 30, 5 * t / 30, 10.0, 10.0, -0.17, -0.17) for t in range(60)]  # 5 m/s
        report, _ = module.analyse(lines([HEADER] + standing + running))
        self.assertEqual(report["lean"]["running"]["samples"], 59)
        self.assertAlmostEqual(report["lean"]["running"]["lean_native_degrees"], 10.0)
        self.assertAlmostEqual(report["lean"]["standing"]["eye_forward_of_head_metres"], 0.15)
        self.assertTrue(any("running leans the spine 10.0 deg" in w for w in report["warnings"]))
        corrected = [moving(2 + t / 30, 5 * t / 30, 10.0, 0.5, -0.17, 0.14) for t in range(60)]
        report, _ = module.analyse(lines([HEADER] + standing + corrected))
        self.assertAlmostEqual(report["lean"]["running"]["eye_forward_of_published_head_metres"], 0.14)
        self.assertFalse(any("running leans" in w for w in report["warnings"]), "an upright-corrected run is not flagged")

    def test_shoulder_twist_by_speed(self):
        # docs/PAULDRON.md: native stance twist (left shoulder leading) and what was published.
        def standing(t, native, final):
            s = sample(t)
            s["character"].update({"root": [0, 0, 0], "units_per_metre": 1, "twist_degrees": {"native": native, "final": final}})
            return s
        rows = [standing(t / 30, -48.6, 0.0) for t in range(60)]
        report, events = module.analyse(lines([HEADER] + rows))
        self.assertAlmostEqual(report["lean"]["standing"]["twist_native_degrees"], 48.6)
        self.assertEqual(report["lean"]["standing"]["twist_final_degrees"], 0.0, "a squared publication reads 0, not missing")
        self.assertIn("shoulder twist native +48.6 deg, published +0.0 deg", module.text(report, events, False))
        old = [sample(t / 30) for t in range(60)]
        for s in old:
            s["character"].update({"root": [0, 0, 0], "units_per_metre": 1})
        report, events = module.analyse(lines([HEADER] + old))
        self.assertIsNone(report["lean"]["standing"]["twist_native_degrees"])
        self.assertNotIn("shoulder twist", module.text(report, events, False), "older traces print no twist")

    def test_sprint_clearance_by_pad_state_and_speed(self):
        # Joe's 20 Oct trace in miniature: BACKup +0.02 m ahead of the root walking, +0.24 m sprinting;
        # the eye stays 0.25 m ahead of the root unless the sprint camera adds its offset.
        def moving(t, x, sprint_pad, backup, offset=0.0, takeover=False):
            s = sample(t)
            eye = x + 0.25 + offset
            s["input"].update({"pad": {"buttons": 0x40 if sprint_pad else 0}, "sprint_camera": {"offset_metres": offset}})
            s["character"].update({"root": [x, 0, 0], "root_z_axis": [1, 0, 0], "units_per_metre": 0.9 / 0.9, "head": [x + 0.6, 1.8, 0],
                                   "animation_takeover": takeover, "body": [{"name": "BACKup", "native": [x + backup, 1.5, 0]},
                                                                            {"name": "NECK", "native": [x + 0.5, 1.7, 0]}]})
            s["camera"]["eye_centre"] = [eye, 1.9, 0]
            return s
        walking = [moving(t / 30, 2 * t / 30, False, 0.02) for t in range(60)]
        onset = [moving(2 + t / 30, 4 + 5 * t / 30, True, 0.2) for t in range(10)]  # Pad held, still at running speed.
        sprinting = [moving(3 + t / 30, 6 + 8.6 * t / 30, True, 0.24, takeover=True) for t in range(60)]
        report, _ = module.analyse(lines([HEADER] + walking + onset + sprinting))
        walk, sprint = report["sprint"]["walking"], report["sprint"]["sprinting"]
        self.assertEqual(walk["samples"], 59)
        self.assertEqual(sprint["samples"], 59, "the onset (pad held below sprint speed) is left out; takeover does not hide sprinting")
        self.assertAlmostEqual(walk["eye_forward_of_BACKup_metres"], 0.23)
        self.assertAlmostEqual(sprint["BACKup_forward_of_root_metres"], 0.24)
        self.assertAlmostEqual(sprint["eye_forward_of_BACKup_metres"], 0.01)
        self.assertAlmostEqual(sprint["eye_forward_of_HEAD_metres"], -0.35)
        self.assertTrue(any("sprinting: the eye is only 1 cm ahead of BACKup" in w for w in report["warnings"]))
        self.assertIn("sprinting clearance: 59 samples", module.text(report, [], False))
        offset = [moving(3 + t / 30, 6 + 8.6 * t / 30, True, 0.24, offset=0.20) for t in range(60)]
        report, _ = module.analyse(lines([HEADER] + walking + offset))
        self.assertAlmostEqual(report["sprint"]["sprinting"]["eye_forward_of_BACKup_metres"], 0.21)
        self.assertAlmostEqual(report["sprint"]["sprinting"]["sprint_offset_metres"], 0.20)
        self.assertFalse(any("sprinting: the eye" in w for w in report["warnings"]), "0.20 m of sprint offset clears the warning")

    def test_animset_change_counts_as_a_switch(self):
        report, _ = module.analyse(lines([HEADER, sample(0), event(1, "animset", animset=3), sample(2)]))
        self.assertEqual(report["weapon_switches"][0]["trigger"], "animset 3")

    def test_missing_data_and_malformed_lines_do_not_crash(self):
        empty = {"t": 1, "type": "sample", "input": None, "character": None, "camera": None,
                 "metrics": {"head_offset_metres": None, "left_target_offset_metres": None, "right_target_offset_metres": None,
                             "left_residual_metres": None, "right_residual_metres": None}}
        report, _ = module.analyse(lines([HEADER, empty]) + ["{not json", ""])
        self.assertEqual(report["malformed"], 1)
        self.assertEqual(report["coverage"]["head_offset"], 0)
        self.assertNotIn("head_offset", report["segments"][0])
        self.assertTrue(any("head offset only available in 0%" in w for w in report["warnings"]))

    def test_text_report_lists_events_with_wall_clock(self):
        report, events = module.analyse(lines([HEADER, sample(0), sample(65), event(62.5, "guard_start", hand=[0, 0, 0], angle_degrees=70)]))
        out = module.text(report, events, True)
        self.assertIn("14:01:02 guard_start", out)
        self.assertIn("eye minus head bone", out)

    def test_reads_a_line_written_by_the_mod(self):
        # Copied from the C++ trace writer (tests/test_vr_trace.cpp sample_line) to pin the schema.
        line = ('{"t":1.500,"type":"sample","input":{"age_ms":10.0,"epoch":3,"armed":true,"head":[0.0000,1.7000,0.0000],"head_forward":[-0.0000,-0.0000,-1.0000],'
                '"left":null,"left_forward":null,"right":[0.0000,0.0000,0.0000],"right_forward":[-0.0000,-0.0000,-1.0000]},'
                '"character":{"age_ms":10.0,"actor":"abc","driven":false,"units_per_metre":1.0000},'
                '"camera":{"age_ms":10.0,"mode":"body","anchored":true,"facing_error_degrees":0.00},'
                '"metrics":{"head_offset_metres":[0.0000,0.0500,0.1000],"left_target_offset_metres":null,"right_target_offset_metres":[0.0000,0.0000,0.1000],'
                '"left_residual_metres":null,"right_residual_metres":0.0200}}')
        report, _ = module.analyse([json.dumps(HEADER), line])
        self.assertEqual(report["samples"], 1)
        self.assertAlmostEqual(report["segments"][0]["head_offset"]["z"]["median"], 0.1)
        self.assertAlmostEqual(report["hands"]["right"]["ik_residual"]["median"], 0.02)


if __name__ == "__main__":
    unittest.main()
