"""Decode scene_fault_pod.bin (NativeSceneFaultEvidence) into JSON; never infers a cause.

    python -I scripts/analyze_scene_fault_pod.py <run folder>/dump/scene_fault_pod.bin

The pod is written once, on the first access violation at the game's reference-count helper
(the 23 Oct level-load crash, docs/PLAYTEST_2026-10-23_1948.md). Standard library only.
"""
import argparse
import json
from pathlib import Path
import struct

MAGIC, VERSION = 0x534D3253, 1
HEADER = struct.Struct("<10Ii3I11Q16Q")
REGION = struct.Struct("<4Q4I512s")
LABELS = ("stack", "object", "table", "entry", "entry_target", "scene_renderer")
REGISTERS = ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15")
CHAIN = {1: "holder_return", 2: "caller_return", 4: "index_read"}


def ascii_run(raw):
    """The longest printable ASCII prefix of a pointer-sized value (the 23 Oct 'mp_bunke')."""
    text = raw.split(b"\0")[0]
    return text.decode("ascii") if text and all(32 <= c < 127 for c in text) else None


def parse(data):
    if len(data) != HEADER.size + len(LABELS) * REGION.size:
        raise ValueError("scene fault pod length differs")
    words = HEADER.unpack_from(data)
    names = ("magic", "version", "size", "pid", "thread", "timestamp", "code", "first_chance", "disposition", "chain",
             "index", "scene_flags", "scene_slot", "scene_active",
             "filetime", "module", "rip", "parameter0", "parameter1", "scene_renderer", "scene_dispatch", "object", "table", "entry", "entry_value")
    value = dict(zip(names, words))
    value["registers"] = dict(zip(REGISTERS, words[len(names):]))
    if (value["magic"], value["version"], value["size"], value["code"], value["first_chance"], value["disposition"]) != (MAGIC, VERSION, len(data), 0xC0000005, 1, 0):
        raise ValueError("scene fault pod header differs")
    value["chain_names"] = [n for bit, n in CHAIN.items() if value["chain"] & bit]
    value["rip_rva"] = value["rip"] - value["module"]
    value["rdx_ascii"] = ascii_run(value["registers"]["rdx"].to_bytes(8, "little"))
    regions = []
    for i, label in enumerate(LABELS):
        address, allocation, base, region_size, state, protect, kind, size, raw = REGION.unpack_from(data, HEADER.size + i * REGION.size)
        if size > 512 or any(raw[size:]):
            raise ValueError(f"{label}: bytes past the copied extent")
        regions.append(dict(label=label, address=address, allocation=allocation, base=base, region_size=region_size,
                            state=state, protect=protect, type=kind, size=size, bytes=raw[:size].hex()))
    value["regions"] = regions
    target = regions[4]
    value["entry_target_ascii"] = ascii_run(bytes.fromhex(target["bytes"])[:64]) if target["size"] else None
    value["cause_verified"] = False
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pod", type=Path)
    print(json.dumps(parse(parser.parse_args().pod.read_bytes()), indent=1))


if __name__ == "__main__":
    main()
