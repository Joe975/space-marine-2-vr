import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import check_game_build_run as c  # noqa: E402

GOOD = """[08:00:00.001] [I] [tid:1] GameBuild: exe timestamp 0x6ad10fcb (2026-10-21) known=true
[08:00:01.000] [I] [tid:2] EngineCameraProbe: game build 0x6ad10fcb (2026-10-21), add camera at rva 0x1d76e90
[08:00:01.001] [I] [tid:2] EngineCameraProbe: hooked camera-list add at 7ff6 (passive)
[08:00:01.002] [I] [tid:2] VrProjection: hooked builder at 7ff6; passive until a scoped eye is provided
[08:00:01.003] [I] [tid:2] EngineControllerRegistration: native input-update hook installed
[08:00:01.004] [I] [tid:2] EngineControllerRegistration: registered native XInput slot zero
[08:00:01.005] [I] [tid:2] CinematicState: bound=true
[08:00:01.006] [I] [tid:2] EngineBodyProbe: passive=false installed=true
[08:00:01.007] [I] [tid:2] BodyFollowing: installed=true facing_enabled=true room_enabled=false
[08:00:01.008] [I] [tid:2] PlayerOpacity: installed=true enabled=true
[08:00:05.000] [I] [tid:3] EngineCameraProbe: state=0 calls=0 call_sites=0
[08:00:09.000] [I] [tid:3] EngineCameraProbe: state=1 calls=500 call_sites=3
[08:00:09.001] [I] [tid:3] VrProjection: state=1 calls=500 overridden=480
[08:00:09.002] [I] [tid:3] ScreenUi: installed=true calls=10 overflow=0 movies=1
[08:00:09.003] [I] [tid:3] WorldMarkers: installed=true calls=5 overflow=0 corrected=5 unbound=0 readers_installed=true reader_overrides=2
[08:00:09.004] [I] [tid:3] TrackerHandleGuard: enabled=true exe_installed=true dll_installed=true exe_refused=false dll_refused=false getter_trips=0
[08:00:09.005] [I] [tid:3] ChainswordRev: bound=true actor=1 property=2 starts=0 stops=0 refused=0
[08:00:09.006] [I] [tid:3] OrdinaryHeadsetRetention: ready=true stopped=false active=true pairs=420 refusal=0
[08:00:09.007] [I] [tid:3] OrdinaryHeadset: consumed=10320 submitted=10300 dropped=19 dispatch=10680 gpu_verified=true
""".splitlines()

# 2026-10-21 11:17: one stereo pair, then the bloom guard refused and the flat panel took over.
FLAT_FALLBACK = """[11:17:34.967] [E] [tid:16880] OrdinaryHeadsetStopped: dispatch=247 packet_refusal=0 bloom_refusal=9 scene_calls=2 scene_refused=0 scene_dropped=0
[11:17:34.970] [I] [tid:16880] OrdinaryHeadsetRetention: ready=true stopped=false active=false pairs=1 refusal=0
[11:17:34.998] [I] [tid:16880] OrdinaryHeadset: consumed=1 submitted=0 dropped=0 dispatch=247 gpu_verified=true
[11:17:35.021] [I] [tid:16880] VrFlatFallback: active=true reason=stopped submitted=1 frames=0
""".splitlines()

# The 06:33 run on the patched game with the unported DLL.
BROKEN = """[06:33:57.099] [E] [tid:41756] EngineControllerRegistration: unsupported executable; leaving native input unchanged
[06:33:57.099] [I] [tid:41756] CinematicState: bound=false
[06:33:57.099] [I] [tid:41756] EngineBodyProbe: passive=false installed=false
[06:33:57.255] [E] [tid:80440] EngineCameraProbe: exe timestamp 0x6ad10fcb or function bytes differ from build 0x6ac3e716; not hooking
[06:34:04.845] [I] [tid:80440] ScreenUi: installed=false calls=0 overflow=0 movies=0
[06:34:04.845] [I] [tid:80440] EngineCameraProbe: state=2 calls=0 call_sites=0
[06:34:00.139] [I] [tid:80440] OrdinaryHeadsetRetention: ready=false stopped=false active=false pairs=0 refusal=0
""".splitlines()


class CheckRunTest(unittest.TestCase):
    def test_complete_port_passes(self):
        results = c.check(GOOD, controls=True, stereo=True)
        self.assertTrue(all(ok for _, ok in results), [n for n, ok in results if not ok])

    def test_unported_run_fails_each_hook(self):
        failed = {n for n, ok in c.check(BROKEN, controls=True, stereo=True) if not ok}
        for name in ("build identified and known", "camera list add hooked", "camera probe receiving calls",
                     "body probe installed", "cinematic state bound", "stereo pairs delivered",
                     "no a hook refused the build"):
            self.assertIn(name, failed)

    def test_flat_fallback_is_not_stereo(self):
        lines = [l for l in GOOD if "OrdinaryHeadset" not in l] + FLAT_FALLBACK
        failed = {n for n, ok in c.check(lines, stereo=True) if not ok}
        self.assertIn("stereo pairs submitted (100+)", failed)
        self.assertIn("no stereo stop", failed)

    def test_last_status_line_decides(self):
        lines = GOOD + ["[08:01:00.000] [I] [tid:3] ScreenUi: installed=false calls=0 overflow=0 movies=0"]
        self.assertIn(("screen UI hook", False), c.check(lines))

    def test_screen_ui_hook_is_optional(self):
        # 26 Oct release tidy: installed only for eye snapshots or SM2VR_DIAG_SCREEN_UI=1.
        lines = [l for l in GOOD if "ScreenUi:" not in l]
        self.assertIn(("screen UI hook", True), c.check(lines))

    def test_unknown_build_is_reported(self):
        lines = GOOD + ["[08:00:02.000] [W] [tid:9] GameBuild: unknown build; 130 of 139 signatures resolved"]
        self.assertIn(("no unknown game build", False), c.check(lines))


if __name__ == "__main__":
    unittest.main()
