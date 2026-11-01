"""Read the installed SSLB stored-field declarations used by native grenade ownership.

No game writes or VM execution. Header and protobuf bounds are checked; the report
records payload hashes so a changed script layout can be distinguished from code.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile


def varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        if offset >= len(data):
            raise ValueError("truncated protobuf varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
    raise ValueError("oversized protobuf varint")


def fields(data):
    offset = 0
    result = []
    while offset < len(data):
        tag, offset = varint(data, offset)
        number, wire = tag >> 3, tag & 7
        if not number:
            raise ValueError("invalid protobuf field")
        if wire == 0:
            value, offset = varint(data, offset)
        elif wire == 2:
            size, offset = varint(data, offset)
            if size > len(data) - offset:
                raise ValueError("truncated protobuf field")
            value = data[offset:offset + size]
            offset += size
        else:
            raise ValueError(f"unsupported SSLB protobuf wire type {wire}")
        result.append((number, wire, value))
    return result


def stored_fields(payload, wanted):
    if len(payload) < 16 or payload[:4] != b"SSLB" or struct.unpack_from("<HH", payload, 4) != (7, 3):
        raise ValueError("unsupported SSLB header")
    table = fields(payload[16:])
    strings = [value.decode("utf-8") for number, wire, value in table if number == 6 and wire == 2]
    matches = []
    for number, wire, value in table:
        if number != 8 or wire != 2:
            continue
        declaration = fields(value)
        class_name = next(v for f, w, v in declaration if f == 1 and w == 0)
        if strings[class_name] != wanted:
            continue
        names = []
        for f, w, member in declaration:
            if f == 4 and w == 2:
                scalars = {f: v for f, w, v in fields(member) if w == 0}
                if scalars.get(3) == 6:
                    names.append(strings[scalars[1]])
        matches.append(names)
    if len(matches) != 1:
        raise ValueError(f"expected one declaration of {wanted}")
    return matches[0]


ENTRIES = {
    "PropBase": "ssl/ssl/prop_base_release.sslbin",
    "PropEquipment": "ssl/weapons/common/prop_equipment_release.sslbin",
    "PropEquipmentClient": "ssl/weapons/common/prop_equipment_client_release.sslbin",
}


def inspect(pak):
    with zipfile.ZipFile(pak) as archive:
        payloads = {name: archive.read(entry) for name, entry in ENTRIES.items()}
    declarations = {name: stored_fields(payload, name) for name, payload in payloads.items()}
    # Three VM header words plus the native association weak-reference word.
    offset = 8 * (4 + len(declarations["PropBase"]) + declarations["PropEquipment"].index("user"))
    if offset != 0xc0:
        raise ValueError(f"unsupported PropEquipment.user offset {offset:#x}")
    return {"user_offset": offset, "declarations": declarations,
            "sha256": {name: hashlib.sha256(payload).hexdigest() for name, payload in payloads.items()}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pak", type=Path)
    args = parser.parse_args()
    if not args.pak.is_absolute():
        parser.error("pak path must be absolute")
    print(json.dumps(inspect(args.pak), indent=2))
