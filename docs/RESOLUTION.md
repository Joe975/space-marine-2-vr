# Resolution and DLSS

The game's output resolution, its internal DLSS render resolution and the headset's display
resolution are separate settings. Each eye is rendered through the game before being copied to
the runtime. Changing SteamVR's recommended resolution does not necessarily resize the game's
own output or its internal render targets.

## Player settings

Enable DLSS in the game's video menu. Cards without DLSS can try TAA, which has only been tested on
the development RTX 4090; FSR shows the flat game. Quality can look better than more aggressive modes without
a material frame-rate cost when the scene is CPU-bound. Native rendering costs more GPU time;
lower graphics presets do not remove the CPU cost of large levels and many enemies.

The installer does not overwrite your graphics preferences. Test changes at the same checkpoint
and compare fresh stereo pairs, not the desktop counter alone. See [performance](PERFORMANCE.md).

## Experimental output sizing

The developer launcher accepts `-OutputSize WIDTHxHEIGHT`, `-OutputSizeMode config` and
`-ExperimentalOutputSize`. This changes the game's output through a guarded settings transaction,
not just the runtime swapchain. Read [eye sizing](EYE_SIZE.md) before using it.

The option is experimental because output aspect changes can crop or zoom the view. It is not
part of the normal player setup and has no established general performance benefit.
