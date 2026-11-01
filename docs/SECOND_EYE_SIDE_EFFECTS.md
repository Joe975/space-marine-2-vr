# Second-eye side effects

The second eye runs the game's native rendering code again within one simulation frame.
Work that belongs to a frame rather than a view must not advance twice. A renderer change
therefore needs both an image check and a lifetime/state check.

## Guards

`NativePoolReleaseGuard` avoids duplicate release of mode-2 pool records that are no longer
in the in-use list. `PoolReleaseGuard:` reports calls and skipped call sites; `PoolHealth:`
reports pool counts and ordering. New skipped callers or steadily growing pool use need review.

Exposure/adaptation state and temporal histories require consistent per-eye handling. The
per-eye call census helps identify work that unexpectedly ran twice or was missing.
`ForeignViewHold.h` suspends stereo when a game-owned picture-in-picture view would conflict
with the owned pair's slots, then waits for readiness before resuming.

## Review a change

Check retained resources, fences, reference counts, history-bank parity, viewport tags and
native callbacks. Repeating a call with apparently unchanged arguments does not establish that
it is safe twice. Avoid freeing a resource until its actual GPU completion is known.

Add a deterministic test at the native boundary, then run the real trigger: a level load,
class menu, pause/photo transition or return to title. Check the run report, pool health and
both eyes. Automated coverage does not establish that every native render path is guarded.

See [stereo](SINGLE_FRAME_STEREO.md), [menu automation](GAME_AUTOMATION.md) and
`scripts/run_report.py`. Detailed investigations and raw logs are maintained privately.
