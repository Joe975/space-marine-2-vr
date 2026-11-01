"""Dump the game's writable image sections (its globals) from the running process, read-only.

Dev tool for finding a native state flag by diffing dumps taken in different game states:

    dump_game_globals.py --label menu      # writes build/globals/<time>_menu.bin (+ .json)
    dump_game_globals.py --label hangar
    dump_game_globals.py --diff build/globals/*.bin

The diff lists offsets (as RVAs) whose 4-byte value is the same in every dump of one label and
differs between labels. Only ReadProcessMemory is used; nothing in the game is written.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = "Warhammer 40000 Space Marine 2 - Retail.exe"
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
IMAGE_SCN_MEM_WRITE = 0x80000000


def find_pid(name):
    out = ctypes.create_string_buffer(1)
    k32 = ctypes.windll.kernel32
    TH32CS_SNAPPROCESS = 2

    class PE32(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD),
                    ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_char * 260)]
    del out
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = PE32()
    entry.dwSize = ctypes.sizeof(PE32)
    found = []
    ok = k32.Process32First(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.decode(errors="replace").lower() == name.lower():
            found.append(entry.th32ProcessID)
        ok = k32.Process32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    if len(found) != 1:
        raise SystemExit(f"expected one {name}, found {found}")
    return found[0]


def module_base(handle):
    psapi = ctypes.windll.psapi
    mods = (ctypes.c_void_p * 1)()
    needed = wt.DWORD()
    if not psapi.EnumProcessModulesEx(handle, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3):
        raise SystemExit("EnumProcessModulesEx failed")
    return mods[0]


def read(handle, address, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ctypes.windll.kernel32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    if not ctypes.windll.kernel32.ReadProcessMemory(handle, ctypes.c_void_p(address), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def sections(handle, base):
    dos = read(handle, base, 0x40)
    lfanew = struct.unpack_from("<I", dos, 0x3c)[0]
    nt = read(handle, base + lfanew, 0x108)
    stamp = struct.unpack_from("<I", nt, 8)[0]
    count = struct.unpack_from("<H", nt, 6)[0]
    optional = struct.unpack_from("<H", nt, 20)[0]
    table = read(handle, base + lfanew + 24 + optional, 40 * count)
    out = []
    for i in range(count):
        name, vsize, rva, _, _, _, _, _, _, chars = struct.unpack_from("<8sIIIIIIHHI", table, 40 * i)
        out.append((name.rstrip(b"\0").decode(), rva, vsize, chars))
    return stamp, out


def dump(label, pid=None):
    pid = pid or find_pid(EXE)
    handle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not handle:
        raise SystemExit(f"OpenProcess({pid}) failed")
    base = module_base(handle)
    stamp, table = sections(handle, base)
    out_dir = ROOT / "build" / "globals"
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = out_dir / f"{time.strftime('%Y%m%d_%H%M%S')}_{label}"
    blob = bytearray()
    index = []
    for name, rva, vsize, chars in table:
        if not chars & IMAGE_SCN_MEM_WRITE:
            continue
        data = bytearray()
        page = 0x1000
        for off in range(0, vsize, page):
            chunk = read(handle, base + rva + off, min(page, vsize - off))
            data += chunk if chunk else b"\0" * min(page, vsize - off)
        index.append({"name": name, "rva": rva, "size": vsize, "offset": len(blob)})
        blob += data
    stem.with_suffix(".bin").write_bytes(blob)
    stem.with_suffix(".json").write_text(json.dumps({"label": label, "pid": pid, "stamp": hex(stamp), "base": hex(base), "sections": index}, indent=1))
    print(f"{stem}.bin {len(blob)} bytes, stamp {stamp:#x}, sections {[s['name'] for s in index]}")


def diff(paths, limit):
    dumps = []
    for p in paths:
        p = Path(p)
        meta = json.loads(p.with_suffix(".json").read_text())
        dumps.append((meta, p.read_bytes()))
    labels = sorted({m["label"] for m, _ in dumps})
    if len(labels) < 2:
        raise SystemExit("need at least two labels")
    sections_ = dumps[0][0]["sections"]
    size = min(len(b) for _, b in dumps) // 4 * 4
    import array
    words = [array.array("I", b[:size]) for _, b in dumps]
    by_label = {l: [w for (m, _), w in zip(dumps, words) if m["label"] == l] for l in labels}
    hits = []
    for i in range(size // 4):
        values = {}
        ok = True
        for l, ws in by_label.items():
            v = {w[i] for w in ws}
            if len(v) != 1:
                ok = False
                break
            values[l] = v.pop()
        if ok and len(set(values.values())) == len(values):
            off = i * 4
            sec = next(s for s in sections_ if s["offset"] <= off < s["offset"] + s["size"])
            hits.append((sec["rva"] + off - sec["offset"], values))
    print(f"{len(hits)} words differ consistently between {labels} ({ {l: len(v) for l, v in by_label.items()} } dumps)")
    for rva, values in hits[:limit]:
        print(f"  {rva:#x} " + " ".join(f"{l}={v:#x}" for l, v in values.items()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--diff", nargs="+")
    ap.add_argument("--limit", type=int, default=200)
    a = ap.parse_args()
    if a.diff:
        diff(a.diff, a.limit)
    elif a.label:
        dump(a.label, a.pid)
    else:
        ap.error("--label or --diff")


if __name__ == "__main__":
    sys.exit(main())
