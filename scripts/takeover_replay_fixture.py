"""Cut takeover windows out of a playtest trace for the body following replay test.

Usage: takeover_replay_fixture.py TRACE --window START END [--window ...] --out FIXTURE [--buttons]

Each output line is one trace sample with a camera, a character and the pad sent to the game:

    window t root_x root_y root_z game_x game_y game_z forward_x forward_z stick_x stick_y units_per_metre mode

t is trace seconds, positions are world units, forward is the game camera's horizontal forward,
stick is the native left stick over 32767 and mode is the camera mode the mod showed. The replay
(tests/test_body_following.cpp, takeover_replay) feeds these to VrBodyFollowing at their own
timestamps (docs/TAKEOVER_RETURN_2026-10-26.md). --buttons adds the pad buttons sent to the game
after mode, in hex (combo_replay, docs/ANIMATION_CAMERA.md).
"""
import argparse
import json


def rows(trace, windows, buttons=False):
    for line in trace:
        d = json.loads(line)
        if d.get("type") != "sample":
            continue
        t = d["t"]
        window = next((i for i, (a, b) in enumerate(windows) if a <= t <= b), None)
        if window is None:
            continue
        c, ch, inp = d.get("camera"), d.get("character"), d.get("input")
        if not c or not ch or not inp or not c.get("game") or "pad" not in inp:
            continue
        g, r, f, pad = c["game"], ch["root"], c["game_forward"], inp["pad"]
        yield (f"{window} {t:.3f} {r[0]:.4f} {r[1]:.4f} {r[2]:.4f} {g[0]:.4f} {g[1]:.4f} {g[2]:.4f} "
               f"{f[0]:.4f} {f[2]:.4f} {pad['lx'] / 32767:.4f} {pad['ly'] / 32767:.4f} {ch['units_per_metre']:.4f} {c['mode']}"
               + (f" {pad['buttons']:x}" if buttons else ""))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace")
    parser.add_argument("--window", nargs=2, type=float, action="append", required=True, metavar=("START", "END"))
    parser.add_argument("--out", required=True)
    parser.add_argument("--buttons", action="store_true", help="add the pad buttons sent to the game")
    args = parser.parse_args(argv)
    with open(args.trace, encoding="utf-8") as trace, open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write(f"# {args.trace.replace(chr(92), '/').split('/build/')[-1]} windows {args.window}\n")
        for row in rows(trace, args.window, args.buttons):
            out.write(row + "\n")


if __name__ == "__main__":
    main()
