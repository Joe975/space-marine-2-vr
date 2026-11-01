# Features and limits

Implemented means the feature exists in the source. Automated checks exercise the code and, where
available, scripted game runs. Headset playtests are needed to judge comfort and feel; a passing
test does not mean every class, weapon or mission has been played through.

| Feature | Current behaviour and limits |
|---|---|
| Stereo and head tracking | Both eyes use the same game frame. Played on Index; performance remains poor in battles. |
| First-person body and hands | The character's arms follow the controllers. Height calibration and body alignment are implemented; armour clipping and repeated class changes need more testing. |
| Guns and holsters | A right-hand grab at the hip swaps between the primary and the pistol. Primaries stay in the right hand; the pistol is held in the left. Heavy weapons and sniper support need more headset checks. |
| Chainsword | Physical swings select light or power attacks by trigger state. Trigger depth controls revs. Melee still needs tuning. |
| Guard and push | Forearm guarding and an open-palm knockback gesture. Gesture arbitration has automated coverage; class-specific blocking needs more playtests. |
| Grenades | Grab at the belt and release during a throw. Direction and force follow the hand. Different grenade types and detonation need more headset checks. |
| Emote | Raise a fist above the head for the native shout. |
| Camera mode | Freeze the game, fly around, drag and scale the world. Includes tabletop and giant-scale views. |
| Chase camera | The game's over-the-shoulder camera in stereo, controlled with a gamepad. Broader headset testing is still needed. |
| Menus and interaction | Room-fixed menu panels, gaze-directed prompts and native use/reload. Coverage varies across game screens. |
| Diorama and shoulder views | F8 on a motion-control launch: a live tabletop battle or a life-size shoulder camera, played with a gamepad. Played in the headset on 27 Oct; aiming in diorama is hard. |
| Haptics | Game rumble is routed to OpenXR controller vibration, but nothing has been felt on the VR controllers in three headset tests. A gamepad rumbles normally. |
| Cinematics and takeovers | View holds during native animations; fast jump-pack travel rides with the Marine. New behaviour needs headset comfort checks. |
| Other headsets | SteamVR OpenXR support. Quest and Rift S Touch controllers have their own layout (stim, scoreboard, the held ability and recenter move); a Quest plays over Steam Link. Only Index has been played. |

See [controls](CONTROLS.md), [known issues](INSTALL.md#known-issues) and [status](../STATUS.md).
