# Left-handed play

Choose **Main hand** in the SM2VR app. Left-handed motion controls require OpenXR.
The choice is stored with app settings and survives replacement of the package.

Primaries move to the left hand and the pistol/sword roles mirror. Support grabs,
grenades, holsters and gestures follow their new roles. The controls tables describe
right-handed play; swap left and right for left-handed play.

Choose **Left, walk on left stick** to mirror the hands and buttons while keeping
walking on the left stick. Normal **Left** also mirrors the sticks.

The dev launcher has `-LeftHanded` and `-LeftHandedKeepSticks`. They need `-Controls`
and the OpenXR runtime. The chase camera remains gamepad-controlled.

See [Controls](CONTROLS.md) and [installation](INSTALL.md).
