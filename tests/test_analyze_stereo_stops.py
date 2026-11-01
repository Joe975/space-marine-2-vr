import importlib.util
import re
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tool = load("analyze_stereo_stops", "scripts/analyze_stereo_stops.py")

# Verbatim (trimmed) lines from build/run/20261020_201329_master_hangar_awake_retry/sm2vr.log.
NEUROTHROPE = """\
[20:27:09.480] [I] [tid:71320] VrSession: status=OpenXR tracking active; session state=5 epoch=34 submitted_pairs=13559 per_eye_hz=34.5 failed_transfers=0
[20:27:14.619] [I] [tid:71320] OrdinaryHeadsetProducer: job=30006 accepted=true refusal=0 packet_refusal=0 submitted=13713 volume_shared=0 clock_skips=4 provider_returned=true provider_ms=0.3384 tracking_age_ms=0.3374 expired_pre_add_retry=false producer_stopped=false
[20:27:17.165] [E] [tid:71320] OrdinaryHeadsetStopped: dispatch=30072 packet_refusal=0 bloom_refusal=6 scene_calls=2 scene_refused=0 scene_dropped=0
[20:27:18.111] [I] [tid:71320] OrdinaryHeadsetProducer: job=30072 accepted=true refusal=0 packet_refusal=999 submitted=13781 volume_shared=0 clock_skips=4 provider_returned=true provider_ms=0.4949 tracking_age_ms=0.493 expired_pre_add_retry=false producer_stopped=true
[20:27:19.489] [I] [tid:71320] Sm2Probe: presents=36920 fps=40.6 ui_draws=36920
[20:27:19.492] [I] [tid:71320] VrSession: status=OpenXR tracking active; session state=5 epoch=34 submitted_pairs=13781 per_eye_hz=14.0 failed_transfers=0
[20:27:24.497] [I] [tid:71320] Sm2Probe: presents=37120 fps=40.0 ui_draws=37120
[20:27:24.500] [I] [tid:71320] VrSession: status=OpenXR tracking active; session state=5 epoch=34 submitted_pairs=13781 per_eye_hz=0.0 failed_transfers=0
[20:27:29.505] [I] [tid:71320] Sm2Probe: presents=37320 fps=40.0 ui_draws=37320
[20:27:53.349] [I] [tid:71320] OrdinaryLighting: dispatch=30000 slot=1 complete=true
"""

RECOVERED = """\
[21:00:00.000] [I] [tid:1] VrSession: status=OpenXR tracking active; session state=5 epoch=1 submitted_pairs=100 per_eye_hz=40
[21:00:01.000] [E] [tid:1] OrdinaryHeadsetSuspended: dispatch=500 bloom_refusal=6 contract_failure=shape contract_difference=0x500 last_accepted=499 episode=1 refused_attempts=0 created_variant=0 observed_variant=2
[21:00:01.250] [W] [tid:1] OrdinaryBloomContractRebased: records=0x11a0ee32000->0x11b701ec000 record_count=117->125 rebases=1 episode=1 (native record table moved; owned bloom contract otherwise identical)
[21:00:01.200] [I] [tid:1] VrFlatFallback: active=true reason=suspended submitted=100 frames=0
[21:00:03.000] [I] [tid:1] OrdinaryHeadsetRecovered: dispatch=560 previous=499 episode=1 attempts=1 refused_attempts=0
[21:00:03.100] [I] [tid:1] VrFlatFallback: active=false reason=none submitted=101 frames=40
[21:00:04.000] [I] [tid:1] OrdinaryStereoRecovery: suspended=false exhausted=false episodes=1 attempts=1 refused_attempts=0 recoveries=1 probe_refusals=0 probe_difference=0x0 suspended_records=2 dropped_pairs=2 bloom_stopped=false
[21:00:05.000] [I] [tid:1] VrSession: status=OpenXR tracking active; session state=5 epoch=1 submitted_pairs=180 per_eye_hz=40
"""


class AnalyzeStereoStopsTests(unittest.TestCase):
    def test_neurothrope_dropout_is_a_permanent_bloom_contract_stop(self):
        r = tool.analyze(NEUROTHROPE.splitlines())
        self.assertEqual(r["verdict"], "stopped")
        self.assertEqual(r["stops"], [{"time": "20:27:17.165", "dispatch": 30072, "packet_refusal": 0, "bloom_refusal": "contract"}])
        self.assertEqual(r["first_stopped_receipt"]["job"], 30072)
        self.assertTrue(r["first_stopped_receipt"]["accepted"])
        self.assertEqual(r["last_submitted"], 13781)
        self.assertAlmostEqual(r["stereo_frozen_s"], 33.857, places=3)

    def test_suspension_and_recovery(self):
        r = tool.analyze(RECOVERED.splitlines())
        self.assertEqual(r["verdict"], "recovered")
        s = r["suspensions"][0]
        self.assertEqual((s["dispatch"], s["bloom_refusal"], s["contract_failure"]), (500, "contract", "shape"))
        self.assertEqual(s["changed"], ["records_moved", "variant_changed"])
        self.assertEqual(r["recoveries"], [{"time": "21:00:03.000", "dispatch": 560, "previous": 499}])
        self.assertEqual([f["active"] for f in r["fallback"]], [True, False])
        self.assertEqual(r["recovery"], {"episodes": 1, "attempts": 1, "refused_attempts": 0, "recoveries": 1, "suspended": False, "exhausted": False})
        self.assertEqual(r["rebases"], [{"time": "21:00:01.250", "records": "0x11a0ee32000->0x11b701ec000", "record_count": "117->125"}])
        self.assertEqual(s["packet_refusal"], 999, "older suspension lines carry no packet refusal")
        unrecovered = tool.analyze(RECOVERED.splitlines()[:3])
        self.assertEqual(unrecovered["verdict"], "suspended")

    def test_level_transition_packet_refusal_suspends_and_recovers(self):
        # The 23 Oct 08:45:58 refusal as it is logged once packet refusals 4/12/13 suspend.
        lines = [
            "[08:45:58.572] [E] [tid:1] OrdinaryHeadsetSuspended: dispatch=16422 bloom_refusal=12 packet_refusal=4 contract_failure=none contract_difference=0x0 last_accepted=16421 episode=1 refused_attempts=0 created_variant=0 observed_variant=-1",
            "[08:46:20.000] [I] [tid:1] OrdinaryHeadsetRecovered: dispatch=17100 previous=16421 episode=1 attempts=1 refused_attempts=0",
        ]
        r = tool.analyze(lines)
        self.assertEqual(r["verdict"], "recovered")
        s = r["suspensions"][0]
        self.assertEqual((s["dispatch"], s["bloom_refusal"], s["packet_refusal"], s["changed"]), (16422, "packet", 4, []))

    def test_older_logs_without_stop_lines_report_frozen_stereo(self):
        lines = [l for l in NEUROTHROPE.splitlines() if "Stopped:" not in l and "producer_stopped=true" not in l]
        self.assertEqual(tool.analyze(lines)["verdict"], "frozen")
        idle = [l for l in lines if "Sm2Probe:" not in l]
        self.assertEqual(tool.analyze(idle)["verdict"], "clean", "a session that stopped presenting is not a stereo freeze")
        self.assertEqual(tool.analyze(RECOVERED.splitlines())["stereo_frozen_s"], 0.0)

    def test_difference_names_match_the_native_enum(self):
        header = (ROOT / "mod/src/NativeTextureContract.h").read_text()
        body = re.search(r"enum Difference:uint32_t \{(.*?)\};", header, re.S).group(1)
        values = {name: int(value) for name, value in re.findall(r"(\w+)=(\d+)", body)}
        self.assertEqual([values[name] for name in tool.DIFFERENCE], [1 << bit for bit in range(len(tool.DIFFERENCE))])
        self.assertEqual(len(values), len(tool.DIFFERENCE))

    def test_refusal_names_match_the_native_enums(self):
        bloom = (ROOT / "mod/src/NativeOrdinaryBloom.h").read_text()
        producer = (ROOT / "mod/src/NativeTrackedOrdinaryCamera.h").read_text()
        names = lambda text: [n.strip() for n in re.search(r"enum class Refusal:uint32_t \{(.*?)\};", text, re.S).group(1).split(",")]
        self.assertEqual(names(bloom), tool.BLOOM_REFUSAL)
        self.assertEqual(names(producer), tool.PRODUCER_REFUSAL)


if __name__ == "__main__":
    unittest.main()
