"""Translate old-build RVAs to a new build using the maps written by build_diff.py.

    python -I scripts/port_rvas.py build/diff/exe.json 0x1d76b50 0x4e8ec50 ...
    python -I scripts/port_rvas.py build/diff/exe.json --scan mod/src     (every literal in the sources)

Status of each translation:
  moved      inside a function whose normalised bytes are unchanged; new = function start + same offset
  ref        a global/vtable/callee: matched code references it, `votes` instructions agree
  ref-near   inside a data object: the nearest referenced address below shifts by the same delta
  changed    inside a function whose code changed: needs manual porting
  unmapped   no evidence; never use
Only `moved` and `ref` (no conflicts) are verified by construction. Standard library only.
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import sys
from pathlib import Path


class BuildMap:
    def __init__(self, path: str | Path):
        d = json.loads(Path(path).read_text())
        self.d = d
        self.old_funcs = d["old_functions"]
        self.old_begins = [b for b, _ in self.old_funcs]
        self.fmap = {int(k): v for k, v in d["function_map"].items()}
        self.refs = {int(k): v for k, v in d["ref_map"].items()}
        self.ref_keys = sorted(self.refs)
        self.changed = set(d["changed"])
        self.text_end = next((va + vs for n, va, vs in d["sections_old"] if n == ".text"), 0)

    def function_of(self, rva: int):
        i = bisect.bisect_right(self.old_begins, rva) - 1
        if i >= 0 and self.old_funcs[i][0] <= rva < self.old_funcs[i][1]:
            return self.old_funcs[i]
        return None

    def translate(self, rva: int) -> dict:
        r = self.refs.get(rva)
        if r is not None and r["conflicts"] == 0:
            return {"status": "ref", "new": r["new"], "votes": r["votes"]}
        f = self.function_of(rva)
        if f is not None:
            begin = f[0]
            if begin in self.fmap:
                return {"status": "moved", "new": self.fmap[begin] + rva - begin,
                        "function": begin, "new_function": self.fmap[begin]}
            return {"status": "changed", "function": begin, "end": f[1]}
        if r is not None:
            return {"status": "ref-conflict", "new": r["new"], "votes": r["votes"], "conflicts": r["conflicts"]}
        if rva >= self.text_end:
            i = bisect.bisect_right(self.ref_keys, rva) - 1
            if i >= 0:
                k = self.ref_keys[i]
                v = self.refs[k]
                if rva - k < 0x1000 and v["conflicts"] == 0:
                    return {"status": "ref-near", "new": v["new"] + rva - k, "anchor": k, "votes": v["votes"]}
        return {"status": "unmapped"}


def scan(src: Path, lo: int = 0x100000):
    found: dict[int, list[str]] = {}
    for p in sorted(src.rglob("*")):
        if p.suffix not in (".h", ".cpp", ".inc", ".py", ".ps1"):
            continue
        for n, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
            code = line.split("//")[0] if p.suffix in (".h", ".cpp", ".inc") else line
            for m in re.finditer(r"0x([0-9a-fA-F]{5,8})\b", code):
                v = int(m.group(1), 16)
                if v >= lo:
                    found.setdefault(v, []).append(f"{p.as_posix()}:{n}")
    return found


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("map")
    p.add_argument("rvas", nargs="*")
    p.add_argument("--scan")
    a = p.parse_args(argv)
    bm = BuildMap(a.map)
    items = [(int(x, 16), []) for x in a.rvas]
    if a.scan:
        items += sorted(scan(Path(a.scan)).items())
    for v, where in items:
        t = bm.translate(v)
        new = f"{t['new']:#x}" if "new" in t else "-"
        print(f"{v:#010x} {t['status']:<12} {new:<10} {','.join(sorted(set(w.split(':')[0].split('/')[-1] for w in where)))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
