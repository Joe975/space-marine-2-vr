import importlib.util
import json
import math
import tempfile
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("check_interaction_alignment", Path(__file__).resolve().parents[1] / "scripts" / "check_interaction_alignment.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

HEADER = {"t": 0, "type": "header", "format": "sm2vr-trace", "version": 1, "hz": 30, "wall_clock_local": "2026-10-20T20:13:58.131"}


def camera_axes(heading, pitch):
    """The game's camera family (title camera: right +X, forward -Z); heading grows to the left."""
    h, p = math.radians(heading), math.radians(pitch)
    forward_h = (-math.sin(h), 0.0, -math.cos(h))
    right = (math.cos(h), 0.0, -math.sin(h))
    forward = (forward_h[0] * math.cos(p), math.sin(p), forward_h[2] * math.cos(p))
    up = (-forward_h[0] * math.sin(p), math.cos(p), -forward_h[2] * math.sin(p))
    return right, up, forward


def to_view(world, camera, heading, pitch):
    d = [world[i] - camera[i] for i in range(3)]
    return [sum(a * b for a, b in zip(axis, d)) for axis in camera_axes(heading, pitch)]


def sample(t, eye_forward, gaze_native=None, game=(1.0, 2.0, 3.0)):
    cam = {"mode": "body", "game": list(game), "eye_forward": list(eye_forward)}
    if gaze_native is not None:
        cam["gaze_native"] = {"yaw_degrees": gaze_native[0], "pitch_degrees": gaze_native[1]}
    return {"t": t, "type": "sample", "camera": cam}


def press(t, down, name="B_interact_reload"):
    return {"t": t, "type": "event", "event": "button", "name": name, "down": down}


class InteractionAlignmentTests(unittest.TestCase):
    def test_marker_recovery_matches_the_native_camera(self):
        camera = (-157.0, 62.0, -547.0)
        for heading, pitch in ((81.1, -6.5), (-120.0, -7.0), (0.0, 10.0), (170.0, -3.0)):
            for world in ((-160.0, 61.0, -560.0), (-150.0, 66.0, -540.0), (-170.0, 63.0, -545.0)):
                solved = module.camera_from_marker(world, camera, to_view(world, camera, heading, pitch))
                self.assertIsNotNone(solved)
                self.assertAlmostEqual(module.wrap(solved[0] - heading), 0.0, places=3)
                self.assertAlmostEqual(solved[1], pitch, places=3)

    def test_wrong_camera_position_is_refused(self):
        world, camera = (-160.0, 61.0, -560.0), (-157.0, 62.0, -547.0)
        view = to_view(world, camera, 30.0, -6.5)
        self.assertIsNone(module.camera_from_marker(world, (camera[0] + 5, camera[1], camera[2]), view))

    def test_gaze_heading_convention(self):
        right, _, forward = camera_axes(0.0, 0.0)
        self.assertAlmostEqual(module.heading_pitch(forward)[0], 0.0)
        # Turning toward the camera's right axis decreases the heading.
        self.assertAlmostEqual(module.heading_pitch(right)[0], -90.0)

    def write(self, folder, lines):
        path = Path(folder) / "sm2vr_trace.jsonl"
        path.write_text("\n".join(json.dumps(l) for l in lines) + "\n", encoding="utf-8")
        return path

    def test_trace_with_gaze_native(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.write(folder, [HEADER, sample(1.0, (0, 0, -1), (150.0, -20.0)), press(1.01, True), press(1.5, False),
                                       sample(2.0, (0, 0, -1), (5.0, -3.0)), press(2.0, True), press(2.2, False)])
            result = module.analyse(path)
        rows = result["presses"]
        self.assertEqual([r["source"] for r in rows], ["trace", "trace"])
        self.assertEqual(rows[0]["hold_ms"], 490)
        self.assertEqual(rows[0]["wall"], "20:13:59.141")
        self.assertEqual(result["summary"]["outside_use_cone"], 1)
        self.assertEqual(result["summary"]["outside_focus_cone"], 1)

    def test_old_trace_recovers_native_camera_from_log(self):
        game, native_heading, native_pitch = (-157.0, 62.0, -547.0), 81.1, -6.5
        world = (-160.0, 61.0, -560.0)
        view = to_view(world, game, native_heading, native_pitch)
        gaze = camera_axes(native_heading + 170.0, -30.0)[2]  # Facing almost opposite the native camera, looking down.
        with tempfile.TemporaryDirectory() as folder:
            trace = self.write(folder, [HEADER, sample(10.0, gaze, game=game), press(10.02, True), press(12.0, False)])
            log = Path(folder) / "sm2vr.log"
            log.write_text("[20:14:08.131] [I] [tid:1] WorldMarkerCamera: caller=0xbf3d8c calls=9 camera=1 thread=2 simulation=1/1 rendering=1/5 "
                           f"presenting=0 local=0 open=1 matching=0 vr=false world=[{world[0]},{world[1]},{world[2]}] view=[{view[0]},{view[1]},{view[2]}] near=0.1\n"
                           "[20:14:08.131] [I] [tid:1] WorldMarkerCamera: caller=0xbf497a calls=9 camera=1 vr=false world=[0,0,0] view=[1,1,1]\n", encoding="utf-8")
            result = module.analyse(trace, log)
        row = result["presses"][0]
        self.assertEqual(row["source"], "log")
        self.assertAlmostEqual(abs(row["yaw_degrees"]), 170.0, places=2)
        self.assertAlmostEqual(row["pitch_degrees"], -23.5, places=2)
        self.assertEqual(result["summary"]["recovered_native_samples"], 1, "only the 2D marker path is used")

    def test_traces_from_before_interact_moved_to_native_b_still_count(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.write(folder, [HEADER, sample(1.0, (0, 0, -1), (5.0, -3.0)), press(1.0, True, "X_interact_reload"), press(1.2, False, "X_interact_reload"),
                                       press(1.5, True, "X_equipment_grenade"), press(1.6, False, "X_equipment_grenade")])
            self.assertEqual(len(module.analyse(path)["presses"]), 1, "a grenade press is not an interact")

    def test_press_without_alignment_is_reported_unknown(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.write(folder, [HEADER, sample(1.0, (0, 0, -1)), press(1.0, True)])
            result = module.analyse(path)
        self.assertIsNone(result["presses"][0]["yaw_degrees"])
        self.assertEqual(result["summary"]["measured"], 0)


def follow_sample(t, yaw, pitch=0.0, rx=0, mode="body", gaze_heading=0.0, gaze_pitch=0.0):
    return {"t": t, "type": "sample", "input": {"pad": {"buttons": 0, "rx": rx, "ry": 0}},
            "camera": {"mode": mode, "gaze_native": {"yaw_degrees": yaw, "pitch_degrees": pitch},
                       "eye_forward": list(camera_axes(gaze_heading, gaze_pitch)[2])}}


def look(seconds_moving=1.0, degrees=90.0, catch_up_s=0.5, park=1.9, hz=30, still_s=2.0, stick_after=None, start_heading=0.0):
    """Gaze turns `degrees` right at a constant rate, the camera lags 15 degrees while it
    moves and catches up `catch_up_s` after the gaze stops, then parks at `park` degrees."""
    rows, t, dt = [], 0.0, 1.0 / hz
    for _ in range(int(0.5 * hz)):  # Still before the look.
        rows.append(follow_sample(t, park, gaze_heading=start_heading)); t += dt
    steps = int(seconds_moving * hz)
    for i in range(1, steps + 1):
        rows.append(follow_sample(t, 15.0, rx=32767, gaze_heading=start_heading - degrees * i / steps)); t += dt
    stop = t
    while t < stop + catch_up_s:
        rows.append(follow_sample(t, 15.0 * (1 - (t - stop) / catch_up_s) + 2, rx=20000, gaze_heading=start_heading - degrees)); t += dt
    end = t + still_s
    while t < end:
        rx = 9000 if stick_after is not None and t > stop + catch_up_s + stick_after else 0
        rows.append(follow_sample(t, park, rx=rx, gaze_heading=start_heading - degrees)); t += dt
    return rows


class FollowTests(unittest.TestCase):
    def test_a_tracked_look_settles_after_the_gaze_stops_and_rests(self):
        result = module.follow(look())
        self.assertEqual(result["looks"], 1, "looks are found from the gaze moving, not from a large error")
        l = result["settles"][0]
        self.assertAlmostEqual(l["gaze_degrees"], 90.0, places=0)
        self.assertEqual(l["tracking_lag_degrees"], 15.0)
        self.assertLessEqual(l["settle_seconds"], 0.55)
        self.assertGreater(result["rest_samples"], 30)
        self.assertTrue(result["stick_rests_when_settled"])
        self.assertTrue(result["passed"])

    def test_parking_inside_the_reengage_band_is_settled(self):
        # 21 October rerun: pitch held at 3.8 degrees (inside the controller's 5 degree band).
        self.assertTrue(module.follow(look(park=3.8))["passed"])

    def test_slow_catch_up_fails(self):
        result = module.follow(look(catch_up_s=2.0))
        self.assertEqual(result["slow_looks"], 1, "15 degrees left at the stop may take 0.79 s, not 2 s")
        self.assertFalse(result["passed"])

    def test_never_reaching_the_band_fails(self):
        result = module.follow(look(park=6.5))
        self.assertEqual(result["unsettled"], 1, "parking outside the re-engage band (the first live run) never settles")
        self.assertFalse(result["passed"])

    def test_hunting_while_the_gaze_is_still_fails(self):
        result = module.follow(look(stick_after=0.8))
        self.assertGreater(result["hunting_samples"], 0)
        self.assertFalse(result["stick_rests_when_settled"])
        self.assertFalse(result["passed"])

    def test_corrections_while_the_gaze_moves_are_tracking(self):
        # The look itself sends full stick inside 15 degrees: not hunting.
        self.assertEqual(module.follow(look())["hunting_samples"], 0)

    def test_small_gaze_movements_are_not_looks(self):
        result = module.follow(look(degrees=6.0))
        self.assertEqual(result["looks"], 0)
        self.assertFalse(result["passed"], "no look, no evidence")

    def test_two_looks_are_judged_separately(self):
        first = look()
        offset = first[-1]["t"] + 1 / 30
        second = look(degrees=35.0, start_heading=-90.0)
        for r in second:
            r["t"] += offset
        result = module.follow(first + second)
        self.assertEqual(result["looks"], 2)
        self.assertTrue(result["passed"])

    def test_cutscene_samples_are_ignored(self):
        rows = look()
        for r in rows:
            r["camera"]["mode"] = "scripted"
        self.assertEqual(module.follow(rows)["looks"], 0)

    def test_follow_scenario_is_a_valid_scripted_head_request(self):
        root = Path(__file__).resolve().parents[1]
        spec_driver = importlib.util.spec_from_file_location("vr_test_driver", root / "scripts" / "vr_test_driver.py")
        driver = importlib.util.module_from_spec(spec_driver)
        spec_driver.loader.exec_module(driver)
        script = json.loads((root / "config/test_driver/gaze_camera_follow.json").read_text())
        self.assertEqual(script["mode"], "scripted_head")
        yaws = {f["head"][3] for f in script["frames"] if "head" in f}
        pitches = {f["head"][4] for f in script["frames"] if "head" in f}
        self.assertTrue({90, -90, 0} <= yaws and -35 in pitches, "left, right, ahead and a look down")
        self.assertTrue(any(f.get("turn") for f in script["frames"]) and any(f.get("move") for f in script["frames"]))
        self.assertTrue(driver.compile_script(script, "gaze-follow"))


if __name__ == "__main__":
    unittest.main()
