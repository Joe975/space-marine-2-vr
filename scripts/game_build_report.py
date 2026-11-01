"""Render an inventory written by gen_game_build_table.py as the markdown address table.

    python -I scripts/game_build_report.py mod/src/GameBuildTable.inventory.json

Standard library only.
"""
import json
import sys
from pathlib import Path

EVIDENCE = {
    "moved": "function unchanged (normalised bytes equal), same offset",
    "ref": "referenced by unchanged code; all matched instructions agree",
    "ref-near": "field inside a referenced object (anchor shifted)",
    "leaf-moved": "leaf without unwind data; bytes equal at the neighbours' shift",
    "aligned": "site inside a changed function; inside an identical instruction run",
}


def main(argv=None):
    argv = argv or sys.argv[1:]
    inv = json.loads(Path(argv[0]).read_text())
    out = ["| Image | Reference RVA | New RVA | Kind | Evidence | Verified bytes | Signature | Used in |",
           "|---|---|---|---|---|---|---|---|"]
    rejected = []
    for b in inv["builds"]:
        for e in b["entries"]:
            files = ", ".join(sorted({s["file"] for s in e["sites"]}))
            if "rva" not in e or (e["kind"] == "code" and not e.get("verified")):
                rejected.append(f"- {b['image']} `{e['reference']:#x}` ({e['status']}): {files}")
                continue
            sig = f"{e['signature']['length']} B" if e.get("signature") else "-"
            out.append(f"| {b['image']} | `{e['reference']:#x}` | `{e['rva']:#x}` | {e['kind']} | {e['status']} | "
                       f"{e.get('verified', 0) or '-'} | {sig} | {files} |")
    print("\n".join(out))
    if rejected:
        print("\nNot in the table (not RVAs, or unverifiable):\n")
        print("\n".join(rejected))
    print("\nEvidence: " + "; ".join(f"**{k}** {v}" for k, v in EVIDENCE.items()) + ".")
    return 0


if __name__ == "__main__":
    sys.exit(main())
