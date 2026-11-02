SM2VR - Warhammer 40,000: Space Marine 2 in VR
================================================

Unofficial, non-commercial fan mod. Not affiliated with or endorsed by Saber Interactive,
Focus Entertainment or Games Workshop. The game: https://store.steampowered.com/app/2183900/

Offline campaign only. Modded launches start the game without Easy Anti-Cheat; the mod does
not patch or bypass it, and a normal Steam launch never sees the mod. Don't use it online.

Use at your own risk. No warranty of any kind.

You need
--------
- Space Marine 2 on Steam, on the game version this release supports (the window checks).
  Steam updates the game on its own; after a game patch the mod refuses to start until a
  mod update supports the new version. The game can't be rolled back. Supported now: patch
  15.2, the Steam build of 21 October 2026 (CLIENT 15.2.PATCH.1454884 in the window title).
- A PC VR headset with SteamVR set as the OpenXR runtime (played on a Valve Index; other
  headsets untested).
- Valve Index controllers for motion controls (Touch untested), or an Xbox-compatible
  gamepad for the chase camera.
- A strong GPU. Developed on an RTX 4090; campaign fights run at about 27-35 stereo pairs
  per second there, with SteamVR reprojection filling the gaps.
- DLSS or TAA, in the game's own video settings. Use DLSS if your card has it; AMD, Intel
  and older NVIDIA cards can try TAA, which has only been tested on the 4090. FSR doesn't
  work: the headset shows the flat game until you pick DLSS or TAA, and then 3D returns
  without a restart.
  The mod does not change your graphics settings, so set them before you play. Each eye
  renders at your monitor's resolution: a 1080p monitor gives a soft image, 4K a sharp one.
  For a sharper image on a smaller monitor, turn on DSR (NVIDIA) or VSR (AMD) in your graphics
  driver and set your Windows desktop to 3840x2160 before you start. Go no higher: above
  4096x2160 the headset shows the flat game. Changing resolution during a session also
  flattens the headset until you restart the game.
  Turn Frame Generation off. Native render resolution costs about a fifth of the frame rate. The DLSS mode you pick hardly matters (the game is CPU-bound); Quality
  looks best for the same frame rate. High graphics settings are worth it: going up from
  Low cost about a tenth of the frame rate in a mission on the 4090.

Start
-----
1. Extract this folder anywhere you like (not inside the game folder).
2. Double-click SM2VR.cmd. A window opens with five steps. Windows may warn you first, because
   the mod isn't signed: press More info, then Run anyway. SM2VR.cmd is a short text file you can
   read in Notepad.
3. Follow the steps top to bottom. Each one says what is wrong and how to fix it.
   - Install builds the VR data pack from your own game files and backs up your saves.
4. Press one of the two Play buttons. The first start takes 3-4 minutes. Put the headset on
   when the window says it is waiting for the headset.
   - Play: motion controls. First person: you are the Marine, with the Index controllers as
     his hands. F8 in game switches between first person, diorama and shoulder; the last two
     use a gamepad.
   - Play: chase camera. The game's own over-the-shoulder camera in VR, played with a gamepad
     exactly as on a flat screen. Back/View on the gamepad recenters the view.
   Pick either one each time you play; nothing needs reinstalling to switch.

When you close the game, the mod's files are removed from the game folder. If the game or the
PC crashes, the files stay until you next open SM2VR, so open it once before a normal Steam launch.

Controls
--------
Press Controls in the window. In the game: Left A tap is your class ability, hold it to
recenter. Left B tap opens the pause menu. In camera mode (hold Left B on the pause menu) a
right stick click saves a picture to Pictures\Space Marine 2 VR. Nothing shows in the headset, so
check the folder afterwards.

Your saves
----------
The mod plays on your normal saves. Before installing and before each update SM2VR copies
them to %LOCALAPPDATA%\sm2vr\player\save-backups. To restore one, close the game and copy
its "storage" folder back over %LOCALAPPDATA%\Saber\Space Marine 2\storage.

Known issues
------------
- An occasional crash while a level loads.
- Now and then a launch stalls before the headset view starts. Close it and play again.
- VR can drop out partway through a campaign mission.
- In the armoury the Marine can go black until you pick another item.
- Some props and decals are missing from the right eye.
- Comfort: you move with the stick and turn with snap turn, and walking around your room moves
  the Marine too. Against a wall the Marine stops and your view stops soon after, while your
  head carries on: step back. If you get motion sick easily, start with short sessions or try
  the chase camera. To turn room walking off, set room_enabled=0 in
  %LOCALAPPDATA%\sm2vr\player\config\index_camera_guard.calibration; it applies within a second.

Problems
--------
- "This game version is not the one this mod release supports": Space Marine 2 updated.
  Wait for a mod update, then press Check for updates.
- Another mod's files are beside the game (ReShade, Special K, OptiScaler): SM2VR needs
  dxgi.dll and will not overwrite them. Remove that mod first.
- Something went wrong in game: press Make crash report. It puts a zip on your desktop with
  the logs (no memory dumps). Logs can contain your Windows user name. Attach it to an issue
  on the mod's GitHub page.

Uninstall
---------
Press Uninstall, then delete this folder. Your save backups and logs stay in
%LOCALAPPDATA%\sm2vr\player until you delete them, and pictures stay in your Pictures folder.
Your F11 tuning goes with the mod.

License
-------
SM2VR's own code is MIT licensed. Third-party components: THIRD_PARTY_NOTICES.txt and the
licenses folder.
