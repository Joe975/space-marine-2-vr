"""Python view of mod/src/GameBuildTable.inc: the same per-build verified address table the mod uses.

    import game_build
    game_build.rva(0x6ad10fcb, 0x4f7c6c8)            -> 0x4f7c688 (0 if not verified)
    game_build.code(stamp, 0x209fac0, actual, ref)  -> True when `actual` is the verified port of `ref`
                                                       (ref: the reference bytes, or (length, sha256 hex))

Standard library only. The reference build (exe 0x6ac3e716, dll 0x6ac3eb69) is the identity. Each
ported entry holds the new RVA, the verified length and the delta (offset -> reference XOR new
byte) over that prefix; the table carries no code bytes.
"""
from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass, field
from pathlib import Path

REFERENCE_EXE = 0x6AC3E716
REFERENCE_DLL = 0x6AC3EB69
TABLE = Path(__file__).resolve().parents[1] / "mod" / "src" / "GameBuildTable.inc"


@dataclass
class Entry:
    rva: int
    verified: int
    delta: dict[int, int] = field(default_factory=dict)


@dataclass
class Build:
    image: str
    reference_timestamp: int
    timestamp: int
    size_of_image: int
    name: str
    entries: dict[int, Entry] = field(default_factory=dict)


def load(path: Path = TABLE) -> list[Build]:
    text = Path(path).read_text()
    deltas = {m.group(1): {int(o): int(x, 16) for o, x in re.findall(r"\{(\d+),(0x[0-9a-f]+)\}", m.group(2))}
              for m in re.finditer(r"inline constexpr Delta (DELTA_\w+)\[\]\{(.*?)\};", text)}
    entries = {}
    for m in re.finditer(r"inline constexpr Entry ENTRIES_(\w+)\[\]\{\n(.*?)\n\};", text, re.S):
        entries[m.group(1)] = {int(r, 16): Entry(int(n, 16), int(v), dict(deltas.get(d, {})))
                               for r, n, v, d in re.findall(r"\{(0x[0-9a-f]+),(0x[0-9a-f]+),(\d+),(DELTA_\w+|\{\})\}", m.group(2))}
    builds = []
    for image, ref, stamp, size, name, ident in re.findall(
            r'\{Image::(\w+),(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+),"([^"]*)",ENTRIES_(\w+)\}', text):
        builds.append(Build(image, int(ref, 16), int(stamp, 16), int(size, 16), name, entries[ident]))
    return builds


_BUILDS: list[Build] | None = None


def builds() -> list[Build]:
    global _BUILDS
    if _BUILDS is None:
        _BUILDS = load()
    return _BUILDS


def reference_timestamp(image: str = "exe") -> int:
    return REFERENCE_DLL if image == "dll" else REFERENCE_EXE


def ported(timestamp: int, image: str = "exe") -> Build | None:
    return next((b for b in builds() if b.image == image and b.timestamp == timestamp), None)


def known(timestamp: int, image: str = "exe") -> bool:
    return timestamp == reference_timestamp(image) or ported(timestamp, image) is not None


def rva(timestamp: int, reference: int, image: str = "exe") -> int:
    if timestamp == reference_timestamp(image):
        return reference
    b = ported(timestamp, image)
    e = b.entries.get(reference) if b else None
    return e.rva if e else 0


def _matches(actual: bytes, expected) -> bool:
    if isinstance(expected, tuple):
        length, digest = expected
        return len(actual) == length and hashlib.sha256(actual).hexdigest() == digest
    return actual == expected


def code(timestamp: int, reference: int, actual: bytes, expected, image: str = "exe") -> bool:
    length = expected[0] if isinstance(expected, tuple) else len(expected)
    if len(actual) < length:
        return False
    head = bytes(actual[:length])
    if timestamp == reference_timestamp(image):
        return _matches(head, expected)
    b = ported(timestamp, image)
    e = b.entries.get(reference) if b else None
    if not e or e.verified < length:
        return False
    undone = bytes(c ^ e.delta.get(i, 0) for i, c in enumerate(head))
    return _matches(undone, expected)
