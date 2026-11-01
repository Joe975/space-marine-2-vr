# Scripted VR input and poses

The test driver replaces controller poses and raw actions at the backend boundary. Production
gestures, arm solving, weapon aim and native input routing still run. It does not teleport the
character or count requested actions as successful game actions.

## Launch and queue

From a developer checkout, use `-TestDriver` with the normal motion-control launcher.
Add `-ScriptedHead` for unattended head poses. SteamVR is still required. Queue scenarios with
`scripts/vr_test_driver.py run <scenario.json>`; `status` reports progress and `stop` cancels.
Each script must run with the checkout's `.venv` Python. See
[Developing](DEVELOPING.md#testing-without-hands) for commands.

Physical controller actions are ignored in this mode. Focus/dashboard loss cancels input;
returning focus does not restart the scenario. Unattended head poses exercise game-side camera
behaviour rather than real tracking latency or comfort.

## Scenario format

Scenarios in `config/test_driver/` contain `mode`, `duration_ms` and ordered `frames`, beginning
at `ms: 0`. Frames specify complete states; omitted controls release.

| Field | Meaning |
|---|---|
| `move`, `turn` | Stick axes; native locomotion and snap rules still apply |
| `buttons` | Action names such as `dodge`, `interact`, `menu`, `recenter` and `finisher` |
| `fire`, `aim` | Trigger values from 0 to 1 |
| `left_grip`, `right_grip` | Grip force from 0 to 1 |
| `utility` | Trackpad x, y and press |
| `left`, `right` | Wrist x, y, z, yaw, pitch, roll relative to the level head |
| `head` | Head pose for scripted-head mode only |

Positions use metres and angles use degrees. XR axes are +X right, +Y up and -Z forward.
Poses interpolate between keyframes. Startup supplies a neutral preroll so the production input
mapper can arm. Completion and cancellation release controls.

## Check outcomes

Use [trace](VR_TRACE.md), game state and both-eye captures to establish what happened. Driver
completion alone proves only that the request ran. Require actual inventory, animation, pose or
menu-state changes for the behaviour under test. Preserve your test recipe locally; raw run
captures are excluded from source snapshots.
