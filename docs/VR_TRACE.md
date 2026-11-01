# Playtest trace

The trace records raw head/controller input, routed controls and the game's resulting character
poses. It helps distinguish requested actions from native outcomes and measure drift between
the view, hands and character model.

## Enable

Use `-Trace` in the developer launcher. `SM2VR_TRACE_HZ` selects 1-120 samples/s, default 30;
`SM2VR_TRACE_PATH` can set an absolute output path. The launcher archives the trace and mod log
under the run directory. F11 includes **Mark moment in trace** for a timestamped event.

The default trace path is under `%LOCALAPPDATA%\sm2vr`. Logs and traces can include local paths
and user information; review them before sharing. They are not committed to the release repo.

## Format and tools

JSON Lines begin with a header, followed by fixed-rate samples and event records. Missing
producers are null. Samples distinguish raw controls, emitted pad state, native animation poses,
tracked targets, solved hands, weapons, camera mode and scale. Source: `mod/src/VrTrace.h`.

Run `scripts/analyze_vr_trace.py <trace> --events` using the checkout's virtual environment to
summarise a run and its events. `scripts/analyze_melee_trace.py` focuses on swings and guards.
`scripts/run_report.py <run-directory>` checks stereo states, refusals, suspensions and crashes.

Use world units and `units_per_metre` consistently. A missing sample or game producer is not a
zero measurement. Correlate events with the producing frame before drawing conclusions.

See [the test driver](TEST_DRIVER.md) and [Developing](DEVELOPING.md#tests).
