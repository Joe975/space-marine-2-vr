"""The per-build address table (mod/src/GameBuildTable.inc) and the sources that rely on it.

Deterministic and offline: no game binaries are read. The table is generated from the patched
binaries by scripts/gen_game_build_table.py; these tests check its invariants, that every
reference address the mod translates is present (a missing one fails closed in the game, which
would silently switch a feature off), that the table holds only offsets and XOR masks (no code
bytes), the porting scanner on a synthetic .text, and the RVA porter's decision rules on a
synthetic diff.
"""
import hashlib
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import game_build  # noqa: E402
import scan_game_build  # noqa: E402
from port_rvas import BuildMap  # noqa: E402

NEW_EXE, NEW_DLL = 0x6AD10FCB, 0x6AD110A6
SRC = ROOT / "mod" / "src"
DLL_REFERENCES = {0xD6C170, 0xD6C19C, 0x136A0E0, 0x3FC7300}


def source():
    return {p.name: p.read_text(errors="replace") for p in SRC.glob("*") if p.suffix in (".h", ".cpp")
            and not p.name.startswith("GameBuild")}


def translated_references():
    """Reference RVAs the sources hand to game_build (directly or via a named constant)."""
    found = set()
    for name, text in source().items():
        code = "\n".join(line.split("//")[0] for line in text.splitlines())
        for m in re.finditer(r"game_build::(?:rva|code)\((?:[\w:.]+,)?0x([0-9a-fA-F]{6,7})\b", code):
            found.add(int(m.group(1), 16))
        for m in re.finditer(r"\b\w*RVAS?\w*\s*(?:=|\{)\s*\{?([^;]*?)(?:;|\}\s*;)", code):
            found.update(int(v, 16) for v in re.findall(r"\b0x([0-9a-fA-F]{6,7})\b", m.group(1)))
        for m in re.finditer(r"\{0x([0-9a-fA-F]{6,7}),(?:\{|Site::)", code):
            found.add(int(m.group(1), 16))
        for m in re.finditer(r"\brvas\{([^}]*)\}", code):
            found.update(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{6,7})", m.group(1)))
    return {v for v in found if 0x100000 <= v < 0x5570000}


class TableTest(unittest.TestCase):
    def test_builds_and_invariants(self):
        builds = {(b.image, b.timestamp): b for b in game_build.builds()}
        self.assertIn(("exe", NEW_EXE), builds)
        self.assertIn(("dll", NEW_DLL), builds)
        for b in builds.values():
            self.assertEqual(b.reference_timestamp, game_build.reference_timestamp(b.image))
            for ref, e in b.entries.items():
                self.assertTrue(e.rva > 0, hex(ref))
                self.assertLessEqual(e.verified, 544, hex(ref))
                self.assertTrue(all(0 <= o < e.verified and 0 < x < 256 for o, x in e.delta.items()), hex(ref))
        table = (ROOT / "mod/src/GameBuildTable.inc").read_text()
        self.assertIsNone(re.search(r'"[^"]*\\x[0-9a-f]{2}', table), "the table carries no code bytes")
        self.assertNotIn("Signature", table)

    def test_every_translated_reference_is_in_the_table(self):
        exe = game_build.ported(NEW_EXE).entries
        dll = game_build.ported(NEW_DLL, "dll").entries
        missing = sorted(v for v in translated_references() if v not in exe and not (v in DLL_REFERENCES and v in dll))
        self.assertEqual([hex(v) for v in missing], [])

    def test_identity_and_fail_closed(self):
        self.assertEqual(game_build.rva(game_build.REFERENCE_EXE, 0x1D76B50), 0x1D76B50)
        self.assertEqual(game_build.rva(NEW_EXE, 0x1D76B50), 0x1D76E90)
        self.assertEqual(game_build.rva(NEW_EXE, 0x1D76B51), 0)
        self.assertEqual(game_build.rva(NEW_EXE + 1, 0x1D76B50), 0)
        self.assertEqual(game_build.rva(NEW_DLL, 0xD6C170, "dll"), 0xD6C3D0)
        self.assertEqual(game_build.rva(NEW_EXE, 0xD6C170, "dll"), 0)
        # Synthetic reference bytes: their port is the bytes with the entry's delta applied.
        e = game_build.ported(NEW_EXE).entries[0x1D76B50]
        self.assertTrue(e.delta, "the camera add moved a displacement")
        reference = bytes((i * 29 + 7) & 0xFF for i in range(e.verified))
        new = bytes(c ^ e.delta.get(i, 0) for i, c in enumerate(reference))
        self.assertTrue(game_build.code(NEW_EXE, 0x1D76B50, new, reference))
        self.assertFalse(game_build.code(NEW_EXE, 0x1D76B50, reference, reference), "the reference bytes are not the port")
        digest = (e.verified, hashlib.sha256(reference).hexdigest())
        self.assertTrue(game_build.code(NEW_EXE, 0x1D76B50, new, digest))
        self.assertTrue(game_build.code(game_build.REFERENCE_EXE, 0x1D76B50, reference, digest))
        self.assertFalse(game_build.code(NEW_EXE, 0x1D76B50, new + b"\xcc", reference + b"\xcc"), "past the verified prefix")
        self.assertFalse(game_build.code(NEW_EXE, 0x1D76B50, b"\x00" + new[1:], reference))

    def test_scanner_resolves_a_planted_signature(self):
        pattern = bytes((i * 13 + 5) & 0xFF for i in range(24))
        mask = bytes(0 if 8 <= i < 12 else 1 for i in range(24))
        filler = bytes((i * 37 + 11) & 0xFF for i in range(0x2000))
        text = filler[:0x800] + pattern + filler[0x800:]
        self.assertEqual(scan_game_build.find_unique(text, pattern, mask), 0x800)
        relocated = bytearray(text)
        relocated[0x808:0x80C] = b"\x5a\x5a\x5a\x5a"
        self.assertEqual(scan_game_build.find_unique(bytes(relocated), pattern, mask), 0x800, "masked fields may differ")
        broken = bytearray(text)
        broken[0x800] ^= 1
        self.assertIsNone(scan_game_build.find_unique(bytes(broken), pattern, mask), "a changed opcode is not resolved")
        self.assertIsNone(scan_game_build.find_unique(text + pattern, pattern, mask), "an ambiguous signature is not resolved")
        sig = [{"reference": 0x1D76B50, "pattern": pattern.hex(), "mask": mask.hex()}]
        self.assertEqual(scan_game_build.scan(text, 0x1000, sig), {0x1D76B50: 0x1800})

    def test_fault_pod_analyzer_knows_the_ported_builds(self):
        analyzer = (ROOT / "scripts" / "analyze_native_fault_pod.py").read_text()
        self.assertIn("0x6ad110a6", analyzer.lower())
        self.assertIn("0x6ad10fcb", analyzer.lower())


NAMED_ALLOWED = {
    # Reference defaults, replaced by the translated value when the hook installs.
    ("EngineCameraProbe.h", "MAIN_VIEW_RETURN_RVA"), ("EngineCameraProbe.h", "ADDITIONAL_VIEW_RETURN_RVA"),
    ("EngineProjectionHook.h", "ADD_RETURN_RVA"),
    # Translated through the module's own r() wrapper around game_build::rva.
    ("EngineControllerRegistration.cpp", "*"),
    # Diagnostic async-lighting bytes, placed by NativeAsyncLightingControl's rva() wrapper.
    ("Sm2Probe.cpp", "TILED_SWITCH"), ("Sm2Probe.cpp", "ASYNC_SWITCH"),
    # Fault pod: falls back to the reference RVA only before arm() resolved the running build.
    ("NativeFaultEvidence.cpp", "RVA"), ("NativeFaultEvidence.cpp", "EXE_RVA"),
    # A reference value written into a receipt, not an address.
    ("NativeOrdinaryTextureCreation.cpp", "RELEASE_RVA"),
    # A flag mask whose value happens to fall inside the image, so the inventory lists it.
    ("NativePlayerPartVisibility.h", "OUTFIT_HIDDEN"),
}


def named_reference_constants():
    inv = json.loads((ROOT / "mod/src/GameBuildTable.inventory.json").read_text())
    names = {s["symbol"] for b in inv["builds"] for e in b["entries"] for s in e["sites"]
             if re.fullmatch(r"[A-Z][A-Z0-9_]*", s["symbol"] or "")}
    return (names | {"CALLS", "RVAS", "READER_RVAS", "SCENE_SITES"}) - {"BUILD_TIMESTAMP", "TIMESTAMP", "EXE_TIMESTAMP"}


def inside_game_build_call(code, pos):
    for m in re.finditer(r"game_build::(?:rva|code)\(", code[:pos]):
        depth = 1
        for ch in code[m.end():pos]:
            depth += ch == "("
            depth -= ch == ")"
            if depth == 0:
                break
        if depth > 0:
            return True
    return False


class NamedConstantTest(unittest.TestCase):
    def test_named_reference_rvas_only_reach_the_game_through_game_build(self):
        names = named_reference_constants()
        pat = re.compile(r"\b(" + "|".join(sorted(names, key=len, reverse=True)) + r")\b")
        bad = []
        for name, text in source().items():
            for n, line in enumerate(text.splitlines(), 1):
                code = line.split("//")[0]
                for m in pat.finditer(code):
                    sym = m.group(1)
                    if (name, sym) in NAMED_ALLOWED or (name, "*") in NAMED_ALLOWED:
                        continue
                    if (re.search(r"\b(constexpr|static)\b", code[:m.start()]) and re.match(r"\s*[={]", code[m.end():])) \
                            or re.match(r"\s*=\s*0x", code[m.end():]):
                        continue  # the definition itself
                    if "SizeOfImage" in code[max(0, m.start() - 40):m.start()]:
                        continue  # bounds check against the reference size (the image only grew)
                    if inside_game_build_call(code, m.start()):
                        continue
                    if re.match(r"\s*\[", code[m.end():]) is None and re.search(r"for\s*\(\s*const auto&\s*\[", code[:m.start()]):
                        loop = code[m.end():]
                        if "game_build::rva(" in loop or "game_build::code(" in loop:
                            continue  # range-for over reference RVAs, each translated in the body
                    bad.append(f"{name}:{n}: {sym}")
        self.assertEqual(bad, [])


class BuildMapTest(unittest.TestCase):
    def test_translation_rules(self):
        diff = {
            "old_functions": [[0x1000, 0x1100], [0x2000, 0x2100], [0x3000, 0x3080]],
            "function_map": {str(0x1000): 0x1040, str(0x3000): 0x3010},
            "changed": [0x2000],
            "ref_map": {str(0x9000): {"new": 0x9010, "votes": 3, "conflicts": 0},
                        str(0x9100): {"new": 0x9200, "votes": 2, "conflicts": 1}},
            "sections_old": [[".text", 0x1000, 0x4000]],
        }
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "diff.json"
            path.write_text(json.dumps(diff))
            m = BuildMap(path)
        self.assertEqual(m.translate(0x1010), {"status": "moved", "new": 0x1050, "function": 0x1000, "new_function": 0x1040})
        self.assertEqual(m.translate(0x2010)["status"], "changed")
        self.assertNotIn("new", m.translate(0x2010))
        self.assertEqual(m.translate(0x9000), {"status": "ref", "new": 0x9010, "votes": 3})
        self.assertEqual(m.translate(0x9008)["status"], "ref-near")
        self.assertEqual(m.translate(0x9008)["new"], 0x9018)
        self.assertEqual(m.translate(0x9100)["status"], "ref-conflict")
        self.assertEqual(m.translate(0x50000)["status"], "unmapped")


if __name__ == "__main__":
    unittest.main()
