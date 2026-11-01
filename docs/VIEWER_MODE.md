# Camera mode

Hold Left B for one second to pause into the game's photo mode and move independently of the
Marine. Explore a frozen battle at life size, turn it into a tabletop scene, or enlarge it to
inspect the armour and weapons at giant scale.

## Controls

| Input | Does |
|---|---|
| Left stick | Fly horizontally, relative to where you look |
| Right stick X | Snap turn |
| Right stick Y | Fly up or down |
| One grip | Drag the world |
| Both grips | Scale and turn the world; moving the hands apart makes it bigger |
| Right trigger click | Cycle life size, 1:12 action-figure scale and a world four times larger |
| Left B tap or hold | Return to gameplay |

Scale presets place the floor at the physical floor and use your calibrated eye height.
Dragging and scaling keep the floor grounded; flying vertically leaves the ground. Choosing
another preset grounds the view again.

## Implementation

`mod/src/VrViewer.h` owns the rig and entry/exit sequencing. It enters the native photo mode
through the pause menu and waits for the game's state to confirm each transition. Combat input
is neutral while viewing; tracker data still moves the eyes through the frozen scene.

Before freezing, the character's head is restored to full size. The current source preserves
the player's tracked arm and finger pose through that transition. That pose change still needs
headset verification.

World scale changes stereo baseline, head motion and eye height together. Each eye retains the
anchor and scale used to render it, so submission describes the source image correctly.

## Limits and verification

Photo mode must be available in the current game state. Entry can fail during cinematics or a
scene transition. Low frame rate still affects head-motion latency, especially when inspecting
small nearby objects. Avoid treating camera mode as proof that the campaign is stable.

`tests/test_vr_viewer.cpp` covers the rig, grounding, presets and transition sequencing.
Scripted scenarios are in `config/test_driver/viewer_*.json` and
`config/test_driver/animcam_viewer_*.json`. See [the test driver](TEST_DRIVER.md) for execution.
