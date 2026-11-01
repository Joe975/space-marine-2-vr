# Performance

Performance is poor even on the development RTX 4090. Large campaign battles can fall below
25 fps. I recommend the mod as a way to experience the game's scale and environments rather
than a good way to play through the campaign.

## Settings

Keep DLSS enabled if your card has it; TAA is the option for cards that don't. Lower presets and more aggressive DLSS modes have offered little help in
CPU-bound scenes, so I recommend high settings: going from Low to DLSS Quality and the preset below the top
took a mission from about 29 to 26 stereo pairs/s. No preset or experimental switch is a general performance
fix. Depth of field and motion blur are disabled by the VR data pack for comfort.

Occlusion culling runs for both eyes: 32% more stereo pairs/s at a held view in the hangar. The eyes share
the left eye's answer, so something hidden from the left eye behind a door frame can be missing from the right
eye until the left eye can see it.

Historic stationary measurements were about 45 stereo pairs/s in the quiet test room and
27-35 in campaign samples. They are examples of particular scenes, not minimums or a promise
for large battles. Slower GPUs and other headsets have not been benchmarked.

## What to measure

A fresh stereo pair is one game frame rendered for both eyes. Game presents, new pairs and
headset refresh are different rates. SteamVR reprojection fills display intervals; it does not
create more simulated game frames. A smooth desktop mirror does not establish headset comfort.

Compare the same checkpoint, camera pose, resolution, graphics settings and launch options.
Warm up first, use a fixed scripted pose, and check that both runs actually submit stereo.
Record frame times and pair rate along with CPU/GPU utilisation. Report medians and slow frames,
not just an instantaneous counter. Experimental queue and render paths need a separate label.

## Tools

Use the `-Profile` launcher switch and `config/test_driver/performance_stationary.json` for
repeatable captures. `scripts/benchmark_driver_performance.py` captures a stationary window;
read its command-line help for required process and output arguments.
`scripts/run_report.py` checks the log for fallback, new states, suspensions and crashes.

See [resolution](RESOLUTION.md), [the test driver](TEST_DRIVER.md), [trace](VR_TRACE.md) and
[stereo architecture](SINGLE_FRAME_STEREO.md). Raw benchmark captures are kept privately.
