"""Derive an FOV-only benchmark pack while preserving menus and all other entries."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import zipfile
from build_first_person import CAMERAS


def build(base, output, fov):
    if isinstance(fov, bool) or not math.isfinite(fov) or not 80 <= fov <= 120:
        raise ValueError("native benchmark FOV must be in [80, 120]")
    base, output = Path(base), Path(output)
    if base.resolve() == output.resolve():
        raise ValueError("benchmark output must not overwrite the reference pack")
    with zipfile.ZipFile(base) as archive:
        if len(archive.infolist()) > 64 or any(i.file_size > 2_000_000 for i in archive.infolist()):
            raise ValueError("expected bounded VR test pack")
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise ValueError("duplicate pack entries")
        entries = {i.filename: archive.read(i) for i in archive.infolist()}
    changed = []
    for name in CAMERAS:
        if name not in entries:
            raise ValueError(f"missing camera: {name}")
        original = entries[name]
        patched, count = re.subn(rb"(?m)^(\s*fov\s*=\s*)[-+\d.]+(?=\s*(?:\r?$))",
                                lambda m: m[1]+str(float(fov)).encode("ascii"), original)
        if count != 1:
            raise ValueError(f"expected one native FOV field: {name}")
        entries[name] = patched
        if original != patched:
            changed.append(name)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, data in sorted(entries.items()):
            archive.writestr(zipfile.ZipInfo(name, (2026, 10, 15, 0, 0, 0)), data)
    result = {"native_fov": fov, "base_sha256": hashlib.sha256(Path(base).read_bytes()).hexdigest(),
              "pack_sha256": hashlib.sha256(output.read_bytes()).hexdigest(), "changed_entries": changed,
              "runtime_eye_projection_changed": False}
    output.with_suffix(".json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--fov", type=float, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.base, args.out, args.fov), indent=2))
