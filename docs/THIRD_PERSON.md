# Third-person views

## Chase camera launch (`-ChaseCam`)

Use **Play: chase camera** in the installer to put the game's own over-the-shoulder camera in
the headset. An Xbox-compatible gamepad keeps the native controls. The Index controllers do
nothing in this mode. Back/View recentres the headset. Hold Back/View and press D-pad up or down
to make the world bigger or smaller, from twice life size down to a 1/20 tabletop. The game's HUD
shows as in first person.

This is a separate launch mode. Its data pack retains the stock game's camera instead of moving
it into the helmet. The launcher refuses `-ChaseCam` together with `-Controls`.

Headset rotation and lean remain active. Native pitch is levelled; the headset supplies pitch
and roll. F8 does nothing during a chase-camera launch. Scripted shots can hand the view to the
game's own cinematic camera.

Developers build the chase pack with `scripts/build_chase_cam.py` from the VR menus pack.
See [Developing](DEVELOPING.md#quick-start) for the build commands.

## Diorama and shoulder views

In a motion-control launch, F8 cycles first person, diorama and shoulder views. These use the
same stereo pipeline. Diorama fixes the observer above and behind the character; shoulder follows
the native head camera with an offset. Both suspend motion controls and use a gamepad.

F9 or gamepad Back/View recentres; in diorama, hold Back/View and press D-pad up or down to scale
the world. The diorama table stays where it was placed while the Marine walks; a recenter moves
it back under him, turned to his heading. It recenters by itself once he has walked 1.5 m across
the default table; change or turn that off with `diorama_recenter` in the camera-mode settings
(`0` is off). `diorama_follow=1.5` turns on an automatic slide after he walks that far. The
shoulder view is always life-size. Returning
to first person restores the saved motion-control preference; any control held at that moment
stays blocked until it is released. These views differ from the stock chase-camera launch
because they still use the first-person data pack.

In every view the game's crosshair is drawn where the gun's aim actually hits, facing you, so it
sits on the target from any angle. In first person with motion controls that is where the gun in
your hand is pointing, so it leaves your view when you point the gun away. It carries whatever the
game shows on and around it: reload, the magazine count, the plasma heat bars, hit markers. The rest
of the game's HUD stays in front of you. While a pickup prompt or marker is next to the crosshair's
usual place, the crosshair also shows there briefly, so the prompt stays whole. Its size is set in
`config/index_hud_reticle.calibration` (`height`, `width`, `ammo_width`, `ammo_below`); the file is
read while the game runs. Set `first_person=0` there to keep it on the HUD in first person, and
`stand_aside=0` to stop the second crosshair. On the Battle Barge it stays on the HUD.

## Limits

Gamepad camera motion can be uncomfortable in VR. Start with a short session. Automated camera
and input checks do not establish comfort or full campaign compatibility.

There is no camera collision: the diorama and shoulder cameras can sit inside walls or scenery,
most often the shoulder view in narrow rooms.

Implementation: `mod/src/VrBodyFollowing.h`, the camera-mode settings and
`scripts/run-ordinary-headset.ps1`. Bindings are listed in [Controls](CONTROLS.md).
