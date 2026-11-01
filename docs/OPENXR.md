# OpenXR backend

OpenXR is the default runtime. SteamVR must already be running and selected as the active
Windows OpenXR runtime. The mod does not change the runtime registry. `-Runtime OpenVR` selects
the alternate developer backend for a launch.

## Tracking and input

Tracking uses standing STAGE space. Grip poses are converted to the shared controller origin
through `config/openxr_index_grip.calibration`. Gestures, arm tracking, weapon aim and menu
controls then use the same game-facing path as OpenVR.

`mod/src/OpenXrInput.h` defines the actions and suggested Index and Touch bindings. Finger curls
use `XR_EXT_hand_tracking` when available. Only Index has been played; bindings on other
controllers still need hardware testing. The Touch profile sets `touch_layout`, so the samples
take the Touch routes for the controls that live on Index trackpads. Game rumble is routed to
OpenXR haptic actions and the runtime accepts every pulse, but nothing has been felt on the VR
controllers in three headset tests.

## Images and lifecycle

The backend checks D3D12 adapter and device requirements before binding the game's device and
queue. Each retained eye carries its source head pose, eye offset and projection. Submission
must describe that exact image, and duplicate or mixed-frame pairs are rejected.

Reference-space changes invalidate tracking. Swapchain timeouts retain the acquisition for a
later wait, and every begun frame is ended. Runtime image copies are fence guarded; teardown
does not free resources still in use by the GPU.

The injected-runtime tests cover input, lifecycle, pose preservation, two distinct D3D12 eye
copies, timeouts and error paths. They supplement hardware testing rather than establish comfort.
See [stereo rendering](SINGLE_FRAME_STEREO.md) and [Developing](DEVELOPING.md#tests).
