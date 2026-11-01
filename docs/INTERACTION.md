# Interaction and prompts

Right B uses the game's native X command: tap to reload, hold to interact. The engine chooses
the interaction target from its camera. The default gaze-camera path follows where you look;
pointing a controller alone does not choose a use target.

The headset shows the native prompt through the VR HUD/menu path. Depending on the game state,
this can be a room-fixed panel or a prompt in the world. Mission and class menus have different
layouts and still need wider playtesting.

## Contributor checks

Test a pickup, an armoury, a mission marker and a hold-to-use prompt. Confirm the game target
changes with the gaze, the prompt remains visible in both eyes, and the right B tap/hold reaches
the intended native action. Check menu focus separately; gameplay gestures are disabled there.

See [Controls](CONTROLS.md), `mod/src/VrGazeCamera.h`, the native interaction path, and
[the test driver](TEST_DRIVER.md). Automated input routing does not establish prompt legibility.
