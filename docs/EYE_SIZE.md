# Experimental eye output sizing

The normal launcher leaves the game's output size alone. Developer output sizing requires all
three switches: `-OutputSize WIDTHxHEIGHT`, `-OutputSizeMode config` and
`-ExperimentalOutputSize`. Dimensions must be valid even integers. Mode must be `config` or
`device`; `config` is the default mode when an output size is supplied.

## Contract

The mod's requested size and the game's settings transaction must agree. The launcher refuses
incomplete selections and conflicting desktop resolution options before starting the game.
The settings transaction restores the original files afterwards. It cannot be used with
`-NoClose`, because restoration depends on owning the complete launch lifecycle.

Resizing a runtime swapchain alone does not resize all game render targets or fix the game
camera's projection. Incorrect output aspect can produce crop or zoom. `device` and `config`
are experimental paths with different engine behaviour; neither should be described as a
confirmed optimisation for normal play.

## Verification

Check actual game output size, both-eye projection, aspect, field of view and settings restoration.
Compare at a fixed camera pose and label the output-size experiment in any benchmark.
`tests/test_launcher_ui_mode.py` covers refusal and forwarding; the settings tests cover rollback.

See [resolution](RESOLUTION.md) and [performance](PERFORMANCE.md).
