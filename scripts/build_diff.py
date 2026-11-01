"""Function-level diff of two builds of a PE image, for porting hard-coded RVAs after a game patch.

Each .pdata function of the old and new image is disassembled and normalised: the bytes of
rip-relative displacements and rel32 branch/call targets are masked, everything else (opcodes,
registers, struct offsets, immediates) is kept. Functions whose normalised bytes are equal are the
same code that merely moved. A function whose normalised bytes changed is reported as changed and
never mapped.

From every matched pair the rip-relative and call targets line up instruction by instruction, which
gives a second map for globals, vtables and callees, with a vote count per target.

Needs capstone (analysis only; the mod and the test suite do not). Run it from an analysis venv:

    build\\venv-re\\Scripts\\python.exe -I scripts\\build_diff.py diff OLD.image NEW.image OUT.json

Images are memory-mapped (RVA-addressed) files: `DumpImage.java` writes one from a Ghidra project,
and `map` writes one from a PE file on disk:

    build\\venv-re\\Scripts\\python.exe -I scripts\\build_diff.py map Game.exe OUT.image
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import struct
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path


def sections(image: bytes):
    e = struct.unpack_from("<I", image, 0x3C)[0]
    count = struct.unpack_from("<H", image, e + 6)[0]
    opt = e + 24
    table = opt + struct.unpack_from("<H", image, e + 20)[0]
    out = []
    for i in range(count):
        o = table + 40 * i
        name = image[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va = struct.unpack_from("<II", image, o + 8)
        out.append((name, va, vsize))
    return out


def header(image: bytes):
    e = struct.unpack_from("<I", image, 0x3C)[0]
    opt = e + 24
    return {
        "timestamp": struct.unpack_from("<I", image, e + 8)[0],
        "size_of_image": struct.unpack_from("<I", image, opt + 56)[0],
        "pdata": struct.unpack_from("<II", image, opt + 112 + 3 * 8),
    }


def functions(image: bytes):
    rva, size = header(image)["pdata"]
    out = []
    for i in range(size // 12):
        begin, end, _ = struct.unpack_from("<III", image, rva + 12 * i)
        if begin and end > begin:
            out.append((begin, end))
    out.sort()
    return out


_IMAGE: bytes | None = None
# Non-rip displacements in this range are image-relative RVAs (no struct is 1 MB+ large).
IMAGE_DISP = (0x100000, 0x8000000)


def _init(path: str):
    global _IMAGE
    _IMAGE = Path(path).read_bytes()


def normalise(image: bytes, begin: int, end: int):
    """Return (normalised bytes, [(offset, kind, target)]) for one function range."""
    import capstone
    from capstone import x86

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    code = image[begin:end]
    norm = bytearray(code)
    refs = []
    pos = 0
    while pos < len(code):
        decoded = False
        for insn in md.disasm(code[pos:], begin + pos):
            off = insn.address - begin
            decoded = True
            if insn.disp_size and any(
                op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP for op in insn.operands
            ):
                d = off + insn.disp_offset
                norm[d:d + insn.disp_size] = b"\xcc" * insn.disp_size
                disp = int.from_bytes(code[d:d + 4], "little", signed=True)
                refs.append((off, "rip", insn.address + insn.size + disp))
            elif insn.disp_size == 4 and any(
                op.type == x86.X86_OP_MEM and op.mem.index != 0 and IMAGE_DISP[0] <= op.mem.disp < IMAGE_DISP[1]
                for op in insn.operands
            ):
                # [base + index*scale + RVA] with base = image base: a global or jump table addressed
                # image-relative (MSVC switch tables, arrays). The displacement is an RVA, not an offset.
                d = off + insn.disp_offset
                disp = int.from_bytes(code[d:d + 4], "little", signed=True)
                norm[d:d + 4] = b"\xcc" * 4
                refs.append((off, "img", disp))
            if insn.imm_size == 4 and insn.group(capstone.CS_GRP_BRANCH_RELATIVE):
                i = off + insn.imm_offset
                norm[i:i + 4] = b"\xcc" * 4
                refs.append((off, "call" if insn.group(capstone.CS_GRP_CALL) else "jmp",
                             insn.operands[0].imm))
            pos = off + insn.size
        if not decoded or pos < len(code):
            pos += 1  # undecodable byte (data in code): keep it literal and resync
    return bytes(norm), refs


def _work(chunk):
    out = []
    for begin, end in chunk:
        norm, refs = normalise(_IMAGE, begin, end)
        out.append((begin, end, hashlib.sha1(norm).hexdigest(), refs))
    return out


def analyse(path: str, workers: int):
    image = Path(path).read_bytes()
    funcs = functions(image)
    step = 2000
    chunks = [funcs[i:i + step] for i in range(0, len(funcs), step)]
    result = []
    with ProcessPoolExecutor(workers, initializer=_init, initargs=(path,)) as pool:
        for part in pool.map(_work, chunks):
            result.extend(part)
    return image, result


def diff(old_path: str, new_path: str, workers: int):
    old_image, old = analyse(old_path, workers)
    new_image, new = analyse(new_path, workers)
    by_hash: dict[str, list[int]] = {}
    new_info = {b: (e, h, r) for b, e, h, r in new}
    for b, e, h, r in new:
        by_hash.setdefault(h, []).append(b)
    old_hashes: dict[str, int] = {}
    for b, e, h, r in old:
        old_hashes[h] = old_hashes.get(h, 0) + 1

    fmap: dict[int, int] = {}
    # Pass 1: unique on both sides.
    for b, e, h, r in old:
        cands = by_hash.get(h, [])
        if len(cands) == 1 and old_hashes[h] == 1:
            fmap[b] = cands[0]
    # Pass 2: duplicates, resolved by the delta of the nearest uniquely matched neighbour.
    keys = sorted(fmap)
    used = set(fmap.values())
    for b, e, h, r in old:
        if b in fmap:
            continue
        cands = [c for c in by_hash.get(h, []) if c not in used]
        if not cands or not keys:
            continue
        i = bisect.bisect_left(keys, b)
        deltas = {fmap[keys[j]] - keys[j] for j in (i - 1, i) if 0 <= j < len(keys)}
        hit = [c for c in cands if c - b in deltas]
        if len(hit) == 1:
            fmap[b] = hit[0]
            used.add(hit[0])

    # Reference map from matched pairs.
    votes: dict[int, dict[int, int]] = {}
    old_info = {b: (e, h, r) for b, e, h, r in old}
    for b, nb in fmap.items():
        _, _, orefs = old_info[b]
        _, _, nrefs = new_info[nb]
        if len(orefs) != len(nrefs):
            continue
        for (oo, ok, ot), (no, nk, nt) in zip(orefs, nrefs):
            if oo == no and ok == nk:
                votes.setdefault(ot, {}).setdefault(nt, 0)
                votes[ot][nt] += 1
    refmap = {}
    for ot, c in votes.items():
        best = max(c.items(), key=lambda kv: kv[1])
        refmap[ot] = {"new": best[0], "votes": best[1], "conflicts": len(c) - 1}

    changed = [b for b, e, h, r in old if b not in fmap]
    return {
        "old": header(old_image) | {"path": old_path},
        "new": header(new_image) | {"path": new_path},
        "old_functions": [[b, e] for b, e, h, r in old],
        "new_functions": [[b, e] for b, e, h, r in new],
        "function_map": {str(k): v for k, v in sorted(fmap.items())},
        "changed": changed,
        "ref_map": {str(k): v for k, v in sorted(refmap.items())},
        "sections_old": sections(old_image),
        "sections_new": sections(new_image),
    }


def _insns(image: bytes, begin: int, end: int):
    """[(offset, size, normalised text)] for [begin, end); rip/rel32 targets replaced by '?'."""
    import capstone
    from capstone import x86

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    out = []
    for insn in md.disasm(image[begin:end], begin):
        text = f"{insn.mnemonic} {insn.op_str}"
        rip = any(op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP for op in insn.operands)
        if rip:
            text = f"{insn.mnemonic} " + __import__("re").sub(r"\[rip [+-] 0x[0-9a-f]+\]", "[rip+?]", insn.op_str)
        elif insn.disp_size == 4 and any(op.type == x86.X86_OP_MEM and op.mem.index != 0 and
                                         IMAGE_DISP[0] <= op.mem.disp < IMAGE_DISP[1] for op in insn.operands):
            text = f"{insn.mnemonic} " + __import__("re").sub(r" \+ 0x[0-9a-f]{6,8}\]", " + ?]", insn.op_str)
        if insn.imm_size == 4 and insn.group(capstone.CS_GRP_BRANCH_RELATIVE):
            text = f"{insn.mnemonic} ?"
        elif insn.group(capstone.CS_GRP_BRANCH_RELATIVE):
            text = f"{insn.mnemonic} short"
        out.append((insn.address - begin, insn.size, text))
    return out


def _neighbour_deltas(diff_result, rva: int, span: int = 0x20000):
    fmap = {int(k): v for k, v in diff_result["function_map"].items()}
    keys = sorted(fmap)
    i = bisect.bisect_left(keys, rva)
    deltas = []
    for j in range(max(0, i - 8), min(len(keys), i + 8)):
        if abs(keys[j] - rva) < span:
            d = fmap[keys[j]] - keys[j]
            if d not in deltas:
                deltas.append(d)
    return deltas


def locate(old_image: bytes, new_image: bytes, diff_result, rva: int, length: int = 24):
    """Find an old address that build_diff could not map: a leaf function or a changed one."""
    import difflib

    funcs = diff_result["old_functions"]
    begins = [b for b, _ in funcs]
    i = bisect.bisect_right(begins, rva) - 1
    inside = i >= 0 and funcs[i][0] <= rva < funcs[i][1]
    deltas = _neighbour_deltas(diff_result, rva)
    if not inside:
        old_norm, _ = normalise(old_image, rva, rva + length)
        hits = [d for d in deltas if normalise(new_image, rva + d, rva + d + length)[0] == old_norm]
        if len(hits) == 1:
            return {"status": "leaf-moved", "new": rva + hits[0], "delta": hits[0], "window": length}
        return {"status": "leaf-unresolved", "deltas": deltas, "hits": hits}
    begin, end = funcs[i]
    nfuncs = diff_result["new_functions"]
    nbegins = [b for b, _ in nfuncs]
    best = None
    for d in deltas:
        j = bisect.bisect_left(nbegins, begin + d)
        for k in (j - 1, j, j + 1):
            if 0 <= k < len(nfuncs):
                nb, ne = nfuncs[k]
                a = _insns(old_image, begin, end)
                b = _insns(new_image, nb, ne)
                sm = difflib.SequenceMatcher(None, [t for _, _, t in a], [t for _, _, t in b], autojunk=False)
                ratio = sm.ratio()
                if best is None or ratio > best[0]:
                    best = (ratio, nb, ne, a, b, sm)
    if best is None:
        return {"status": "changed-unresolved"}
    ratio, nb, ne, a, b, sm = best
    off = rva - begin
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        for ii in range(i1, i2):
            if a[ii][0] <= off < a[ii][0] + a[ii][1]:
                inner = off - a[ii][0]
                if tag == "equal":
                    jj = j1 + (ii - i1)
                    # Context: how many equal instructions surround the site.
                    return {"status": "aligned", "new": nb + b[jj][0] + inner, "new_function": nb,
                            "ratio": round(ratio, 4), "equal_run": i2 - i1, "run_pos": ii - i1,
                            "old_insn": a[ii][2], "new_insn": b[jj][2]}
                return {"status": "site-changed", "tag": tag, "old_insn": a[ii][2],
                        "new_function": nb, "ratio": round(ratio, 4),
                        "new_candidates": [x[2] for x in b[j1:j2]][:8]}
    return {"status": "changed-unresolved"}


def map_pe(src: str, dst: str):
    data = Path(src).read_bytes()
    e = struct.unpack_from("<I", data, 0x3C)[0]
    opt = e + 24
    size = struct.unpack_from("<I", data, opt + 56)[0]
    hdr = struct.unpack_from("<I", data, opt + 60)[0]
    out = bytearray(size)
    out[:hdr] = data[:hdr]
    table = opt + struct.unpack_from("<H", data, e + 20)[0]
    for i in range(struct.unpack_from("<H", data, e + 6)[0]):
        o = table + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
        n = min(vsize or rsize, rsize)
        out[va:va + n] = data[raw:raw + n]
    Path(dst).write_bytes(bytes(out))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("diff")
    d.add_argument("old")
    d.add_argument("new")
    d.add_argument("out")
    d.add_argument("--workers", type=int, default=8)
    m = sub.add_parser("map")
    m.add_argument("pe")
    m.add_argument("out")
    lo = sub.add_parser("locate")
    lo.add_argument("old")
    lo.add_argument("new")
    lo.add_argument("diff")
    lo.add_argument("rvas", nargs="+")
    lo.add_argument("--length", type=int, default=24)
    a = p.parse_args(argv)
    if a.cmd == "map":
        map_pe(a.pe, a.out)
        return 0
    if a.cmd == "locate":
        old_image, new_image = Path(a.old).read_bytes(), Path(a.new).read_bytes()
        d = json.loads(Path(a.diff).read_text())
        for r in a.rvas:
            res = locate(old_image, new_image, d, int(r, 16), a.length)
            if "new" in res:
                res["new"] = hex(res["new"])
            if "new_function" in res:
                res["new_function"] = hex(res["new_function"])
            print(r, json.dumps(res))
        return 0
    result = diff(a.old, a.new, a.workers)
    Path(a.out).write_text(json.dumps(result))
    nf = len(result["old_functions"])
    print(f"functions old={nf} new={len(result['new_functions'])} matched={len(result['function_map'])} "
          f"changed={len(result['changed'])} refs={len(result['ref_map'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
