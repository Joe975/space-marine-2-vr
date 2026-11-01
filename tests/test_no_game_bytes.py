"""The repo must not carry the game's machine code: no run of 16 or more byte literals in tracked
C++ sources and tests (0x.. lists or \\x.. strings), unless the bytes are the mod's own and the line,
or the line above it, says `mod-authored`. Game code is referenced by SHA-256 (game_build::hashed).
"""
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MINIMUM = 16
BYTE = r"(?:0x[0-9a-fA-F]{1,2}|[0-9]{1,3})"
LIST = re.compile(BYTE + r"(?:\s*,\s*\{?\s*" + BYTE + r"\s*\}?){%d,}" % (MINIMUM - 1))
ESCAPES = re.compile(r"(?:\\x[0-9a-fA-F]{2}){%d,}" % MINIMUM)


def byte_runs(text):
    """(line, count) for each run of >= MINIMUM byte literals not marked mod-authored."""
    lines = text.splitlines()
    out = []
    for rx in (LIST, ESCAPES):
        for m in rx.finditer(text):
            tokens = re.findall(r"0x[0-9a-fA-F]+|\\x[0-9a-fA-F]{2}|\b\d+\b", m.group())
            if sum(t.startswith(("0x", "\\x")) for t in tokens) < MINIMUM // 2:
                continue  # plain decimal lists (sizes, indices) are not code
            if any(int(t[2:], 16) > 0xFF for t in tokens if t.startswith("0x")):
                continue
            n = text.count("\n", 0, m.start())
            context = " ".join(lines[max(0, n - 1):n + 1])
            # GameBuildTable.inc's deltas are {offset, XOR} pairs between two builds, not code.
            if "mod-authored" not in context and "constexpr Delta " not in lines[n]:
                out.append((n + 1, len(tokens)))
    return out


def tracked(*dirs):
    names = subprocess.run(["git", "-C", str(ROOT), "ls-files", *dirs], capture_output=True, text=True, check=True).stdout.split()
    return [ROOT / n for n in names if n.endswith((".h", ".hpp", ".cpp", ".inc", ".c"))]


class NoGameBytesTest(unittest.TestCase):
    def test_detector(self):
        game = "constexpr std::array<uint8_t,16> P{" + ",".join(["0x48"] * 16) + "};"
        self.assertEqual(byte_runs(game), [(1, 16)])
        self.assertEqual(byte_runs('const char* c="' + "\\x48" * 16 + '";'), [(1, 16)])
        self.assertEqual(byte_runs("// mod-authored patch\n" + game), [])
        self.assertEqual(byte_runs("std::array<int,20> sizes{" + ",".join(["1"] * 20) + "};"), [])
        self.assertEqual(byte_runs("std::array<uint8_t,15> p{" + ",".join(["0x48"] * 15) + "};"), [])
        self.assertEqual(byte_runs("inline constexpr Delta D[]{" + ",".join(["{9,0xc0}"] * 16) + "};"), [])

    def test_sources_and_tests_carry_no_game_code(self):
        found = []
        for path in tracked("mod/src", "tests"):
            for line, count in byte_runs(path.read_text(encoding="utf-8", errors="replace")):
                found.append(f"{path.relative_to(ROOT).as_posix()}:{line}: {count} byte literals")
        self.assertEqual(found, [], "game code belongs in game_build::hashed digests, not in the repo")


if __name__ == "__main__":
    unittest.main()
