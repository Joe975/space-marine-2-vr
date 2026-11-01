"""Poll a few of the game's globals (by RVA) from the running process, read-only, and print
each change with a timestamp. Dev tool, companion of dump_game_globals.py.

    watch_game_globals.py 0x44f54c8 0x4dd8020:4 0x4f17744 [--seconds 600] [--interval 0.1]

An RVA takes an optional :size (1, 2, 4 or 8 bytes; default 4).
"""
import argparse
import ctypes
import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from dump_game_globals import EXE, PROCESS_QUERY_INFORMATION, PROCESS_VM_READ, find_pid, module_base, read  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rvas", nargs="+")
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--interval", type=float, default=0.1)
    ap.add_argument("--pid", type=int)
    a = ap.parse_args()
    pid = a.pid or find_pid(EXE)
    handle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    base = module_base(handle)
    targets = []
    for text in a.rvas:
        rva, _, size = text.partition(":")
        targets.append((int(rva, 16), int(size or 4)))
    last = None
    end = time.time() + a.seconds
    while time.time() < end:
        values = []
        for rva, size in targets:
            raw = read(handle, base + rva, size)
            values.append(int.from_bytes(raw, "little") if raw else None)
        if values != last:
            stamp = time.strftime("%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"
            print(stamp, " ".join(f"{rva:#x}={v:#x}" if v is not None else f"{rva:#x}=?" for (rva, _), v in zip(targets, values)), flush=True)
            last = values
        time.sleep(a.interval)


if __name__ == "__main__":
    sys.exit(main())
