"""Generate the mod's per-build address table (mod/src/GameBuildTable.inc) for a patched game build.

Inputs are RVA-mapped images of the reference build (the one the mod's constants were written
against, PE timestamp 0x6ac3e716) and of the new build, and the function diffs from build_diff.py.

For every build-specific literal in mod/src the generator
  1. translates it (port_rvas.BuildMap; leaf functions and changed functions via build_diff.locate),
  2. for code, computes the verified prefix: the longest run of whole instructions from the address
     whose normalised bytes are equal in both builds and whose rip-relative and call targets
     correspond (each old target translates to the new target, or the target code is equal),
  3. for data, accepts only a reference map entry without conflicts (or an offset inside such an
     object, marked interpolated),
  4. derives a signature: the shortest masked byte pattern from the reference address that is
     unique in the reference .text and finds exactly the translated address in the new .text.

The C++ side (GameBuild.h) only lets a module compare code against at most `verified` bytes, and
only translates addresses present here, so anything unverified fails closed.

    build\\venv-re\\Scripts\\python.exe -I scripts\\gen_game_build_table.py \\
        --old-exe build/gamebin/old_exe.image --new-exe build/gamebin/new_exe.image --exe-diff build/diff/exe.json \\
        --old-dll build/gamebin/old_dll.image --new-dll build/gamebin/new_dll.image --dll-diff build/diff/dll.json \\
        --name 2026-10-21 --inventory mod/src/GameBuildTable.inventory.json --out mod/src/GameBuildTable.inc
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_diff  # noqa: E402
from port_rvas import BuildMap  # noqa: E402

WINDOW = 96
# Modules that compare more than WINDOW bytes of code: reference RVA -> bytes to verify.
WINDOWS = {0x2079230: 544}  # NativePoolReleaseBytes.h: the whole 535-byte pool release function
SIGNATURE_MAX = 64

# Literals that are Retail.dll RVAs (everything else in mod/src is a Retail.exe RVA).
DLL_SITES = {("NativeTrackerHandleGuard.h", "DLL{"), ("NativeFaultEvidence.h", "RVA=0xd6c19c")}
DLL_LITERALS = {0xD6C170, 0xD6C19C, 0x136A0E0, 0x3FC7300}


def literals(src: Path, image_size: int):
    """{value: [(file, line, symbol, image)]} for hex literals inside the reference image."""
    out: dict[tuple[int, str], list] = {}
    for p in sorted(src.glob("*")):
        if p.suffix not in (".h", ".cpp", ".inc") or p.name == "GameBuildTable.inc":
            continue
        for n, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
            code = line.split("//")[0]
            dll_line = any(p.name == f and key in code for f, key in DLL_SITES)
            for m in re.finditer(r"0x([0-9a-fA-F]{5,8})\b", code):
                v = int(m.group(1), 16)
                if not 0x100000 <= v < image_size:
                    continue
                image = "dll" if dll_line and v in DLL_LITERALS else "exe"
                before = code[:m.start()]
                sym = re.search(r"(\w+)\s*(?:=|\{|\(|,)\s*$", before)
                out.setdefault((v, image), []).append(
                    {"file": p.name, "line": n, "symbol": sym.group(1) if sym else ""})
    return out


class Porter:
    def __init__(self, old_path, new_path, diff_path):
        self.old = Path(old_path).read_bytes()
        self.new = Path(new_path).read_bytes()
        self.diff = json.loads(Path(diff_path).read_text())
        self.map = BuildMap(diff_path)
        self.text_old = next((va, va + vs) for n, va, vs in build_diff.sections(self.old) if n == ".text")
        self.text_new = next((va, va + vs) for n, va, vs in build_diff.sections(self.new) if n == ".text")

    def is_code(self, rva):
        return self.text_old[0] <= rva < self.text_old[1]

    def translate(self, rva):
        t = self.map.translate(rva)
        if t["status"] in ("moved", "ref", "ref-near"):
            return t
        if self.is_code(rva):
            loc = build_diff.locate(self.old, self.new, self.diff, rva)
            if loc["status"] in ("leaf-moved", "aligned"):
                return loc
            return loc
        return t

    def target_ok(self, old_t, new_t):
        t = self.map.translate(old_t)
        if "new" in t and t["status"] in ("moved", "ref"):
            return t["new"] == new_t
        a, _ = build_diff.normalise(self.old, old_t, old_t + 24)
        b, _ = build_diff.normalise(self.new, new_t, new_t + 24)
        return a == b and self.old[old_t:old_t + 24] != b"\0" * 24

    def verified_prefix(self, old_rva, new_rva):
        """Bytes of whole instructions from the address that are the same code in both builds."""
        import capstone
        from capstone import x86

        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = True
        window = WINDOWS.get(old_rva, WINDOW)
        a = list(md.disasm(self.old[old_rva:old_rva + window + 16], old_rva))
        b = list(md.disasm(self.new[new_rva:new_rva + window + 16], new_rva))
        length = 0
        for ia, ib in zip(a, b):
            if ia.size != ib.size or ia.mnemonic != ib.mnemonic or length + ia.size > window:
                break
            na, nb = bytearray(ia.bytes), bytearray(ib.bytes)
            targets = []
            for insn, buf in ((ia, na), (ib, nb)):
                t = None
                if insn.disp_size and any(op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP
                                          for op in insn.operands):
                    buf[insn.disp_offset:insn.disp_offset + insn.disp_size] = b"\xcc" * insn.disp_size
                    t = insn.address + insn.size + int.from_bytes(insn.bytes[insn.disp_offset:insn.disp_offset + 4], "little", signed=True)
                elif insn.disp_size == 4 and any(op.type == x86.X86_OP_MEM and op.mem.index != 0 and
                                                 build_diff.IMAGE_DISP[0] <= op.mem.disp < build_diff.IMAGE_DISP[1]
                                                 for op in insn.operands):
                    buf[insn.disp_offset:insn.disp_offset + 4] = b"\xcc" * 4
                    t = int.from_bytes(insn.bytes[insn.disp_offset:insn.disp_offset + 4], "little", signed=True)
                if insn.imm_size == 4 and insn.group(capstone.CS_GRP_BRANCH_RELATIVE):
                    buf[insn.imm_offset:insn.imm_offset + 4] = b"\xcc" * 4
                    t = insn.operands[0].imm
                targets.append(t)
            if na != nb:
                break
            if targets[0] is not None and not self.target_ok(targets[0], targets[1]):
                break
            length += ia.size
        return length

    def masked(self, rva, length, image):
        """Reference bytes from rva with rip/rel32 fields wildcarded: (bytes, mask)."""
        norm, _ = build_diff.normalise(image, rva, rva + length)
        raw = image[rva:rva + length]
        mask = bytes(0 if n == 0xCC and r != 0xCC else 1 for n, r in zip(norm, raw))
        return raw, mask

    def signature(self, old_rva, new_rva, verified):
        if verified < 8:
            return None
        lo, hi = self.text_old
        nlo, nhi = self.text_new
        for length in range(8, min(verified, SIGNATURE_MAX) + 1, 4):
            raw, mask = self.masked(old_rva, length, self.old)
            rx = re.compile(b"".join(re.escape(bytes([c])) if m else b"." for c, m in zip(raw, mask)), re.S)
            hits_old = [m.start() for m in rx.finditer(self.old, lo, hi)][:2]
            if hits_old != [old_rva]:
                continue
            hits_new = [m.start() for m in rx.finditer(self.new, nlo, nhi)][:2]
            if hits_new == [new_rva]:
                return {"length": length, "pattern": raw.hex(), "mask": mask.hex()}
            return None
        return None


def delta(old: bytes, new: bytes):
    """[offset, old ^ new] where the verified prefixes differ (only moved rip/rel32 fields)."""
    return [[i, a ^ b] for i, (a, b) in enumerate(zip(old, new)) if a != b]


def build_entries(porter: Porter, values, image):
    entries = []
    for (v, img), sites in sorted(values.items()):
        if img != image:
            continue
        t = porter.translate(v)
        e = {"reference": v, "image": image, "sites": sites, "status": t["status"],
             "kind": "code" if porter.is_code(v) else "data"}
        if "new" in t:
            e["rva"] = t["new"]
            if e["kind"] == "code":
                e["verified"] = porter.verified_prefix(v, t["new"])
                e["xor"] = delta(porter.old[v:v + e["verified"]], porter.new[t["new"]:t["new"] + e["verified"]])
                e["signature"] = porter.signature(v, t["new"], e["verified"])
            else:
                e["verified"] = 0
                e["votes"] = t.get("votes", 0)
                if t["status"] == "ref-near":
                    e["anchor"] = t["anchor"]
        for k in ("ratio", "equal_run", "delta"):
            if k in t:
                e[k] = t[k]
        entries.append(e)
    return entries


def accepted(e):
    if "rva" not in e:
        return False
    if e["kind"] == "code":
        return e["verified"] > 0
    return e["status"] in ("ref", "ref-near")


def emit_cpp(name, builds, out: Path):
    lines = [
        "// Generated by scripts/gen_game_build_table.py; do not edit by hand.",
        "// Per-build verified address table: reference-build RVA -> this build's RVA, the length of the",
        "// code proven equal to the reference code there, and where the two builds' bytes differ in it",
        "// (offset, reference XOR this build: moved rip/rel32 fields only). No code bytes.",
        "#pragma once",
        "",
    ]
    for b in builds:
        ident = f"{b['image'].upper()}_{b['timestamp']:08X}"
        for e in b["entries"]:
            if e.get("xor"):
                pairs = ",".join(f"{{{o},{m:#04x}}}" for o, m in e["xor"])
                lines.append(f"inline constexpr Delta DELTA_{ident}_{e['reference']:X}[]{{{pairs}}};")
        lines.append(f"inline constexpr Entry ENTRIES_{ident}[]{{")
        for e in b["entries"]:
            d = f"DELTA_{ident}_{e['reference']:X}" if e.get("xor") else "{}"
            lines.append(f"    {{{e['reference']:#x},{e['rva']:#x},{e['verified']},{d}}},")
        lines.append("};")
        lines.append("")
    lines.append("inline constexpr Build PORTED_BUILDS[]{")
    for b in builds:
        ident = f"{b['image'].upper()}_{b['timestamp']:08X}"
        lines.append(f"    {{Image::{b['image']},{b['reference_timestamp']:#x},{b['timestamp']:#x},{b['size_of_image']:#x},"
                     f"\"{name}\",ENTRIES_{ident}}},")
    lines.append("};")
    out.write_text("\n".join(lines) + "\n", newline="\n")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for k in ("old-exe", "new-exe", "exe-diff", "old-dll", "new-dll", "dll-diff", "inventory", "out", "name"):
        p.add_argument("--" + k, required=True)
    p.add_argument("--src", default="mod/src")
    # Masked reference patterns for porting the next build. They are the game's bytes, so they stay
    # in build/ (gitignored) and out of the DLL: scripts/scan_game_build.py reads them.
    p.add_argument("--signatures", default="build/gen/signatures.json")
    a = p.parse_args(argv)
    builds, inventory, signatures = [], {"name": a.name, "builds": []}, {"name": a.name, "builds": []}
    for image, old, new, diff in (("exe", a.old_exe, a.new_exe, a.exe_diff), ("dll", a.old_dll, a.new_dll, a.dll_diff)):
        porter = Porter(old, new, diff)
        h_old, h_new = build_diff.header(porter.old), build_diff.header(porter.new)
        values = literals(Path(a.src), h_old["size_of_image"])
        entries = build_entries(porter, values, image)
        signatures["builds"].append({"image": image, "timestamp": h_new["timestamp"], "signatures": [
            {"reference": e["reference"], **e["signature"]} for e in entries if accepted(e) and e.get("signature")]})
        for e in entries:
            e.pop("signature", None)
        inventory["builds"].append({"image": image, "reference_timestamp": h_old["timestamp"],
                                    "timestamp": h_new["timestamp"], "size_of_image": h_new["size_of_image"],
                                    "entries": entries})
        builds.append({"image": image, "reference_timestamp": h_old["timestamp"], "timestamp": h_new["timestamp"],
                       "size_of_image": h_new["size_of_image"], "entries": [e for e in entries if accepted(e)]})
        ok = sum(accepted(e) for e in entries)
        print(f"{image}: {len(entries)} literals, {ok} accepted")
        for e in entries:
            if not accepted(e):
                print(f"  rejected {e['reference']:#x} {e['status']} {[s['file'] for s in e['sites']]}")
    for path, value in ((a.inventory, inventory), (a.signatures, signatures)):
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_text(json.dumps(value, indent=1) + "\n", newline="\n")
    emit_cpp(a.name, builds, Path(a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
