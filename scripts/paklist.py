"""List the contents of the game's .pak archives (ZIP containers) without extracting anything.

Usage (repo venv, isolated mode; the paks are untrusted input):
    .venv/Scripts/python.exe -I scripts/paklist.py <out_dir> <pak> [<pak> ...]

Writes <out_dir>/<pak name>.list.txt with "<size>\t<compressed>\t<method>\t<name>" per entry and
prints a per-pak summary (entry count, compression methods, top extensions).
"""
import sys
import zipfile
from collections import Counter
from pathlib import Path


def summarize(infos):
    """Return (entry_count, Counter of compress_type, Counter of file extension) for ZipInfo-like items."""
    methods = Counter(i.compress_type for i in infos)
    exts = Counter(ext_of(i.filename) for i in infos if not i.filename.endswith("/"))
    return len(infos), methods, exts


def ext_of(name):
    """Extension including compound suffixes after the first dot of the base name, e.g. '.ssl.resource'."""
    base = name.rsplit("/", 1)[-1]
    dot = base.find(".")
    return base[dot:].lower() if dot > 0 else "(none)"


def main(out_dir, paks):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for pak in paks:
        p = Path(pak)
        with open(p, "rb") as f:
            magic = f.read(4)
        if not zipfile.is_zipfile(p):
            print(f"{p.name}: not a zip (magic {magic!r})")
            continue
        with zipfile.ZipFile(p) as z:
            infos = z.infolist()
        with open(out / (p.name + ".list.txt"), "w", encoding="utf-8", newline="\n") as o:
            for i in infos:
                o.write(f"{i.file_size}\t{i.compress_size}\t{i.compress_type}\t{i.filename}\n")
        n, methods, exts = summarize(infos)
        top = ", ".join(f"{e} x{c}" for e, c in exts.most_common(8))
        print(f"{p.name}: {n} entries, methods {dict(methods)}, {top}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2:])
