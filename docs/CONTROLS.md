# Controls

There are two ways to play. **Motion controls** (first person, Index controllers) are the tables
below; Quest and Rift S controllers differ only where [Touch controllers](#touch-controllers-quest-rift-s)
says. **Chase camera** (gamepad) is [its own section](#chase-camera-gamepad) at the end.

First person with `-Controls -Gestures`. These tables come from the code (`mod/src/VrControllerInput.h`,
`mod/src/VrSession.cpp`, `config/bindings_index.json`). Where another document disagrees, these tables
are correct; [Changing controls](DEVELOPING.md#changing-controls-and-gestures) explains how to keep them that way.

"Tap" acts on **release**. "Hold" acts once, after 1 second, while still held, and never also taps.

Left-handed? Set **Main hand** in the SM2VR app. Left and right then swap in every table here,
holsters and gestures included: [LEFT_HANDED](LEFT_HANDED.md).

## Left controller

| Input | Does | Native pad |
|---|---|---|
| Stick | Walk, relative to where you look. Walking around your room moves the Marine too (`room_enabled`) | Left stick |
| Stick click | Sprint (toggle; forward only, within ±40°) | LS click |
| A tap | Class ability. Aimed ones (Vanguard grapnel, Tactical auspex scan) go where you look. Assault: the jump pack's dash along the ground (a tap is too short for the leap) | Y |
| A hold 1 s | Recenter the headset; stand straight, it measures your height. Not needed after loading: putting the headset on recenters by itself | none |
| B press | Pause menu. A brush under 0.07 s does nothing, and neither does a press that also catches left A (both opened it mid-fight). B again closes it | Start |
| B hold 1 s | Viewer mode: freeze the game, fly a free camera | none |
| Trigger | Gun out: aim/zoom. Pistol out: fire the pistol | LT / RT |
| Grip at the belt | Grab a grenade; release during a throw to throw it. A slow release puts it back without parrying or rejecting a mission | X on throw |
| Grip on the primary gun body | Hold two-handed aiming at the chosen grab point. The padded region follows native grip positions and the gun origin; it is not the full mesh outline. Support holds the game's aimed mode and releasing it restores manual trigger aim | LT full |
| Grip elsewhere | No direct parry/block input. Grip is for grabbing guns, grenades and holsters | none |
| Forearm raised across the face, hand close to the face | Guard (parry/block); starts no lower than 40 cm below the eyes in the body-scaled zone. Lower the hand to release and rearm | LB held |
| Open hand thrust straight out from the chest (sword out) | Knockback: the held strike. At least 30 cm at 2 m/s, ending 40 cm out at chest height. A fist, a squeezed grip or a raised guard is not a push; needs finger tracking ([details](COMBAT_CONTROLS.md)) | D-pad up |
| Trackpad press up | Class ability; held, aimed ones aim at where you look and fire on release. Assault: hold it a moment (a fifth of a second) to leap into the air with the jump pack, onto the landing marker where you look; swing the sword in the air to slam down | Y |
| Trackpad press down | Weapon cycle | D-pad down |
| Trackpad press right | Stim: press firmly on the right side of the pad. One press, no hold. [Details](#stims-and-pickups) | D-pad right |
| Trackpad press left | Scoreboard | Back |

## Right controller

| Input | Does | Native pad |
|---|---|---|
| Stick left/right | Snap turn 30° | none |
| Stick click | Tap: finisher / target lock, at once. Hold 0.5 s: also detonates a placed melta charge. The game's prompt shows its X button, which no controller's X sends; this is the control for it, and the prompt says so. Holds X for as long as the click is held, at least 0.4 s (the game's detonate needs X held 0.3 s). With no charge out, the long hold uses your equipment instead, as a trackpad press would: it can throw a grenade or place a charge | RS click, X held from 0.5 s |
| A | Dodge in the stick direction (backwards with the stick centred); menu confirm. Held, it is the same one dodge, at 0.35 s, and never a run: sprint is the left stick click | A |
| B | Hold: interact (pick up, activate, open the armoury). Tap: reload. In a cutscene: hold to skip (the game's prompt may say Hold Enter). The game chooses the target from its own camera, which follows your gaze (not your hand; [docs/INTERACTION.md](INTERACTION.md)) | B |
| Trigger | Primary out: fire. Pistol/chainsword out: rev the chainsword; pull depth sets tooth speed and volume (a finger resting on the trigger doesn't rev) | RT / none |
| Swing (pistol/chainsword out) | The hand must move, a wrist flick does nothing. Swings may start from the hip or shoulder; lifting the sword (straight up, or up and back) is not a swing; slashes can follow each other without stopping. Both hands together on the sword work. Trigger released (a resting finger counts as released): light attack. Trigger held: power attack. A left-hand punch does nothing. The game turns your Marine onto the enemy nearest where you look (or where the stick points) for the strike | RB / D-pad up |
| Hard swing with the bolter out | Light attack: triggers released for 0.3 s, a strong stroke below eye level (aiming and recoil never count) | RB |
| Grip at the right hip | Swap weapons, both ways: primary to pistol, pistol to primary (Joe, 31 Oct: the hip only; a grip over the shoulder no longer draws). A squeeze that starts while the thumb presses A, B, the stick or the trackpad never swaps: pressing A squeezes the Index grip | D-pad down |
| Grip, fist raised well above the head (35 cm above the eyes) | Emote (shout and fist raise) | D-pad left |
| Trackpad press | Grenade / equipment; held 0.3 s, it also detonates a placed melta charge. On a menu it is back | X (B on a menu) |

The pistol is the only weapon held in the left hand. A primary (bolter, sniper rifle, heavy weapon)
always stays in the right hand and on the right trigger, even when the game's animation passes it to
the left hand for a finisher or a knife strike. It changes hands only after a weapon switch (left
trackpad down, or a holster grab).

Not bound: the system buttons, right stick up and down (except in viewer mode), and trackpad touch on its own.
Grip melee (right grip = RB) exists but is off; enable it with `$env:SM2VR_MELEE='1'`.

## Stims and pickups

**Pick something up:** look at it and hold right B. The game takes about 0.2 s of hold. Ammo, stims,
weapons and relics all work this way.

**Use a stim:** press the right side of the left trackpad (Touch: right stick up). A Medicae Stimm gives
back about a third of your health over a second. You need one first: you carry up to 2, and an
Operation above the lowest difficulty starts you with none. With none, the press plays a short refusal
sound. It won't go off during a parry, finisher, gun strike, grab, stagger or stun. Restorative Stimms
use the same control.

Hints: the status strip's stim slot shows a trackpad with arrows pointing right. That's the game's own
art for a swipe, but this is a press. The tip on picking up a stim says the same in words, as does the
low-health alert. On a Touch controller the words are right and the trackpad picture isn't.

## Touch controllers (Quest, Rift S)

A Quest plays over Steam Link or another SteamVR stream; the game still renders on the PC.
SteamVR must stay the OpenXR runtime: the Meta Horizon Link app often takes it when installed,
and the installer's runtime check says so. The left controller's X and Y are the Index's
left A and B; the right controller's A and B match. Everything above applies except these rows,
because Touch has no trackpads. Not yet tried on a Quest.

| Input | Does | Native pad |
|---|---|---|
| Left X | Class ability, held for as long as X is held: a tap uses it, aimed ones aim at where you look and fire on release. Assault: a tap dashes, hold it a moment (a third of a second) to leap. Never recenters | Y |
| Left X and Y together, held 1 s | Recenter the headset. Press them together; neither the ability nor the pause menu happens | none |
| Left stick click held 1 s | Scoreboard, held while the click is held. The click still toggles sprint; the scoreboard stops it | Back |
| Right stick up | Stim | D-pad right |
| Grip | A button, not a force sensor: left grip grabs a gun or grenade; use forearm guarding to parry | grab / LT while supported |
| Grenade | The belt grab-and-throw only (there is no right trackpad) | X on throw |
| Weapon cycle | The right hip grab only (there is no left trackpad) | D-pad down |

## Menus

The pause menu and the full-screen menus that keep the world running (the Battle Barge's mission
select, training, the Datavault) show whole on a panel fixed in the room. The face buttons are the
pad's four: right A = A (confirm), right B = B (back, also closes the pause menu), left A = X (held),
left B tap = Y, except on the pause menu, where left B is Start and closes it. Y there, for Edit Squad
(your bots), is left trackpad up; the prompt says so. In the hub, left X sends Y as well. The grips are the shoulder buttons: right grip = RB, left grip = LB (menu tabs).
Both sticks are the pad's sticks (no snap turn on a menu); the triggers and the
trackpad navigate too. The left stick moves the game's cursor, the right stick scrolls. Turning, sprint, the ability,
recenter and all gestures are off while a menu is open. Holding left B for a second still opens
viewer mode from the pause menu.

## Viewer mode (Left B hold)

The game pauses into its own photo mode and you fly a free camera
([docs/VIEWER_MODE.md](VIEWER_MODE.md)). The frozen Marine keeps your pose, faces your way and looks where you looked:

| Input | Does |
|---|---|
| Left stick | Slide, relative to where you look |
| Right stick | Left/right snap turns, up/down flies |
| One grip | Drag the world |
| Both grips | Scale the world (hands apart = bigger) and turn it |
| Right trigger | Next preset scale: life size, 1:12 action figure, world four times bigger |
| Right A | Let the game run while you watch from where you are, with sound: an execution plays to its end, then you are back in the game (3 s if nothing moves; 10 s at most). Press again to freeze at once ([details](VIEWER_MODE.md#30-oct-watch-it-play-out-right-a)) |
| Right stick click | Save a picture of what your left eye sees, also while the game runs. One picture a click, and nothing shows in the headset, so check the folder afterwards. It goes in your Pictures folder, `Pictures\Space Marine 2 VR`, as `SM2VR_<date>_<time>.png`. No HUD or panels in it |
| Left B tap or hold | Back to the game (also while it runs) |

## Keyboard

| Key | Does |
|---|---|
| F11 | Mod overlay (closed at start): status, calibration sliders, gesture tuning, "Mark moment in trace" |
| F8 | Cycle camera: first person, diorama, shoulder ([docs/THIRD_PERSON.md](THIRD_PERSON.md)). Does nothing in the chase camera |
| F9 | Recenter |

In diorama and shoulder views the controllers are off and a normal gamepad plays the game.
Its B is interact and reload and its X is the grenade, as the prompts show (the chase camera keeps
the stock layout). Its Back button recenters. In diorama and shoulder, hold Back and press D-pad up/down to make the world
bigger/smaller. The diorama also recenters by itself once your character walks off the table
(`diorama_recenter`, [docs/THIRD_PERSON.md](THIRD_PERSON.md)).

## Chase camera (gamepad)

The game's own over-the-shoulder camera in the headset. The Index controllers do nothing; an
Xbox-compatible gamepad plays the game exactly as on a flat screen. Start it with the installer's
**Play: chase camera** button, or `-ChaseCam` from the launcher.

| Input | Does |
|---|---|
| Gamepad, every stick, trigger and button except Back/View | Its normal game action, unchanged |
| Right stick | Turns the camera, as on a flat screen. Your head turns the view on top of that |
| Back/View | Recenter the headset (it no longer opens the game's own Back/View menu) |
| Back/View held + D-pad up/down | World bigger/smaller, from 2x life size to 1/20 (tabletop). The game doesn't see these D-pad presses |
| F9 | Recenter the headset |
| Head | Look around and lean; the camera's up/down tilt follows your head, not the game |

## Two-hand aiming

Two-hand aiming is enabled in the installer profile. Grab within the padded region along the primary gun body to hold native aimed mode; release grip to return to manual trigger aim. The support hand stays at the chosen grab point. The region approximates the body from native grip positions and the gun origin; barrel tips and protruding attachments may be outside it. Grip never directly parries in gameplay. Use forearm guarding, with a floor 40 cm below the eyes in the body-scaled zone. Returning a grenade sends no LB tap; menu grips retain their tab controls.
