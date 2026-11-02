# Combat controls

[Controls](CONTROLS.md) is the binding reference. Raw controller values enter the shared VR
session, gesture detectors choose an action, and the resulting virtual XInput pad reaches the
game's native controls. A requested attack is not proof that the game accepted or hit with it.

## Weapons and gestures

- Primaries remain in the right hand; the pistol is held in the left. Holster selection requests
  a native weapon switch rather than inventing another inventory system.
- Sword swings require arm travel. Trigger released selects a light attack; trigger held selects
  power. Chained slashes are allowed. A strong bolter-out swing has separate gates to avoid
  treating aiming or recoil as an attack.
- Grabbing the primary gun body holds native aimed mode (LT), with a rigid two-hand gun pose.
  The padded grab region is inferred from native grips and the gun origin; it is not the full
  mesh outline. Your hand stays at the chosen grab point. Releasing restores manual trigger aim.
- Forearm guard routes native parry/block; grip is for grabbing. The guard floor is 40 cm below
  the eyes in the body-scaled zone. Sword attacks and pushes
  suppress a held guard until it is rearmed. An open-palm push routes the held strike.
- Grenades are grabbed at the left belt and thrown by releasing during hand travel. The hand's
  release direction and speed influence the throw. Slow release puts the grenade back.
- Raising the right fist well above the head requests the native emote. Holster grabs and emotes
  own their selecting grip so they cannot also trigger a sword attack.

The game keeps control of body facing during a strike or parry so it can turn onto the target.
Melee feel and class-specific blocking still need tuning and headset checks.

## Class abilities

Left A taps native Y. For Assault this is a dash; hold left trackpad up for at least a fifth of
a second to reach the jump-pack leap. On Touch, left X holds native Y for as long as it is held,
so a hold past a third of a second leaps. A sword swing in flight requests the native ground pound.
Aimed abilities follow the game camera, which follows the gaze. Fast jump-pack travel rides
with the character; newer camera behaviour still needs headset comfort testing.

## Safety and tuning

Focus loss, stale input and tracking loss release controls. A tracking stall under a second
resumes with the held controls. Other disarms re-arm on the next sample, but each control held at
that moment stays blocked until it is released (`SM2VR_INPUT_STALL_RESUME_MS`;
`SM2VR_INPUT_NEUTRAL_REARM=1` restores the old whole-pad rule). Gesture parameters live in `config/*.calibration` and header
`PARAMETERS` tables; the F11 overlay exposes tuning where supported.

Core files include `VrControllerInput.h`, `VrSession.cpp`, `VrSwingMelee.h`, `VrGuard.h`,
`VrGrenadeThrow.h`, `VrGrenadeAim.h` and `VrHolster.h` under `mod/src/`.
See [Developing](DEVELOPING.md#changing-controls-and-gestures) for adding a control and its tests.
