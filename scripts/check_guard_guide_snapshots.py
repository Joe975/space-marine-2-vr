"""Check guard-guide eye snapshots (docs/GUARD_GUIDE.md) and build a review sheet.

Usage:
  python scripts/check_guard_guide_snapshots.py DUMP_DIR name=state [name=state ...] [--sheet OUT.png]

Each name is a snapshot folder written by an -EyeSnapshots launch (left.ppm, right.ppm,
pair.json). state is the guide state that snapshot must show in both eyes: hidden, idle,
turn, blocked or active. pair.json records the state the capture actually drew and, when
drawn, the grip marker's pixel in each eye (from the renderer's own projection). The check
passes when every snapshot exists and both eyes drew the expected state. The sheet shows a
crop of the left eye around the grip marker (or the lower centre when hidden) per snapshot,
with the mean colour at the marker, for a person to judge the look.
"""
import argparse
import json
import sys
from pathlib import Path

STATES = ('hidden', 'idle', 'turn', 'blocked', 'active')


def read_ppm(path):
    data = Path(path).read_bytes()
    parts, offset = [], 0
    while len(parts) < 4:  # P6, width, height, maxval; whitespace separated
        while data[offset:offset + 1].isspace():
            offset += 1
        end = offset
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[offset:end])
        offset = end
    if parts[0] != b'P6' or parts[3] != b'255':
        raise ValueError(f'{path}: not an 8-bit P6 image')
    width, height = int(parts[1]), int(parts[2])
    return width, height, data[offset + 1:offset + 1 + width * height * 3]


def mean_colour(image, x, y, radius=2):
    width, height, rgb = image
    total, count = [0, 0, 0], 0
    for yy in range(max(0, int(y) - radius), min(height, int(y) + radius + 1)):
        for xx in range(max(0, int(x) - radius), min(width, int(x) + radius + 1)):
            index = (yy * width + xx) * 3
            for channel in range(3):
                total[channel] += rgb[index + channel]
            count += 1
    return [round(value / count) for value in total] if count else None


def check(dump, expectations):
    """Returns (ok, rows); rows describe each snapshot for the report and sheet."""
    rows, ok = [], True
    for name, expected in expectations:
        folder = Path(dump) / name
        row = {'name': name, 'expected': expected, 'folder': folder}
        try:
            pair = json.loads((folder / 'pair.json').read_text())
        except (OSError, json.JSONDecodeError) as error:
            row['error'] = f'no snapshot ({error.__class__.__name__})'
            rows.append(row)
            ok = False
            continue
        eyes = pair.get('eyes', [])
        row['states'] = [eye.get('guard_guide', 'missing') for eye in eyes]
        row['pixels'] = [eye.get('guard_guide_pixel') for eye in eyes]
        row['ok'] = len(eyes) == 2 and all(state == expected for state in row['states']) and \
            all((pixel is not None) == (expected != 'hidden') for pixel in row['pixels'])
        if row['pixels'] and row['pixels'][0] and (folder / 'left.ppm').exists():
            row['colour'] = mean_colour(read_ppm(folder / 'left.ppm'), *row['pixels'][0])
        ok &= row['ok']
        rows.append(row)
    return ok, rows


def sheet(rows, output, crop=360):
    from PIL import Image, ImageDraw
    tiles = []
    for row in rows:
        path = row['folder'] / 'left.ppm'
        if not path.exists():
            continue
        image = Image.open(path).convert('RGB')
        pixel = (row.get('pixels') or [None])[0]
        cx, cy = pixel if pixel else (image.width / 2, image.height * 0.72)
        box = (int(cx - crop), int(cy - crop * 0.6), int(cx + crop), int(cy + crop * 0.6))
        tile = image.crop(box).resize((crop, int(crop * 0.6)))
        draw = ImageDraw.Draw(tile)
        label = f"{row['name']}: {'/'.join(row.get('states', ['?']))} (want {row['expected']})"
        draw.rectangle((0, 0, tile.width, 18), fill=(0, 0, 0))
        draw.text((4, 3), label, fill=(255, 255, 255) if row.get('ok') else (255, 90, 90))
        tiles.append(tile)
    if not tiles:
        return None
    columns = min(3, len(tiles))
    rows_count = (len(tiles) + columns - 1) // columns
    out = Image.new('RGB', (columns * tiles[0].width, rows_count * tiles[0].height))
    for index, tile in enumerate(tiles):
        out.paste(tile, ((index % columns) * tile.width, (index // columns) * tile.height))
    out.save(output)
    return output


def parse_expectation(text):
    name, _, state = text.partition('=')
    if not name or state not in STATES:
        raise argparse.ArgumentTypeError(f'expected name=state with state one of {", ".join(STATES)}: {text}')
    return name, state


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dump')
    parser.add_argument('expectations', nargs='+', type=parse_expectation)
    parser.add_argument('--sheet')
    args = parser.parse_args(argv[1:])
    ok, rows = check(args.dump, args.expectations)
    for row in rows:
        if 'error' in row:
            print(f"{row['name']}: FAIL {row['error']}")
            continue
        print(f"{row['name']}: {'ok' if row['ok'] else 'FAIL'} states={row['states']} want={row['expected']} "
              f"pixels={row['pixels']} colour={row.get('colour')}")
    if args.sheet:
        print('sheet', sheet(rows, args.sheet))
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
