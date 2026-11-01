"""Resolve the porting signatures over a new game build's .text: the first step of porting the table.

    python -I scripts/scan_game_build.py "<new Retail.exe>" [--signatures build/gen/signatures.json]

The signatures are masked reference-build patterns (rip/rel32 fields wildcarded), written by
scripts/gen_game_build_table.py. They are the game's bytes, so they stay in build/ (gitignored)
and out of the repo and the DLL; regenerate them from the reference images when missing. Every
hit is a candidate for docs/GAME_PATCH_2026-10-21.md's method, never an address to hook.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path


def find_unique(haystack: bytes, pattern: bytes, mask: bytes) -> int | None:
    """Offset of the single masked match of pattern in haystack, or None (absent or ambiguous)."""
    if not pattern or not any(mask):
        return None
    rx = re.compile(b"".join(re.escape(bytes([c])) if m else b"." for c, m in zip(pattern, mask)), re.S)
    hits = [m.start() for m, _ in zip(rx.finditer(haystack), range(2))]
    return hits[0] if len(hits) == 1 else None


def text_section(pe: bytes) -> tuple[bytes, int]:
    """(.text bytes, its RVA) from a PE file on disk."""
    lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    if pe[lfanew:lfanew + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    count, optional = struct.unpack_from("<HH", pe, lfanew + 6)[0], struct.unpack_from("<H", pe, lfanew + 20)[0]
    table = lfanew + 24 + optional
    for i in range(count):
        name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", pe, table + 40 * i)
        if name.rstrip(b"\0") == b".text":
            return pe[raw:raw + min(vsize, rsize)], va
    raise ValueError("no .text section")


def scan(text: bytes, text_rva: int, signatures: list[dict]) -> dict[int, int | None]:
    out = {}
    for s in signatures:
        pattern, mask = bytes.fromhex(s["pattern"]), bytes.fromhex(s["mask"])
        hit = find_unique(text, pattern, mask)
        out[s["reference"]] = None if hit is None else hit + text_rva
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("exe", type=Path)
    p.add_argument("--signatures", type=Path, default=Path("build/gen/signatures.json"))
    p.add_argument("--image", default="exe", choices=("exe", "dll"))
    a = p.parse_args(argv)
    if not a.signatures.is_file():
        print(f"{a.signatures} missing: run scripts/gen_game_build_table.py against the reference images first", file=sys.stderr)
        return 2
    sigs = next((b["signatures"] for b in json.loads(a.signatures.read_text())["builds"] if b["image"] == a.image), [])
    text, va = text_section(a.exe.read_bytes())
    found = scan(text, va, sigs)
    for ref, hit in sorted(found.items()):
        print(f"{ref:#x} -> {hit:#x}" if hit is not None else f"{ref:#x} -> no unique match")
    print(f"{sum(h is not None for h in found.values())} of {len(found)} signatures resolved (candidates only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
