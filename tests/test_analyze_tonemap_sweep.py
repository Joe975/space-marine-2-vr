import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('tone', Path(__file__).resolve().parents[1] / 'scripts/analyze_tonemap_sweep.py')
tone = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tone)


class TonemapEvidenceTests(unittest.TestCase):
    def test_completed_writer_and_timestamp_contract(self):
        gpu = dict(fence_completed=True, simulation_frame_verified=False, frequency=1000, serial=1,
                   resource=123, queue=456, commands=789, generation=3, writes=1, lists=7,
                   gpu_begin=100, gpu_end=102, gpu_ms=2, completion_upper_ms=3)
        self.assertTrue(tone.valid_gpu(gpu))
        for key, value in [('fence_completed', False), ('frequency', 0), ('writes', 0), ('resource', 0),
                           ('gpu_end', 99), ('gpu_ms', 3), ('gpu_ms', float('nan')),
                           ('completion_upper_ms', -1), ('completion_upper_ms', float('inf')),
                           ('simulation_frame_verified', True)]:
            changed = copy.deepcopy(gpu)
            changed[key] = value
            self.assertFalse(tone.valid_gpu(changed), (key, value))
        del gpu['fence_completed']
        self.assertFalse(tone.valid_gpu(gpu))

    def test_both_repeated_modes_must_really_apply(self):
        self.assertTrue(tone.alternating_counts([0, 120, 512, 632]))
        for counts in ([0, 0, 0, 0], [0, 120, 512, 512], [1, 120, 512, 632], [0, 120, 110, 632], [0, 120]):
            self.assertFalse(tone.alternating_counts(counts))


if __name__ == '__main__':
    unittest.main()
