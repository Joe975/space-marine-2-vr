import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1] / "scripts/inspect_grenade_equipment_layout.py"
spec = importlib.util.spec_from_file_location("grenade_layout", PATH)
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


def var(value):
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    return bytes(out) + bytes([value])


def scalar(field, value):
    return var(field << 3) + var(value)


def blob(field, value):
    return var((field << 3) | 2) + var(len(value)) + value


class GrenadeLayoutTests(unittest.TestCase):
    def test_native_owner_offset_rejects_shifted_installed_scripts(self):
        def payload(name, names):
            strings = [name.encode(), *(n.encode() for n in names)]
            declaration = scalar(1, 0) + b"".join(blob(4, scalar(1, i) + scalar(3, 6)) for i in range(1, len(strings)))
            return b"SSLB\x07\x00\x03\x00" + bytes(8) + b"".join(blob(6, s) for s in strings) + blob(8, declaration)
        data = {layout.ENTRIES["PropBase"]: payload("PropBase", [f"base{i}" for i in range(11)]),
                layout.ENTRIES["PropEquipment"]: payload("PropEquipment", [f"equipment{i}" for i in range(9)] + ["user"]),
                layout.ENTRIES["PropEquipmentClient"]: payload("PropEquipmentClient", ["syncToken", "netUser"])}
        with patch.object(layout.zipfile, "ZipFile") as archive:
            archive.return_value.__enter__.return_value.read.side_effect = data.__getitem__
            self.assertEqual(layout.inspect(Path("C:/fixture.pak"))["user_offset"], 192)
            data[layout.ENTRIES["PropEquipment"]] = payload("PropEquipment", ["extra"] + [f"equipment{i}" for i in range(9)] + ["user"])
            with self.assertRaisesRegex(ValueError, "unsupported PropEquipment.user offset"):
                layout.inspect(Path("C:/fixture.pak"))

    def test_declaration_order_ignores_methods_constants_and_editor_metadata(self):
        strings = [b"PropEquipment", b"GetUser", b"first", b"user"]
        members = b"".join(blob(4, scalar(1, name) + scalar(3, kind))
                           for name, kind in [(1, 2), (2, 6), (1, 7), (3, 6)])
        payload = b"SSLB\x07\x00\x03\x00" + bytes(8)
        payload += b"".join(blob(6, s) for s in strings) + blob(8, scalar(1, 0) + members)
        self.assertEqual(layout.stored_fields(payload, "PropEquipment"), ["first", "user"])
        with self.assertRaises(ValueError):
            layout.stored_fields(payload, "UnknownClass")

    def test_truncated_or_changed_format_fails_closed(self):
        for data in [b"", b"SSLB" + bytes(12), b"\x0a\x05x", b"\x80", b"\0", b"\x0f"]:
            with self.assertRaises(ValueError):
                if data.startswith(b"SSLB") or not data:
                    layout.stored_fields(data, "PropEquipment")
                else:
                    layout.fields(data)


if __name__ == "__main__":
    unittest.main()
