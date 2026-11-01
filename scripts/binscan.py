"""Static, read-only inspection of the game's binaries.

Usage (run with the repo venv, isolated mode, since the inputs are untrusted):
    .venv/Scripts/python.exe -I scripts/binscan.py strings <binary> <out.txt>
    .venv/Scripts/python.exe -I scripts/binscan.py pe <binary> <out.txt>

`strings` writes one line per string: "<hex file offset>\t<a|u>\t<text>" (a = ASCII, u = UTF-16LE).
`pe` writes headers, sections with entropy, debug/PDB info, imports and exports.
"""
import math
import mmap
import re
import sys
from collections import Counter

MIN_LEN = 6
_ASCII = re.compile(rb"[\x20-\x7e]{%d,}" % MIN_LEN)
_UTF16 = re.compile(rb"(?:[\x20-\x7e]\x00){%d,}" % MIN_LEN)


def extract_strings(data):
    """Yield (offset, kind, text) for printable ASCII and UTF-16LE runs, in offset order per kind."""
    for m in _ASCII.finditer(data):
        yield m.start(), "a", m.group().decode("ascii")
    for m in _UTF16.finditer(data):
        yield m.start(), "u", m.group().decode("utf-16-le")


def entropy(data):
    """Shannon entropy in bits per byte (0.0 for empty input)."""
    if not data:
        return 0.0
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in Counter(data).values())


def cmd_strings(path, out):
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm, \
            open(out, "w", encoding="utf-8", newline="\n") as o:
        count = 0
        for off, kind, text in extract_strings(mm):
            o.write(f"{off:08x}\t{kind}\t{text}\n")
            count += 1
    print(f"{count} strings -> {out}")


def cmd_pe(path, out):
    import datetime
    import pefile

    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY[n] for n in (
            "IMAGE_DIRECTORY_ENTRY_IMPORT", "IMAGE_DIRECTORY_ENTRY_EXPORT",
            "IMAGE_DIRECTORY_ENTRY_DEBUG", "IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT",
            "IMAGE_DIRECTORY_ENTRY_TLS")])
    with open(out, "w", encoding="utf-8", newline="\n") as o:
        ts = pe.FILE_HEADER.TimeDateStamp
        o.write(f"file: {path}\n")
        o.write(f"timestamp: {ts:#x} {datetime.datetime.fromtimestamp(ts, datetime.timezone.utc)}\n")
        o.write(f"image base: {pe.OPTIONAL_HEADER.ImageBase:#x}\n")
        o.write(f"entry rva: {pe.OPTIONAL_HEADER.AddressOfEntryPoint:#x}\n")
        o.write(f"dll characteristics: {pe.OPTIONAL_HEADER.DllCharacteristics:#x}\n")
        o.write(f"tls callbacks: {'yes' if hasattr(pe, 'DIRECTORY_ENTRY_TLS') and pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks else 'no'}\n")
        o.write("\n[sections] name vaddr vsize rawsize entropy flags\n")
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode("latin-1")
            o.write(f"{name:<10} {s.VirtualAddress:#010x} {s.Misc_VirtualSize:#010x} "
                    f"{s.SizeOfRawData:#010x} {entropy(s.get_data()):.2f} {s.Characteristics:#x}\n")
        o.write("\n[debug]\n")
        for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
            e = d.entry
            pdb = getattr(e, "PdbFileName", None)
            if pdb:
                o.write(f"pdb: {pdb.rstrip(bytes(1)).decode('latin-1')}\n")
        for label, attr in (("imports", "DIRECTORY_ENTRY_IMPORT"), ("delay imports", "DIRECTORY_ENTRY_DELAY_IMPORT")):
            o.write(f"\n[{label}]\n")
            for imp in getattr(pe, attr, []):
                names = [(i.name.decode("latin-1") if i.name else f"#{i.ordinal}") for i in imp.imports]
                o.write(f"{imp.dll.decode('latin-1')} ({len(names)})\n")
                for n in names:
                    o.write(f"    {n}\n")
        o.write("\n[exports]\n")
        exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
        if exp:
            o.write(f"count: {len(exp.symbols)}\n")
            for s in exp.symbols:
                o.write(f"    {s.address:#010x} {s.name.decode('latin-1') if s.name else '#' + str(s.ordinal)}\n")
    print(f"pe info -> {out}")


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("strings", "pe"):
        sys.exit(__doc__)
    {"strings": cmd_strings, "pe": cmd_pe}[sys.argv[1]](sys.argv[2], sys.argv[3])
