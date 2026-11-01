# Same-frame stereo

The mod adds a tracked second camera to the game's native render dispatch. Both eye views use
the same simulation frame and frozen tracking sample. This avoids rendering left and right
eyes from different moments, but means much of the game rendering work runs twice.

## Image identity

Camera placement, projection, texture source, queue, completion fence and submission all belong
to the same issued stereo job. An eye is usable only when those identities agree. Retained
images keep the pose and field of view that produced them; a newer tracker sample cannot be
attached to an older image.

The runtime receives two independently rendered images. A desktop mirror, a copied flat image
or a single successful pair is not proof of continuous stereo. Tests should distinguish source
frame rate, new stereo pairs and headset refresh.

## Recovery and fallback

Loads, menus, cutscenes and game-owned picture-in-picture views can change which camera owns
the frame. Guards suspend the owned pair when readiness or camera ownership is uncertain.
The headset can show the flat game while waiting, then recover stereo when the native state is
ready. A foreign game view must not share the owned pair's rendering slots.

Native resources shared across views need explicit lifetime and per-frame handling. See
[second-eye side effects](SECOND_EYE_SIDE_EFFECTS.md). Runtime copying and lifecycle are covered
in [OpenXR](OPENXR.md).

## Contributor checks

Exercise normal play, pause/photo mode, class changes, a level transition and returning to the
title. Inspect `scripts/run_report.py` output and both-eye captures. Confirm that no suspension
is silently counted as a successful pair. Add a deterministic test for each changed guard and
repeat the corresponding real game transition. See [automation](GAME_AUTOMATION.md).
