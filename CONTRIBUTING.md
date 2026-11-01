# Contributing

This is a one-person fan project that is playable but far from finished, so help is welcome: bug
reports, testing on hardware I don't have, fixes, and features. This page says how to do each so
it lands.

## Ways to help

- **Report what you hit.** Open an [issue](https://github.com/Joe975/space-marine-2-vr/issues).
  Press **Make crash report** in the installer window and attach the zip it puts on your desktop
  (logs only, no memory dumps; logs can contain your Windows user name). Say which view you played
  (motion controls or chase camera), what you were doing, and your headset and GPU.
- **Test on other hardware.** So far it has only been played on a Valve Index with an RTX 4090.
  OpenXR should reach other headsets through SteamVR, and Touch bindings exist, but none of that has
  been tried. A report that says what worked, what didn't, and the stereo pairs/s you got is useful
  on its own, with no code attached.
- **Fix or build something.** Starting points: [known issues](docs/INSTALL.md#known-issues), the
  open items in [STATUS.md](STATUS.md), and the rows marked pending in
  [docs/FEATURES.md](docs/FEATURES.md). For anything bigger than a small fix, open or comment on an
  issue first so the same thing isn't built twice.

## Setting up

[docs/DEVELOPING.md](docs/DEVELOPING.md) has the toolchain, the build, the data pack, how to play
a dev build and how to run the tests. You need the game installed through Steam and a PC VR headset
with SteamVR; most of the suites run without either and skip what they can't check.

## The rules the tests enforce

The export that produces this public repo refuses anything on this list, so a change that breaks
one can't ship:

- **Nothing of the game's.** No game files (`.pak`, `.sso`, `.exe`, `.dll`, `.pdb`), no decompiler or
  disassembly output, no crash dumps. Tests that need the game's own files read them from the
  player's install (`tests/game_sources.py`) and skip when it isn't there.
- **No runs of game code bytes.** `tests/test_no_game_bytes.py` fails on any literal of 16 or more
  bytes in `mod/src` or `tests`. Where the mod has to check that game code is what it expects,
  compare a SHA-256 digest (`game_build::hashed`, checked by `tests/test_game_code_hashes.py`)
  rather than the bytes.
- **No local paths or user names** in anything committed.
- **Offline only.** The mod never touches Easy Anti-Cheat, online play or multiplayer, and nothing
  in the repo may point people at a bypass.
- **No bundled binaries.** The DLL is built from source. Third-party dependencies are pinned, and
  their licences are listed in `installer/THIRD_PARTY_NOTICES.txt`.

## Making a change

1. **Make the behaviour testable, then test it.** Add or update a test that fails without your
   change: a native suite in `tests/*.cpp` (CMake and ctest) or a Python one in `tests/*.py`
   (`unittest`, no pytest). `scripts\build-mod.bat` builds the DLL and every native suite and runs them all
   ([docs/DEVELOPING.md](docs/DEVELOPING.md#tests)).
2. **Controls** follow the six-stage recipe in
   [Changing controls and gestures](docs/DEVELOPING.md#changing-controls-and-gestures). Update the
   tables in [docs/CONTROLS.md](docs/CONTROLS.md): the installer's controls page is generated from
   them, and a test checks every row made it.
3. **Gestures** are header-only classes with a `PARAMETERS` table and a `config/*.calibration` file
   that the F11 overlay can tune live. Don't bury a new constant in code.
4. **Docs**: one page per topic in `docs/`, linked from [docs/README.md](docs/README.md). Keep
   [STATUS.md](STATUS.md) short (current state, open items, pointers). Run evidence (screenshots,
   logs, traces) goes in `docs/validation/`, which stays out of this repo.
5. **Game patches**: the mod's hooks are tied to one game build. Don't add a new address by hand;
   follow the porting procedure in [docs/GAME_PATCH_2026-10-21.md](docs/GAME_PATCH_2026-10-21.md).
6. **Commit per logical change** with a message that says why, not just what.
7. **Say how it was tested** in the pull request: played in a headset, run by the scripted test
   driver, or unit tests only. All three are fine; the difference matters to whoever plays it next.

## How pull requests land

This repo is a snapshot of a private development repo. A script exports the cleaned tree (it
leaves out run evidence and dev-only observers, and refuses anything in the rules above) and
commits it here as one snapshot at a time. A pull request is reviewed here, then ported into the
development repo and comes back in the next snapshot, credited in that commit and in the release
notes, rather than being merged directly. Your branch history won't appear in this repo. If that
matters to you, say so in the pull request.

## Agents

The mod was built mostly by coding agents (Codex and Claude Code) with a human in the headset. The
method is written up in [VR modding with agents](https://joe975.github.io/vr-modding-writeup/) and
packaged as the [vr-modding-with-agents](https://github.com/Joe975/vr-modding-with-agents) kit.
Agent-written changes are welcome under the same tests and rules; you are responsible for what you
submit, and the pull request should say what was played and what was only scripted.

## Licence

The mod's own code is MIT ([LICENSE](LICENSE)). Contributions are accepted under the same licence.
