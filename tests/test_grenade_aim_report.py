import importlib.util
import json
from pathlib import Path
import unittest

PATH = Path(__file__).resolve().parents[1] / "scripts/grenade_aim_report.py"
spec = importlib.util.spec_from_file_location("grenade_aim_report", PATH)
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

NATIVE = [0.0, 4.29, -24.33]  # The game's camera aim: always the spawn facing (-Z).


def sample(t, forward):
    return json.dumps({"t": t, "type": "sample", "camera": {"eye_forward": forward, "body_yaw": 0}})


def event(t, name, **fields):
    return json.dumps({"t": t, "type": "event", "event": name, **fields})


class GrenadeAimReportTests(unittest.TestCase):
    def test_angles_are_signed_about_up(self):
        self.assertAlmostEqual(report.horizontal_angle([1, 0, 0], [0, 0, -1]), -90, places=4)
        self.assertAlmostEqual(report.horizontal_angle([0, 5, -1], [0, -.5, -1]), 0, places=4)
        self.assertIsNone(report.horizontal_angle([0, 1, 0], [0, 0, -1]))

    def test_pairs_each_throw_with_its_redirect_and_first_flight(self):
        trace = [
            sample(1, [0, 0, -1]), event(1.1, "grenade_release", outcome="thrown", velocity=[0, .5, 3]),
            event(1.1, "grenade_aim", yaw_degrees=0, pitch_degrees=7, hand_yaw_degrees=0, hand_pitch_degrees=4),
            event(2, "snap_turn", count=1), event(2.3, "snap_turn", count=2), event(2.6, "snap_turn", count=3),
            sample(3, [1, 0, 0]), event(3.1, "grenade_release", outcome="cancelled", velocity=[0, 0, .2]),
            event(3.5, "grenade_release", outcome="thrown", velocity=[0, .5, 3]),
            '{"t": 9, "type": "sample", "camera": {"eye_fo',  # Cut off when the game was killed.
        ]
        log = [
            f"[x] [I] RigidVelocity: serial=1 body=a handle=a owner_props=b system=c system_owner=d caller=12aed3e name=grenade_frag_client velocity=[{','.join(map(str, NATIVE))}]",
            "[x] [I] RigidVelocity: serial=2 body=a handle=a owner_props=b system=c system_owner=d caller=126c0b6 name=grenade_frag_client velocity=[0,3,-20]",
            f"[x] [I] GrenadeRedirect: serial=1 projectile=b before=[{','.join(map(str, NATIVE))}] after=[0,3,-24]",
            f"[x] [I] RigidVelocity: serial=9 body=e handle=e owner_props=f system=c system_owner=d caller=12aed3e name=grenade_frag_client velocity=[{','.join(map(str, NATIVE))}]",
            "[x] [I] RigidVelocity: serial=10 body=e handle=e owner_props=f system=c system_owner=d caller=126c0b6 name=grenade_frag_client velocity=[20,3,0]",
            f"[x] [I] GrenadeLaunch: redirected=2 refused=0 actor=1 projectile=f before=[{','.join(map(str, NATIVE))}] after=[12,1.5,0]",
            f"[x] [I] GrenadeRedirect: serial=2 projectile=f before=[{','.join(map(str, NATIVE))}] after=[12,1.5,0]",
        ]
        result = report.report(trace, log)
        self.assertEqual(result["redirects"], 2)
        first, second = result["throws"]
        self.assertEqual(first["snaps"], 0)
        self.assertEqual(first["aim"]["pitch_degrees"], 7)
        self.assertAlmostEqual(first["launch_from_gaze_degrees"], 0, places=3)
        self.assertEqual(second["snaps"], 3)
        self.assertAlmostEqual(second["native_before_from_gaze_degrees"], 90, places=1, msg="native aim stayed on the spawn facing")
        self.assertAlmostEqual(second["launch_from_gaze_degrees"], 0, places=3)
        self.assertEqual(second["first_flight"], [20, 3, 0], "the second launch's own flight update, not the first's")
        self.assertAlmostEqual(second["flight_from_gaze_degrees"], 0, places=3)
        native = (NATIVE[0] ** 2 + NATIVE[1] ** 2 + NATIVE[2] ** 2) ** .5
        self.assertAlmostEqual(first["launch_speed_ratio"], (9 + 576) ** .5 / native, places=4)
        self.assertAlmostEqual(second["launch_speed_ratio"], (144 + 2.25) ** .5 / native, places=4, msg="a soft throw launches slower than the game's own")

    def test_summary_lines_alone_still_give_one_redirect_each(self):
        log = [f"GrenadeLaunch: redirected={n} refused=0 actor=1 projectile=2 before=[0,4,-24] after=[{n},3,-24]" for n in (1, 1, 2, 2)]
        redirects, _ = report.read_log(log)
        self.assertEqual([r["after"][0] for r in redirects], [1, 2])


if __name__ == "__main__":
    unittest.main()
