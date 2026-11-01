# Developing

Building from source, the launch options the installer sets for you, how the controls
are wired, and the tests. Players don't need any of this; they want [INSTALL.md](INSTALL.md).
How to send changes back: [CONTRIBUTING.md](../CONTRIBUTING.md). Run the commands below from
the repo root.

## Quick start

You need:
- Steam and SteamVR running, with the headset and both controllers tracked.
- The game installed through Steam. The scripts find it there; `SM2VR_GAME_ROOT` overrides.
- Visual Studio with the C++ tools, CMake and Ninja.
- The framework source the DLL builds against: `bash scripts/fetch-tools.sh` (Git Bash) clones
  it into the gitignored `refs/`.
- The repo `.venv` (`python -m venv .venv`). The pack builders need only the standard library.
  `scripts/voice-requirements.txt` is for voice feedback.

**1. Build the DLL.** This builds `build\mod\bin\dxgi.dll` and runs the main native test suites:

```powershell
scripts\build-mod.bat
```

**2. Build the data pack.** You only need to do this once, and again whenever `config/first_person*.json`,
`config/vr_menus.json` or `scripts/build_vr_controls.py` changes. Each step builds on the pack from the
step before; together they produce `build\vr_controls\SM2VR_FirstPerson_Test.pak`.
The four steps are: first-person camera, motion blur, depth of field and lens distortion off, menu panel,
VR controls. Rebuild from step 2 when `scripts/build_vr_diagnostics.py` changes.

```powershell
$r=(Get-Location).Path; $py="$r\.venv\Scripts\python.exe"
$paks='<game folder>\client_pc\root\paks\client\default'
& $py -I "$r\scripts\build_first_person.py" --camera-only --source "$paks\default_other.pak" --preset "$r\config\first_person_wide_fov.json" --out "$r\build\first_person\wide_fov_camera\SM2VR_FirstPerson_Test.pak"
& $py -I "$r\scripts\build_vr_diagnostics.py" --camera-pack "$r\build\first_person\wide_fov_camera\SM2VR_FirstPerson_Test.pak" --presets-pack "$paks\default_presets.pak" --out "$r\build\first_person\wide_fov_diagnostics\SM2VR_FirstPerson_Test.pak"
& $py -I "$r\scripts\build_vr_menus.py" --base-pack "$r\build\first_person\wide_fov_diagnostics\SM2VR_FirstPerson_Test.pak" --source "$paks\default_other.pak" --preset "$r\config\vr_menus.json" --out "$r\build\vr_menus\SM2VR_FirstPerson_Test.pak"
& $py -I "$r\scripts\build_vr_controls.py" --base "$r\build\vr_menus\SM2VR_FirstPerson_Test.pak" --source "$paks\default_other.pak" --out "$r\build\vr_controls\SM2VR_FirstPerson_Test.pak"
```

Each builder checks the hashes of the game files it reads and refuses to run if a game patch changed
them. Each also writes a `.json` receipt beside its pak.

**3. Play.** This is the full-feature command used for headset playtests:

```powershell
& scripts\run-ordinary-headset.ps1 -Seconds 3600 -Tag play -DirectLaunch `
  -Controls -Gestures -Trace
```

The launcher copies the DLL and the data pack into the game folder (plus `openvr_api.dll` with
`-Runtime OpenVR`), then starts the exe without EAC. OpenXR through SteamVR is the default runtime.
- With `-DirectLaunch`, the game loads straight into the modders' grey test room, about 3-4 minutes
  after launch. Leave that switch off to get the title screen and the campaign.
- **The game is closed when `-Seconds` runs out** (default 900). Quitting the game earlier also ends
  the run.
- Either way, the launcher removes the mod files and moves the log and trace to
  `build\run\<date>_<tag>\`.
- If a run is interrupted and leaves `dxgi.dll` behind, `scripts\uninstall-mod.ps1` removes it.

The first time, or after `config/vr_actions.json` changes, open SteamVR's controller bindings for the
game and pick the default Index binding. SteamVR keeps an older custom binding otherwise.

**Or play with the chase camera.** This is the game's own over-the-shoulder camera in the headset,
played with a gamepad ([docs/THIRD_PERSON.md](THIRD_PERSON.md#chase-camera-launch--chasecam)). It
needs its own pack, built from the step 2 menus pack (rebuild it whenever that one changes):

```powershell
& .venv\Scripts\python.exe -I scripts\build_chase_cam.py --base build\vr_menus\SM2VR_FirstPerson_Test.pak --out build\chase_cam\SM2VR_FirstPerson_Test.pak
& scripts\run-ordinary-headset.ps1 -Seconds 3600 -Tag chase -ChaseCam
```

**Players** don't run any of this. The installer in `installer/` builds both packs from their own
game files and has a Play button for each view ([installation](INSTALL.md)).

## Launch options

Switches of `scripts/run-ordinary-headset.ps1`. The script turns each switch into `SM2VR_*`
environment variables, which the DLL reads at startup.

| Switch | Effect |
|---|---|
| `-Seconds N` | How long the run lasts before the game is closed. Default 900. |
| `-Tag name` | Name of the run folder under `build\run\`. |
| `-DirectLaunch` | Load straight into the modders' test room (no enemies). Omit for menus and the campaign. |
| `-Controls` | Index controllers, tracked hands and arms, body follows the head, wrist-aimed weapons, holsters, sword swings, analog chainsword rev, gaze camera (the game's own camera follows where you look, so interaction targets what you look at), head hidden in first person, game rumble on the controllers. Without it you only get the stereo view and a gamepad. Needs the `vr_controls` pack. |
| `-Gestures` | Adds the left-forearm guard (parry) and the belt grenade grab-and-throw. Thrown grenades follow the hand's release direction and speed (see [combat controls](COMBAT_CONTROLS.md#weapons-and-gestures)). Needs `-Controls`. |
| `-NoGrenadeAim` | Leave grenade aim to the game: grenades then fly along the game's own camera, which stays at the spawn facing. Needs `-Gestures`. |
| `-NoChainswordRev` | Turn off the analog chainsword rev (on by default with `-Controls`: with the pistol/chainsword out, how far you pull the right trigger sets tooth speed and rev volume). |
| `-NoCompactHud` | Turn off the game HUD in the headset (on by default with `-Controls` or `-ChaseCam`: the smaller native gameplay HUD placed for the headset, plus the centre of the native HUD where the game draws the "hold X" interaction prompt). |
| `-NoGazeCamera` | Turn off the gaze camera (on by default with `-Controls`: the native right stick turns the game's own camera toward where you look, so pickups, consoles and the armoury are targeted by your gaze instead of a camera you can't see; [docs/INTERACTION.md](INTERACTION.md)). |
| `-NoUprightTorso` | Turn off the upright torso (on by default with `-Controls`: keeps the rendered upper body upright under the camera before the arms are posed). |
| `-NoHaptics`, `-HapticsGain 0..2` | Turn off, or scale, the game's rumble on the controllers (on by default with `-Controls`, OpenXR only; [runtime details](OPENXR.md#tracking-and-input)). Nothing has been felt on the VR controllers in headset tests yet. |
| `-ThumbsUp photo` or `off` | Thumbs-up. The default `play` turns a lifted thumb over a closed grip into a full thumbs-up everywhere; `photo` does it only up to the photo freeze; `off` keeps the live finger curl. Needs `-Controls`. |
| `-NativeShadows` | Keep the game's sun-shadow cascade reuse. By default both eyes refit their shadow cascades every frame to address shadows that differ between eyes. Use only for A/B. |
| `-Trace` | Writes a playtest trace of head, hands and what the mod sent ([docs/VR_TRACE.md](VR_TRACE.md)). |
| `-Runtime OpenVR` | Use OpenVR instead of the default OpenXR. Core input and rendering share the same game-facing path; runtime-specific features can differ ([OpenXR](OPENXR.md)). |
| `-KeepHeadVisible` | Don't hide the character's head and helmet in first person. |
| `-PhotoNativePose` | Photo mode (viewer) freezes the game's own arm pose instead of the player's (launch.json `photo_native_pose`). |
| `-NativeHzb` | Use the game's GPU occlusion culling as it ships, not the two-eye version. That culling drops props from the right eye. |
| `-NoTrackerGuard` | Turn off the tracker handle guard (on by default). It stops the crash at stereo start that hit about 1 launch in 12. |
| `-ChaseCam` | The game's own over-the-shoulder camera in stereo, played with a gamepad. Needs the chase pack (`scripts/build_chase_cam.py`); refuses `-Controls` ([docs/THIRD_PERSON.md](THIRD_PERSON.md#chase-camera-launch--chasecam)). |
| `-TestDriver [-ScriptedHead]` | Replace the controllers with scripted ones driven from JSON ([Testing without hands](#testing-without-hands)). Needs `-Controls`. |

Other switches (`-Profile`, `-PerformanceProbes`, `-EyeSnapshots`, `-EyeSourceControl`,
`-RenderDocCapture`, the `*Probe` and other `*Candidate` switches) are diagnostics. Read the
launcher parameter declarations and the relevant source before using them. The launcher refuses
a switch it doesn't declare, and combinations with missing prerequisites. Each launch prints a
`launch flags:` line, also saved as `launch-flags.json` in the run folder, listing every opt-out,
diagnostic and dev-tool switch in effect.

## Changing controls and gestures

### How a button press reaches the game

There is no central mapping table. A press goes through six stages, and each stage lives in one
place:

| Stage | Where | What it decides |
|---|---|---|
| 1. Actions | OpenXR: the `INDEX`/`TOUCH` tables in `mod/src/OpenXrInput.h`. OpenVR: `config/vr_actions.json` (actions; the labels SteamVR's binding UI shows) and `config/bindings_index.json` (physical Index input → action) | Which physical input feeds which named action |
| 2. Reading actions | OpenXR: `openxr_input::sample` (`OpenXrInput.h`), read in `OpenXrBackend.cpp`. OpenVR: `mod/src/OpenVrBackend.cpp` (`initialize_input`, `controllers`), analog actions in `mod/src/OpenVrCombatActions.h` | Fills one `VrControllerSample` (`mod/src/VrControllerInput.h`) per frame |
| 3. Tap/hold, gestures, priorities | `VrSession::poll_controllers` in `mod/src/VrSession.cpp` | Left A/B tap vs hold (`VrTapHold.h`, `VrLeftAButton.h`), which gesture owns a grip, pause-menu and viewer filtering, loadout trigger routing (`VrCombatRouting.h`) |
| 4. Logical → gamepad | `VrControllerInput::update` in `mod/src/VrControllerInput.h` | **The only place that sets gamepad buttons and axes**. Samples with `touch_layout` set (Touch has no trackpads) take the Touch routes for stim, scoreboard and the held ability. Left X's hold and the X+Y recenter chord are `VrTouchXButton.h` |
| 5. Injection | `mod/src/EngineControllerRegistration.cpp` | Serves that pad to the game as XInput slot 0 |
| 6. What the game does with a pad button | `scripts/build_vr_controls.py` → data pack | Native key bindings (for example: D-pad up = power attack, LB = parry only, sprint angle) |

Both runtimes fill the same `VrControllerSample`, so stages 3-6 are shared. `tests/test_openxr_bindings.py`
fails if the OpenXR Index table and `bindings_index.json` disagree.

### Recipe: add or change a mapping

1. **Physical input → action:** edit `config/bindings_index.json` and the matching `// BINDING` line in
   `mod/src/OpenXrInput.h` (Touch has its own table). For a new action, also add it to
   `config/vr_actions.json` with a label and to `openxr_input::ACTIONS`.
2. **New action:** map it in `openxr_input::sample`, and read it in `OpenVrBackend.cpp` (or, for an analog
   action, the `ANALOG` list in `OpenVrCombatActions.h`). Make it optional, as `finisher` is, so older
   bindings still load. Add a field at the **end** of `VrControllerSample`; `OpenVrBackend.cpp` fills the
   struct by position.
3. **Tap/hold or a gesture conflict:** handle it in `VrSession::poll_controllers`. Use `VrTapHold` for
   tap/hold (1 s hold, 150 ms tap pulse). Decide whether the input also passes through to the pause
   menu (pause filter near the top of the function) and to viewer mode (`poll_viewer`).
4. **Gamepad output:** set the button in `VrControllerInput::update`. Add the new field to that
   function's `neutral` check, because the pad only arms once every input is released.
5. **Native meaning:** if the gamepad button does nothing, or the wrong thing, in the game, change
   `scripts/build_vr_controls.py` and rebuild the pack ([Quick start](#quick-start) step 2).
6. **Scriptable for tests:** add the button to `vr_test::actions` in `mod/src/VrTestDriver.h` and to
   `BUTTONS` in `scripts/vr_test_driver.py`.
7. **Tests and docs:** add a case to `tests/test_vr_controller_input.cpp` or `tests/test_vr_session.cpp`
   that fails without your change. Then update the tables in [CONTROLS.md](CONTROLS.md).

### Tuning gestures

Each gesture is a header-only class with a `PARAMETERS` table (name, default, min, max). Its tuning
lives in a text `key=value` file in `config/` (`#` comments allowed). The reader rejects unknown,
missing, duplicate or out-of-range keys. The F11 overlay has sliders for each gesture; moving a slider
saves the file.

| Gesture | Code | Tuning file | Loaded |
|---|---|---|---|
| Sword swing | `VrSwingMelee.h` | `index_swing.calibration` | Live: edits apply while playing |
| Holsters, fist emote | `VrHolster.h` | `index_holster.calibration` | At start |
| Forearm guard | `VrGuard.h` | `index_guard.calibration` (missing: built-in defaults apply until a slider saves it) | At start |
| Guard guide (ghost bar) | `VrGuardGuide.h` | `index_guard_guide.calibration` (look and `enabled`; placement comes from the guard) | At start |
| Grenade grab/throw | `VrGrenadeThrow.h` | `index_grenade.calibration` (missing, as above) | At start |
| Grenade aim (gaze/hand mix, lob) | `VrGrenadeAim.h` | `index_grenade_aim.calibration` (missing, as above) | At start |
| Two-hand support | `VrSupportGrip.h` | `index_support.calibration` | At start |

The other `config/*.calibration` files cover world scale, the camera, hands, weapon fit, HUD and menus;
they are not input settings. `index.calibration` holds the head placement and Joe's
`reference_eye_height_metres` (1.644): each recenter you trigger measures your eye height and scales
first person so hands, holsters and the floor fit your body. Stand
straight when you recenter. It also includes
`sprint_forward_offset_metres` (default 0.20), which moves the view forward while sprinting so the
backpack stays out of sight, and `sprint_blend_seconds` (0.25). Both apply live, with no recenter.

## Tests

From the repo root:

```powershell
scripts\build-mod.bat                                  # build + every native suite
.venv\Scripts\python.exe -m unittest discover -s tests # Python (no pytest)
```

The launcher tests run the dev scripts under PowerShell 7 (`pwsh`) and skip without it. Tests that
read the game's own files skip unless Space Marine 2 is installed (or `SM2VR_GAME_ROOT` is set).

The input and gesture suites are `vr_controller_input`, `vr_session`, `vr_combat` and `swing_melee`;
`build-mod.bat` builds every target and runs every suite. Until 27 October it built only some, and
ctest ran whatever binary was on disk for the other 41, so a change to their code went untested.

`tests/test_controller_bindings.py` checks that every action in the manifest is bound with the right
input type. It does not check the Left A/B, Right A or sprint routes.

## Testing without hands

`-TestDriver` swaps the controllers for scripted ones and keeps every real gesture and mapping stage in
between. You still need SteamVR running; `-ScriptedHead` also scripts the head so nobody has to wear the
headset. Scenarios live in `config/test_driver/*.json`. Each is a list of frames: `ms`, `move`, `turn`,
`buttons`, `fire`, `aim`, the grips, `utility` and hand/head poses. Queue one while the game runs:

```powershell
& scripts\run-ordinary-headset.ps1 -DirectLaunch -Controls -Gestures -TestDriver -ScriptedHead
.venv\Scripts\python.exe scripts\vr_test_driver.py run config\test_driver\viewer_hold_menu.json
```

In the `buttons` list, `menu` is Left B and `recenter` is Left A. Check what happened from the trace
([docs/TEST_DRIVER.md](TEST_DRIVER.md), [docs/VR_TRACE.md](VR_TRACE.md)).

## More docs

See the [documentation index](README.md) for the current public references,
[stereo architecture](SINGLE_FRAME_STEREO.md), [runtime](OPENXR.md),
[performance](PERFORMANCE.md), [camera mode](VIEWER_MODE.md) and
[game-patch procedure](GAME_PATCH_2026-10-21.md).

Build a player package with scripts/package-installer.ps1. A source snapshot does not publish
a new installer or change repository visibility. The publishing script is separate and checks
the distribution authorisation. Development histories and raw evidence are kept privately.
