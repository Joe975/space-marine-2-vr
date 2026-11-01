"""Build a reversible first-person camera experiment from the installed game's data.

No installed file is edited. Output is a Store-mode ZIP mod pack and a manifest.
The JSON preset is the tunable surface; rebuilding the pack does not rebuild the DLL.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import zipfile


# Value, minimum, maximum, unit. These are experimental defaults, not calibrated head tracking.
PARAMETERS = {
    'head_height': (2.5, 1.5, 3.5, 'game units above camera root'),
    'distance': (0.0, 0.0, 0.5, 'game units behind interest'),
    'lateral': (0.0, -0.5, 0.5, 'game units'),
    'forward': (0.15, -0.5, 0.5, 'game units'),
    'fov': (80.0, 60.0, 110.0, 'horizontal degrees'),
    'opacity_padding': (0.1, 0.0, 1.0, 'game units'),
    'opacity_offset': (0.4, 0.0, 1.0, 'game units'),
}
CAMERA_ROOT = 'ssl/camera/cam_modes/'
MODES = ('cam_idle', 'cam_sprint', 'cam_unarmed_mode', 'cam_aim_blind', 'cam_aim_minizoom',
         'cam_aim_minizoom_heavy', 'cam_aim_augmented_vision', 'cam_aim_augmented_vision_heavy',
         'cam_unarmed_minizoom', 'cam_unarmed_augmented_vision', 'cam_aim_scope', 'cam_melee',
         'cam_block_stance', 'cam_dodge')
CAMERAS = (CAMERA_ROOT + 'cam_base.sso',) + tuple(CAMERA_ROOT + 'mgs/' + m + '.sso' for m in MODES)
OPACITY = 'ssl/body/camera/prop_game_camera_opacity_controller.prop'


def parameters(overrides):
    unknown = set(overrides) - PARAMETERS.keys()
    if unknown:
        raise ValueError(f'Unknown parameters: {sorted(unknown)}')
    result = {name: spec[0] for name, spec in PARAMETERS.items()}
    result.update(overrides)
    for name, value in result.items():
        _, low, high, _ = PARAMETERS[name]
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
            raise ValueError(f'{name} must be finite and in [{low}, {high}]')
    return result


def replace_fields(text, replacements):
    """Replace complete column-zero fields in Saber's indented text export, preserving others.

    Reject an unexpected export layout rather than partially replacing nested fields. No eval.
    """
    text = text.replace('\r\n', '\n')
    matches = list(re.finditer(r'(?m)^(\w+)\s*=\s*', text))
    if not matches or matches[-1].group(1) != '__type' or len({m.group(1) for m in matches}) != len(matches):
        raise ValueError('Unexpected SSO root-field layout')
    pieces = []
    existing = {m.group(1) for m in matches}
    for i, match in enumerate(matches):
        name = match.group(1)
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        if name == '__type':
            for missing, value in replacements.items():
                if missing not in existing:
                    pieces.append(f'{missing} = {value}\n')
        pieces.append(f'{name} = {replacements[name]}\n' if name in replacements else text[match.start():end])
    return ''.join(pieces)


def object_text(type_name, **fields):
    """Emit the stock export layout: one assignment per line, including nested objects."""
    lines = ['{']
    for name, value in (*fields.items(), ('__type', f'"{type_name}"')):
        value = str(value).replace('\n', '\n   ')
        lines.append(f'   {name} = {value}')
    return '\n'.join((*lines, '}'))


def vector(x=0, y=0, z=0):
    return object_text('Vector3d', x=x, y=y, z=z)


def camera_patch(text, p):
    point = vector(p['lateral'], 0, p['forward'])
    return replace_fields(text, {
        'offsetRoot': object_text('CameraValueOffsetRootTracker', offset=vector(y=p['head_height']), isLocalSpace='True'),
        'distanceSettings': object_text('CameraValueDistance', distance=p['distance'], upShift=p['distance'], downShift=p['distance']),
        'interestSettings': object_text('CameraValueInterest', interest=point, upShift=point, downShift=point),
        'collisionOffsetSettings': object_text('CameraValueCollisionOffset', collisionOffset=point, upShift=point, downShift=point),
        'fovSettings': object_text('CameraValueFov', fov=p['fov'], isFowScaleEnabled='False'),
        'shakeSettings': object_text('ShakeSettings', isActive='False'),
    })


def build(source, output, overrides, *, camera_only=False):
    p = parameters(overrides)
    entries, hashes = {}, {}
    with zipfile.ZipFile(source) as archive:
        for name in (*CAMERAS, *((OPACITY,) if not camera_only else ())):
            info = archive.getinfo(name)
            if info.file_size > 2_000_000:
                raise ValueError(f'Unexpectedly large source: {name}')
            original = archive.read(name)
            hashes[name] = hashlib.sha256(original).hexdigest()
            text = original.decode('utf-8')
            if name in CAMERAS:
                text = camera_patch(text, p)
            else:
                # Stock disables hashed body opacity for STANDING_MODE (including the hangar).
                text = replace_fields(text, {
                    'bodyStateCondition': object_text('null'),
                    'distancePadding': str(p['opacity_padding']),
                    'additionalOpacityOffset': str(p['opacity_offset']),
                })
            entries[name] = text.encode('utf-8')
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_STORED) as archive:
        for name, content in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 11, 0, 0, 0))
            archive.writestr(info, content)
    manifest = {'parameters': p, 'schema': PARAMETERS, 'camera_only': camera_only, 'source': str(Path(source).resolve()),
                'source_entry_sha256': hashes, 'pak_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                'entries': sorted(entries), 'limitations': 'Prototype; special/cinematic modes and first-person weapon models are not implemented.'}
    output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--preset', type=Path, required=True)
    parser.add_argument('--camera-only', action='store_true', help='Omit body-opacity changes to isolate camera loading')
    args = parser.parse_args()
    print(json.dumps(build(args.source, args.out, json.loads(args.preset.read_text(encoding='utf-8')), camera_only=args.camera_only), indent=2))
