# Menu and transition checks

`scripts/drive-game.ps1` sends keyboard input to the game window and reads the mod log to follow
startup, Continue, pause menus and leaving a level. It uses the native menu flow rather than
loading a scene behind the game.

## Recipes

Run from the checkout root:

```powershell
scripts\drive-game.ps1 -Launch "-Controls -Gestures -TestDriver -ScriptedHead -Seconds 900 -Tag exit_check" `
    -Do 'continue,shot:hangar,quit-title' -RequireStereo -Close
```

`-Launch` accepts developer launcher arguments. Do not combine it with `-DirectLaunch` or
`-AutoContinue`: those skip or compete with the menu flow being tested. Without `-Launch`,
attach to a running modded game with `-GamePid`.

Steps include `continue`, `pause:<Item>`, `quit-title`, `keys:ESC+DOWN`, `hold:W:6`,
`eyes:<name>`, `wait:<seconds>` and `shot:<name>`. `keys:F8` cycles the camera mode and `keys:F9`
recenters; `hold:KEY:seconds` holds a key, for example to walk the Marine. Check the script header
for the current options. Menu contents differ between
the Battle Barge and missions; confirm a menu before adding its recipe.

Exit 0 means the steps completed, 2 means a hang, 3 means a required VR session never started,
and 1 means another error. Completion without actual stereo is not a VR pass.

## Tour and run report

`scripts/tour.ps1` checks startup, the barge, pause/resume and returning to title, then runs the
strict report. `scripts/run_report.py <run-directory>` also works on ordinary playtest logs.
Review states marked `NEW` before learning them into `config/known_run_states.json`.

## Limits

The game must have focus; mouse or keyboard use can steal it. SteamVR standby can prevent an
unattended headset session. A hung game window must not be used for a blocking screenshot.
Let the launcher finish so it removes installed mod files. If interrupted, use
`scripts/uninstall-mod.ps1` before another launch.

Generated screenshots, stacks, dumps and logs belong in local build output. Do not commit them
to this repository. See [stereo](SINGLE_FRAME_STEREO.md) and [the test driver](TEST_DRIVER.md).
