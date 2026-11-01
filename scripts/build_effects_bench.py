"""Build benchmark data packs that switch off groups of the game's post effects through its presets.

    .venv/Scripts/python.exe -I scripts/build_effects_bench.py --base-pack build/vr_controls/SM2VR_FirstPerson_Test.pak
        --presets-pack "<game>/client_pc/root/paks/client/default/default_presets.pak"
        --out-dir build/effects_bench [--variant bloom --variant bloom+lightshafts ...]

Each variant is a copy of the base pack (camera, controls, its motion-blur presets) plus every
preset of the named groups' types, edited to be off. Groups:

  bloom        hdr presets: Bloom.Intensity 0 and Bloom.Enabled False (added where absent).
               Tonemapping and exposure are untouched.
  lightshafts  lightshafts presets: params.enabled False, params.intensity 0.
  sun_flare    new_sky sunFlare.flareIntensity 0, sky sunFlare.flareColorIntensity 0 (the sun disc stays).
  camera_fx    chromatic aberration / RGB shift (image_effects Enabled False where present), vignette,
               vignette2, film grain (enabled False) and depth of field (dofParams.enabled False; a no-op
               on packs built since 23 Oct, whose base already switches depth of field off).

A variant name joins groups with '+'; 'all' is every group. Writes <out-dir>/<variant>/
SM2VR_FirstPerson_Test.pak and a .json manifest (source hashes, edited entries, edit counts).
Launch one with run-ordinary-headset.ps1 -EffectsPack <pak>. Diagnostic packs for performance
A/B only; the game's own lamp flares (GsFlareMng) are per-light and not covered by any preset.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

PAK_NAME = 'SM2VR_FirstPerson_Test.pak'
FIELD = re.compile(r'^(\s*)(\w+)(\s*=\s*)(.*?)\s*$')
OPEN = re.compile(r'^(\s*)(\w+)\s*=\s*([{\[])\s*$')

# group -> {preset type: [(block path below 'preset', field, value, insert_if_missing)]}
GROUPS: dict[str, dict[str, list[tuple[tuple[str, ...], str, str, bool]]]] = {
    'bloom': {'hdr': [(('Bloom',), 'Intensity', '0', False), (('Bloom',), 'Enabled', 'False', True)]},
    'lightshafts': {'lightshafts': [(('params',), 'enabled', 'False', False), (('params',), 'intensity', '0', False)]},
    'sun_flare': {'new_sky': [(('sunFlare',), 'flareIntensity', '0', False)],
                  'sky': [(('sunFlare',), 'flareColorIntensity', '0', False)]},
    'camera_fx': {'image_effects': [(('ChromaticAberration',), 'Enabled', 'False', False), (('RgbShift',), 'Enabled', 'False', False)],
                  'vignette': [((), 'enabled', 'False', False)],
                  'vignette2': [((), 'enabled', 'False', False)],
                  'film_grain_new': [((), 'enabled', 'False', False)],
                  'dof': [(('dofParams',), 'enabled', 'False', False)]},
}


def preset_type(text: str) -> str | None:
    found = re.findall(r'__type__\s*=\s*"([^"]+)"', text)
    return found[0] if len(found) == 1 else None


def patch(text: str, edits: list[tuple[tuple[str, ...], str, str, bool]]) -> tuple[str, int]:
    """Set each (block path, field) to value; returns (text, number of fields changed or added).

    The path is relative to the top-level 'preset = {' block. Fields are matched only at that exact
    depth, so a same-named field in a nested block is never touched. With insert_if_missing the field
    is added before the block's closing brace when the block exists but lacks it.
    """
    lines = text.replace('\r\n', '\n').replace('\r', '').split('\n')
    stack: list[str] = []
    seen: dict[tuple[tuple[str, ...], str], bool] = {}
    inserts: list[tuple[int, str]] = []
    changed = 0
    for i, line in enumerate(lines):
        opened = OPEN.match(line)
        if opened:
            stack.append(opened.group(2) if opened.group(3) == '{' else '[')
            continue
        if line.strip() in ('}', ']'):
            if not stack:
                raise ValueError('Unbalanced preset text')
            inner = tuple(stack[1:])
            for path, field, value, insert in edits:
                if insert and inner == path and not seen.get((path, field)):
                    indent = re.match(r'^(\s*)', line).group(1) + '   '
                    inserts.append((i, f'{indent}{field}   =   {value}'))
                    seen[(path, field)] = True
                    changed += 1
            stack.pop()
            continue
        if not stack or stack[0] != 'preset' or '[' in stack:
            continue
        m = FIELD.match(line)
        if not m:
            continue
        inner = tuple(stack[1:])
        for path, field, value, _ in edits:
            if inner == path and m.group(2) == field:
                if seen.get((path, field)):
                    raise ValueError(f'Duplicate field {field} in {path}')
                seen[(path, field)] = True
                if m.group(4) != value:
                    lines[i] = f'{m.group(1)}{field}{m.group(3)}{value}'
                    changed += 1
    if stack:
        raise ValueError('Unbalanced preset text')
    for index, new in reversed(inserts):
        lines.insert(index, new)
    return '\n'.join(lines), changed


def groups_of(variant: str) -> list[str]:
    names = sorted(GROUPS) if variant == 'all' else variant.split('+')
    unknown = [n for n in names if n not in GROUPS]
    if unknown or not names:
        raise ValueError(f'Unknown effect group(s) {unknown}; known: {sorted(GROUPS)} or all')
    return names


def build(base_pack: Path, presets_pack: Path, out_dir: Path, variant: str) -> dict:
    groups = groups_of(variant)
    edits: dict[str, list] = {}
    for g in groups:
        for kind, rows in GROUPS[g].items():
            edits.setdefault(kind, []).extend(rows)
    entries: dict[str, bytes] = {}
    with zipfile.ZipFile(base_pack) as z:
        for info in z.infolist():
            entries[info.filename] = z.read(info)
    base_entries = set(entries)
    hashes, counts, per_type = {}, 0, {}
    with zipfile.ZipFile(presets_pack) as z:
        for info in z.infolist():
            if not info.filename.endswith('.s3dprs') or info.file_size > 200_000:
                continue
            # A preset the base pack already overrides (motion blur, depth of field) is edited from its copy.
            original = entries[info.filename] if info.filename in base_entries else z.read(info)
            text = original.decode('utf-8')
            kind = preset_type(text)
            if kind not in edits:
                continue
            patched, changed = patch(text, edits[kind])
            if not changed:
                continue
            entries[info.filename] = patched.encode('utf-8')
            hashes[info.filename] = hashlib.sha256(original).hexdigest()
            counts += changed
            per_type[kind] = per_type.get(kind, 0) + 1
    if not hashes:
        raise ValueError('No preset was edited')
    name = '+'.join(groups) if variant != 'all' else 'all'
    output = out_dir / name / PAK_NAME
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_STORED) as z:
        for entry, data in sorted(entries.items()):
            z.writestr(zipfile.ZipInfo(entry, date_time=(2026, 10, 23, 0, 0, 0)), data)
    manifest = {'variant': name, 'groups': groups,
                'base_pack_sha256': hashlib.sha256(base_pack.read_bytes()).hexdigest(),
                'presets_pack_sha256': hashlib.sha256(presets_pack.read_bytes()).hexdigest(),
                'edited_presets_by_type': per_type, 'field_edits': counts,
                'source_entry_sha256': hashes,
                'pak_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                'purpose': 'Benchmark only: base controls pack plus post-effect presets switched off.'}
    output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--base-pack', type=Path, required=True)
    parser.add_argument('--presets-pack', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--variant', action='append', help="group, 'a+b' or 'all'; repeatable (default: each group and all)")
    args = parser.parse_args()
    for v in args.variant or [*sorted(GROUPS), 'all']:
        m = build(args.base_pack, args.presets_pack, args.out_dir, v)
        print(f"{m['variant']}: {m['field_edits']} edits in {m['edited_presets_by_type']}")
