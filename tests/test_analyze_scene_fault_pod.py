import re
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import analyze_scene_fault_pod as pod  # noqa: E402


def make(chain=7, rdx=0x656B6E75625F706D, target=b"mp_bunker_training\0"):
    header = [pod.MAGIC, pod.VERSION, 0, 1, 2, 0x6AD10FCB, 0xC0000005, 1, 0, chain, 5, 0x11, 2, 1,
              1, 0x7FF7F2290000, 0x7FF7F2290000 + 0x26E2D15, 0, 2**64 - 1, 0x1234, 15608, 0x2000, 0x3000, 0x3090, 0x4000]
    registers = [1, rdx + 0x70, rdx] + [0] * 13
    regions = b""
    for i in range(6):
        raw = target if i == 4 else b""
        regions += pod.REGION.pack(0x4000 if i == 4 else 0, 0, 0, 0, 0x1000 if raw else 0, 4 if raw else 0, 0, len(raw), raw)
    data = bytearray(pod.HEADER.pack(*header, *registers) + regions)
    struct.pack_into("<I", data, 8, len(data))
    return bytes(data)


class SceneFaultPodTest(unittest.TestCase):
    def test_layout_matches_the_native_struct(self):
        header = (ROOT / "mod/src/NativeSceneFaultEvidence.h").read_text()
        size = int(re.search(r"sizeof\(Evidence\)==(\d+)", header).group(1))
        self.assertEqual(pod.HEADER.size + 6 * pod.REGION.size, size)

    def test_decodes_the_23_october_fault(self):
        value = pod.parse(make())
        self.assertEqual(value["rip_rva"], 0x26E2D15)
        self.assertEqual(value["rdx_ascii"], "mp_bunke")
        self.assertEqual(value["entry_target_ascii"], "mp_bunker_training")
        self.assertEqual(value["chain_names"], ["holder_return", "caller_return", "index_read"])
        self.assertEqual((value["index"], value["scene_slot"], value["scene_flags"], value["scene_dispatch"]), (5, 2, 0x11, 15608))
        self.assertFalse(value["cause_verified"])

    def test_pointer_values_are_not_text(self):
        value = pod.parse(make(chain=1, rdx=0x24C87563628, target=b""))
        self.assertIsNone(value["rdx_ascii"])
        self.assertIsNone(value["entry_target_ascii"])
        self.assertEqual(value["chain_names"], ["holder_return"])

    def test_refuses_other_files(self):
        data = make()
        with self.assertRaises(ValueError):
            pod.parse(data[:-1])
        with self.assertRaises(ValueError):
            pod.parse(b"\0\0\0\0" + data[4:])


if __name__ == "__main__":
    unittest.main()
