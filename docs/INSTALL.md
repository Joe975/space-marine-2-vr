# Install and play

Everything a player needs: requirements, the installer, what to expect, and where to report
problems. Developers building from source: [DEVELOPING.md](DEVELOPING.md).

## What you need

- **Space Marine 2 on Steam**, on the exact game version the release supports. The installer
  window checks it. Steam updates the game on its own, and the mod refuses to start on a version
  it doesn't know (its hooks are tied to one build), so after a game patch you wait for a mod
  update. There's no way to roll the game back. Supported now: **patch 15.2**, the Steam build of
  21 October 2026 (`CLIENT 15.2.PATCH.1454884` in the window title, `Retail.exe` timestamp
  `0x6ad10fcb`). The 5 October build, patch 15.1, still works too.
- **A PC VR headset with SteamVR set as the OpenXR runtime.** It's been played on a Valve Index.
  OpenXR is the default and should reach other headsets through SteamVR, but none have been
  tried yet.
- **Valve Index controllers for motion controls.** Index is the tested setup. Quest and Rift S
  Touch controllers have their own layout ([Touch controllers](CONTROLS.md#touch-controllers-quest-rift-s)),
  but haven't been played. A Quest plays over Steam Link, with SteamVR as the OpenXR runtime.
  The chase camera and the third-person views need an Xbox-compatible gamepad instead.
- **A strong GPU.** It's developed on an RTX 4090 at Ultra Performance DLSS. Campaign fights run at
  a median of about 27-35 stereo pairs/s there (each pair is one game frame shown to both eyes),
  and the quiet test room about 45. SteamVR reprojection fills the gaps. Slower cards haven't
  been measured.
- **DLSS or TAA, in the game's own video settings** (Resolution Upscaling). Use DLSS if your card
  has it: it's sharper, and about 12% faster in my one comparison on the 4090. AMD, Intel and older
  NVIDIA cards can try TAA. That's new, and it has only been tested on the 4090, so nobody knows yet
  how it runs on those cards. FSR doesn't work: the headset shows the flat game until you pick DLSS
  or TAA, and then 3D returns without a restart.
  The mod doesn't change your graphics settings, so set them before you play. Each eye renders
  at the game's output resolution, so Native render resolution costs about a fifth of the frame
  rate on the 4090 at 4K (56 fps against
  69-70 with DLSS Performance). Which DLSS mode you pick hardly matters, the game is CPU-bound;
  Quality looks best for the same frame rate. Lower graphics presets buy very little either, so I
  recommend high settings: going from Low to DLSS Quality and the preset below the top took a
  mission on the 4090 from about 29 to 26 stereo pairs/s, and it looks far better.

## Steps

1. Download `SM2VR-<version>.zip` from [Releases](https://github.com/Joe975/space-marine-2-vr/releases) and extract it anywhere outside
   the game folder.
2. Double-click `SM2VR.cmd`. A window opens with five steps. Work down them; each one says what's
   wrong and how to fix it.
   1. **Game found**: finds Space Marine 2 through Steam.
   2. **Steam and SteamVR ready**: Steam running, SteamVR the OpenXR runtime.
   3. **Game version supported**: your game build matches this release.
   4. **Install**: backs up your saves, then builds the VR data pack from your own game files.
   5. **Play in VR**: the first time, it asks you to pick the mod's controller binding in SteamVR.
      Then pick **Play: motion controls** (first person, Index controllers as your hands) or
      **Play: chase camera** (gamepad). You can switch views every time you play without
      reinstalling.
3. The first start takes 3-4 minutes. Put the headset on when the window says it's waiting for
   the headset.

When you close the game the mod's files are removed from the game folder, so a normal Steam launch
never sees them. If the game or the PC crashes, the files are removed the next time you open
`SM2VR.cmd`.

**Saves.** The mod plays on your normal saves. Before installing and before each update it copies
them to `%LOCALAPPDATA%\sm2vr\player\save-backups`. To restore one, close the game and copy its
`storage` folder back over `%LOCALAPPDATA%\Saber\Space Marine 2\storage`.

**Uninstall.** Press Uninstall in the window, then delete the folder. Save backups and logs stay in
`%LOCALAPPDATA%\sm2vr\player` until you delete them.

**Another mod already uses `dxgi.dll`** (ReShade, Special K, OptiScaler): SM2VR won't overwrite it.
Remove that mod first.

## Offline only

Play the campaign offline. Modded launches start the game without Easy Anti-Cheat; the mod doesn't
patch or bypass it, and a normal Steam launch is untouched. Don't try to use the mod online.

## Known issues

It's playable, not finished. What you'll most likely hit:

- Occasional crashes while a level loads. The headset shows the flat game image for several
  seconds after a load before 3D comes back.
- Now and then a launch stalls before the headset view starts. Close it and play again.
- VR can drop out partway through a campaign mission.
- Frame rate in big fights ([what you need](#what-you-need)), so expect reprojection.
- Some props and decals are missing from the right eye.
- Controller vibration doesn't reach the VR controllers yet (three headset tests). A gamepad
  rumbles normally.
- Several default-on features (height scaling on recenter, the starting height after
  loading, gaze camera, guard guide) have only been tested by scripts, not played much in the
  headset yet.

**Comfort.** The body turns with your head and you move with the stick, with snap turn. If you get
motion sick easily, start with short sessions, or try the chase camera.

Use at your own risk. No warranty of any kind.

## Reporting bugs

Open a [GitHub issue](https://github.com/Joe975/space-marine-2-vr/issues). Press **Make crash report** in the installer window and attach
the zip it puts on your desktop (logs only, no memory dumps; logs can contain your Windows user
name). Say which view you played and what you were doing.
