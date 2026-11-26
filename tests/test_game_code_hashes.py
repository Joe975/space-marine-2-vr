"""Every game_build::hashed(...) digest in mod/src against the real game images.

The repo carries digests of the game's code, never the bytes, so the native suites run their guards
on synthetic code. This test closes the loop on a machine that has the RVA-mapped images of the
reference build and the ported builds (build/gamebin/old_{exe,dll}.image for the reference,
new_* for 2026-10-21, b26944055_* for 2026-11-27, made by the porting method in
docs/GAME_PATCH_2026-10-21.md): each digest is the reference build's code at its RVA, and on each
ported build the code read at the table's RVA, with the table's delta undone, hashes the same.
Every digest must verify on every ported build: a digest whose verified prefix fell short would
switch its feature off in the game without any other test noticing (docs/GAME_PATCH_2026-11-27.md).
Skipped when the reference or 2026-10-21 images are absent; a later build is checked when its images
are present. SM2VR_GAMEBIN names their folder.
"""
import hashlib
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import game_build  # noqa: E402

HASHED = re.compile(r'game_build::hashed\((0x[0-9a-fA-F]+),(\d+),"([0-9a-f]{64})"(,game_build::Image::dll)?\)')
# Image prefix in build/gamebin -> that build's timestamps. The first is required, later ones optional.
PORTED = {"new": {"exe": 0x6AD10FCB, "dll": 0x6AD110A6}, "b26944055": {"exe": 0x6AFCCA92, "dll": 0x6AFCCEC4}}
# Digests a ported build is known not to verify (prefix, file, rva): its feature stays off there.
NOT_PORTED = set()


def gamebin():
    names = [os.environ.get("SM2VR_GAMEBIN", "")]
    try:
        common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True).stdout.strip()
        main = Path(common).parent if common else ROOT
        names += [str(ROOT / "build/gamebin"), str(main / "build/gamebin")]
        names += [str(p / "build/gamebin") for p in sorted(main.parent.glob(main.name + "-wt-*"))]
    except OSError:
        pass
    for name in names:
        if name and all((Path(name) / f"{k}_{i}.image").is_file() for k in ("old", next(iter(PORTED))) for i in ("exe", "dll")):
            return Path(name)
    return None


def codes():
    """(file, rva, length, digest, image) for every digest declared in mod/src."""
    out = []
    for p in sorted((ROOT / "mod/src").glob("*")):
        if p.suffix in (".h", ".cpp"):
            for m in HASHED.finditer(p.read_text(errors="replace")):
                out.append((p.name, int(m.group(1), 16), int(m.group(2)), m.group(3), "dll" if m.group(4) else "exe"))
    return out


class GameCodeHashTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        folder = gamebin()
        if folder is None:
            raise unittest.SkipTest("reference/ported game images not found (set SM2VR_GAMEBIN)")
        cls.old = {i: (folder / f"old_{i}.image").read_bytes() for i in ("exe", "dll")}
        cls.ported = {prefix: {i: (folder / f"{prefix}_{i}.image").read_bytes() for i in ("exe", "dll")}
                      for prefix in PORTED if all((folder / f"{prefix}_{i}.image").is_file() for i in ("exe", "dll"))}

    def test_digests_are_the_reference_build_code(self):
        found = codes()
        self.assertGreater(len(found), 100)
        bad = [f"{f}: {rva:#x}+{n}" for f, rva, n, digest, image in found
               if hashlib.sha256(self.old[image][rva:rva + n]).hexdigest() != digest]
        self.assertEqual(bad, [])

    def test_ported_builds_accept_their_verified_port(self):
        for prefix, images in self.ported.items():
            with self.subTest(build=prefix):
                stamps = PORTED[prefix]
                bad, checked = [], 0
                for f, rva, n, digest, image in codes():
                    entry = game_build.ported(stamps[image], image).entries.get(rva)
                    if entry is None or entry.verified < n:
                        if (prefix, f, rva) not in NOT_PORTED:
                            bad.append(f"{f}: {rva:#x}+{n} not verified ({entry.verified if entry else 'missing'})")
                        continue
                    read = images[image][entry.rva:entry.rva + n]
                    undone = bytes(c ^ entry.delta.get(i, 0) for i, c in enumerate(read))
                    checked += 1
                    if hashlib.sha256(undone).hexdigest() != digest or not game_build.code(stamps[image], rva, read, (n, digest), image):
                        bad.append(f"{f}: {rva:#x}+{n}")
                    changed = bytes([read[0] ^ 1]) + read[1:]
                    if game_build.code(stamps[image], rva, changed, (n, digest), image):
                        bad.append(f"{f}: {rva:#x} accepts a changed byte")
                self.assertGreater(checked, 100)
                self.assertEqual(bad, [])

    def test_tracker_sentinel_is_the_getters_compare_operand(self):
        # cmp r8,[rip+disp32] at +7 of each tracker getter (NativeTrackerHandleGuard).
        for image, getter, sentinel in (("exe", 0x134C2E0, 0x4D47F00), ("dll", 0xD6C170, 0x3FC7300)):
            disp = int.from_bytes(self.old[image][getter + 10:getter + 14], "little", signed=True)
            self.assertEqual(getter + 14 + disp, sentinel, image)


if __name__ == "__main__":
    unittest.main()
