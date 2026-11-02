# Status

This is an experimental PC VR mod. I recommend it for the experience of standing in the game
and exploring its scale. Large battles are slow even on an RTX 4090.

Version 1.0.0 is the first release baseline. It remains a rough alpha: the version number starts
the release sequence and does not mean the campaign or every control has been tested.

## Supported setup

- Space Marine 2 on Steam, patch 15.2. The installer checks the exact supported game build.
- SteamVR as the OpenXR runtime. Valve Index is the headset used for development and playtests.
- Index controllers for first person, or an Xbox-compatible gamepad for the chase camera and the
  diorama and shoulder views. Quest and Rift S Touch controllers have their own layout, not yet
  played.
- Offline campaign only.

## Current behaviour

First person has tracked hands, wrist-aimed guns, weapon holsters, sword swings, trigger-controlled
chainsword revs, forearm guarding, belt grenade throws and a raised-fist emote. Camera mode
pauses the game and lets you fly, drag, scale and turn the battlefield. F8 swaps first person for
a live diorama or a life-size shoulder view, played with a gamepad.

The source also includes guards for level transitions, game-owned picture-in-picture views,
duplicate texture releases and stereo recovery. Automated checks cover these paths; they do not
establish that every campaign mission works or that the latest changes feel right in a headset.

## Known limits

- Large battles can fall below 25 fps. Slower GPUs and other headsets have not been measured.
- Crashes, slow startup and loss of stereo during a mission remain possible.
- Melee feel, class abilities, heavy-weapon support and behaviour across repeated scene changes
  still need more headset testing.
- Controller vibration doesn't reach the VR controllers yet (three headset tests). A gamepad
  rumbles normally.
- The Touch controller layout hasn't been played.
- Walking around your room moves the Marine, but stairs and bumping allies are untested.
- The diorama and shoulder cameras have no collision and can sit inside walls.

See [installation and known issues](docs/INSTALL.md), [controls](docs/CONTROLS.md),
[feature coverage](docs/FEATURES.md) and [contributing](CONTRIBUTING.md).

This repository contains cleaned source snapshots. Development diaries, agent handoffs and raw
run evidence are maintained privately. A source snapshot does not create a new installer release;
use the versioned asset on the [Releases page](https://github.com/Joe975/space-marine-2-vr/releases).
