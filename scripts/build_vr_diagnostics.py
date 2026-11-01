"""Copy a camera mod into a deterministic diagnostic pack with motion blur, depth of field and lens
distortion disabled.

Installed archives are read-only. Every headset pack (menus, controls, chase cam, the installer's)
is built on top of this one. Depth of field blurs what the player's eyes focus on in a headset, so
every DoF preset (gameplay and cinematic) is switched off; it costs nothing measurable either way
(docs/validation/effects-benchmark-2026-10-23.md). The default camera's lens distortion bends each eye
image away from its projection, pushing each eye's scene about 15 pixels outward near the centre of a
3840-wide eye: the world's stereo depth was off by about 1 degree of vergence, and anything drawn into
the eyes in 3D (the moved crosshair) showed double (docs/THIRD_PERSON.md, 30 Oct). It is switched off.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

PRESETS = tuple('presets/data/default_blur/motion_blur_' + quality + '.s3dprs'
                for quality in ('low', 'med', 'high')) + (
    'presets/data/techlight_preset/motion_blur_shading_scene.s3dprs',)
LENS_DISTORTION = 'presets/data/default_camera_effects/lense_distortion.s3dprs'


def disable_motion_blur(text):
    text = text.replace('\r\n', '\n').replace('\r', '')
    if len(re.findall(r'__type__\s*=\s*"motion_blur"', text)) != 1:
        raise ValueError('Unexpected motion-blur preset type')
    patched, count = re.subn(r'(?m)^(\s*enabled\s*=\s*)(True|False)\s*$', r'\g<1>False', text)
    if count != 1:
        raise ValueError('Expected exactly one enabled field')
    for field in ('scale', 'scaleObject', 'scaleCamera'):
        patched, count = re.subn(r'(?m)^(\s*' + field + r'\s*=\s*)[-+0-9.eE]+\s*$', r'\g<1>0', patched)
        if count > 1:
            raise ValueError('Duplicate motion-blur scale field')
    return patched


def disable_dof(text):
    """Set dofParams.enabled to False. Returns the text, or None for a preset with nothing to switch
    (an empty preset, or already off). Nested blocks' own 'enabled' fields (dynamicFocus) stay."""
    text = text.replace('\r\n', '\n').replace('\r', '')
    if len(re.findall(r'__type__\s*=\s*"dof"', text)) != 1:
        raise ValueError('Unexpected depth-of-field preset type')
    lines, stack, found, changed = text.split('\n'), [], 0, False
    for i, line in enumerate(lines):
        opened = re.match(r'^\s*(\w+)\s*=\s*([{\[])\s*$', line)
        if opened:
            stack.append(opened.group(1) if opened.group(2) == '{' else '[')
            continue
        if line.strip() in ('}', ']'):
            if not stack:
                raise ValueError('Unbalanced depth-of-field preset')
            stack.pop()
            continue
        field = re.match(r'^(\s*enabled\s*=\s*)(True|False)\s*$', line)
        if field and stack == ['preset', 'dofParams']:
            found += 1
            if field.group(2) == 'True':
                lines[i], changed = field.group(1) + 'False', True
    if stack or found > 1:
        raise ValueError('Unbalanced depth-of-field preset or duplicate enabled field')
    return '\n'.join(lines) if changed else None


def disable_lens_distortion(text):
    """Set the lens distortion preset's params.enabled to False; its strengths stay as they are."""
    text = text.replace('\r\n', '\n').replace('\r', '')
    if len(re.findall(r'__type__\s*=\s*"screen_effect_13"', text)) != 1:
        raise ValueError('Unexpected lens distortion preset type')
    lines, stack, found = text.split('\n'), [], 0
    for i, line in enumerate(lines):
        opened = re.match(r'^\s*(\w+)\s*=\s*([{\[])\s*$', line)
        if opened:
            stack.append(opened.group(1) if opened.group(2) == '{' else '[')
            continue
        if line.strip() in ('}', ']'):
            if not stack:
                raise ValueError('Unbalanced lens distortion preset')
            stack.pop()
            continue
        field = re.match(r'^(\s*enabled\s*=\s*)(True|False)\s*$', line)
        if field and stack == ['preset', 'params']:
            found += 1
            lines[i] = field.group(1) + 'False'
    if stack or found != 1:
        raise ValueError('Expected exactly one params.enabled in the lens distortion preset')
    return '\n'.join(lines)


def build(camera_pack, presets_pack, output):
    entries, hashes = {}, {}
    with zipfile.ZipFile(camera_pack) as z:
        for entry in z.infolist():
            if not entry.filename.startswith('ssl/camera/') or entry.file_size > 2_000_000 or entry.filename in entries:
                raise ValueError('Expected a camera-only source pack')
            entries[entry.filename] = z.read(entry)
    if not entries:
        raise ValueError('Camera pack is empty')
    with zipfile.ZipFile(presets_pack) as z:
        for name in PRESETS:
            if z.getinfo(name).file_size > 20_000:
                raise ValueError('Unexpected preset size')
            original = z.read(name)
            hashes[name] = hashlib.sha256(original).hexdigest()
            entries[name] = disable_motion_blur(original.decode('utf-8')).encode('utf-8')
        if z.getinfo(LENS_DISTORTION).file_size > 20_000:
            raise ValueError('Unexpected preset size')
        original = z.read(LENS_DISTORTION)
        hashes[LENS_DISTORTION] = hashlib.sha256(original).hexdigest()
        entries[LENS_DISTORTION] = disable_lens_distortion(original.decode('utf-8')).encode('utf-8')
        for info in z.infolist():
            if not info.filename.endswith('.s3dprs') or info.file_size > 20_000 or info.filename in entries:
                continue
            original = z.read(info)
            text = original.decode('utf-8')
            if not re.search(r'__type__\s*=\s*"dof"', text):
                continue
            patched = disable_dof(text)
            if patched is not None:
                hashes[info.filename] = hashlib.sha256(original).hexdigest()
                entries[info.filename] = patched.encode('utf-8')
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_STORED) as z:
        for name, data in sorted(entries.items()):
            z.writestr(zipfile.ZipInfo(name, date_time=(2026, 10, 12, 0, 0, 0)), data)
    manifest = {'camera_pack_sha256': hashlib.sha256(Path(camera_pack).read_bytes()).hexdigest(),
                'source_entry_sha256': hashes, 'entries': sorted(entries),
                'pak_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                'purpose': 'Camera-only first person plus disabled motion-blur, depth-of-field and lens-distortion presets.'}
    output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--camera-pack', type=Path, required=True)
    parser.add_argument('--presets-pack', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.camera_pack, args.presets_pack, args.out), indent=2))
