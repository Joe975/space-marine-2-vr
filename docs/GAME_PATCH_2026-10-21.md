# Porting to a game patch

The mod's native hooks are tied to exact builds. The supported Steam patch is 15.2
(`Retail.exe` timestamp `0x6ad10fcb`); the 5 October patch 15.1 reference build is also supported.
Unknown builds fail closed. The filename is retained for existing contributor links.

## Address and hash contract

Use the shared build table and `game_build::hashed` checks. Store offsets and SHA-256 digests,
not copied game instructions. A matching name, nearby offset or similar prologue is not enough
to accept a new hook. Keep ABI, calling convention and complete validated code-range checks.

Tests read game files from the contributor's own install. Game images, decompiles, disassembly
and extracted files stay in local ignored storage and must not be committed or shipped.

## Porting the next patch

1. Identify both old and new game builds and preserve their images locally before Steam updates.
2. Map functions with the binary-diff tooling in `scripts/build_diff.py`; inspect its help for
   mapping and analysis commands. Use `scripts/build_signatures.py` for local signatures.
3. Review every used function, global and call site. Put the new offsets in the shared table;
   do not scatter build-specific constants across feature implementations.
4. Recompute guarded digests from the player's build. Preserve failure on changed code and
   unsupported builds. Recheck data-pack hashes and rebuild packs from the updated game files.
5. Run native suites, `tests/test_game_code_hashes.py` and `tests/test_no_game_bytes.py`.
   Read the supported game sources through `tests/game_sources.py`.
6. Run the [tour](GAME_AUTOMATION.md) and real controls/scene-transition checks. Require fresh
   stereo pairs and both-eye images, rather than accepting a flat fallback as stereo.
7. Update supported-build documentation and release metadata only after verification. Record
   what was played in a headset and what was checked automatically.

An address mapping is a starting point for review, not evidence that a new build is compatible.
See [Developing](DEVELOPING.md#tests) and [second-eye side effects](SECOND_EYE_SIDE_EFFECTS.md).
