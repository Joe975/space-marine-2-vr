"""Build the -ChaseCam data pack: the VR menus pack without its first-person camera files.

The game then loads its own cameras, so the native camera is the stock over-the-shoulder chase
camera and the mod renders it in stereo (docs/THIRD_PERSON.md). Everything else in the base
pack (motion-blur presets, VR menus) is copied byte for byte. Installed archives are not read.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

CAMERA_PREFIX = 'ssl/camera/'


def build(base, output):
    entries = {}
    removed = []
    with zipfile.ZipFile(base) as z:
        for entry in z.infolist():
            if entry.filename in entries or entry.filename in removed:
                raise ValueError(f'Duplicate entry {entry.filename}')
            if entry.filename.startswith(CAMERA_PREFIX):
                removed.append(entry.filename)
            else:
                entries[entry.filename] = (entry.date_time, z.read(entry))
    if not removed:
        raise ValueError('Base pack has no first-person camera files; expected the VR menus pack')
    if not entries:
        raise ValueError('Base pack has nothing besides camera files')
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_STORED) as z:
        for name, (date_time, data) in sorted(entries.items()):
            z.writestr(zipfile.ZipInfo(name, date_time=date_time), data)
    manifest = {'base_pack_sha256': hashlib.sha256(Path(base).read_bytes()).hexdigest(),
                'removed': sorted(removed), 'entries': sorted(entries),
                'pak_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                'purpose': 'Stock game cameras (chase camera) with the VR menus; for -ChaseCam launches.'}
    output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True, help='build/vr_menus/SM2VR_FirstPerson_Test.pak')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.base, args.out), indent=2))
