import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest
from test_analyze_ordinary_boundaries import control as cpu_control

spec = importlib.util.spec_from_file_location('ordinary_inline', Path(__file__).resolve().parents[1]/'scripts/analyze_ordinary_inline.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def control():
    return dict(model='ordinary_inline_copy_v1', pip_enabled=False, gpu_pair_verified=False, runtime_submission=False,
                cpu_boundary=cpu_control(), readback_result=0, pixels_saved=True,
                copy=dict(tag=12, generation=8, writes=2, submission=20, fence_value=1, source=0x700000, identity=0x700008,
                          commands=0x400000, queue=0x600000, thread=1, appended=True, submitted=True, completed=True,
                          refusal=0, hazards=0, width=3840, height=2160, format=28, source_flags=1, source_state=4,
                          heap_known=True, heap_flags=0x8c, heap_type=1, heap_cpu=0, heap_pool=0, creation_node=1, visible_node=1,
                          dimension=3, array=1, mips=1, samples=1, quality=0, layout=0, alignment=0,
                          private_display=True, excluded_aliases=2, excluded_uavs=1))


PIXELS = dict(width=3840, height=2160, bytes=3840*2160*3)


class InlineCopyAuditTests(unittest.TestCase):
    def test_completed_control_is_not_stereo_or_colour_parity(self):
        result = audit.analyze(control(), PIXELS)
        self.assertTrue(result['ordinary_preoverwrite_copy_pass'], result)
        for name in ('gpu_pair_verified', 'simulation_snapshot_verified', 'all_final_writers_verified', 'colour_parity_verified', 'runtime_submission'):
            self.assertIs(result[name], False)

    def test_each_recording_source_state_and_fence_field(self):
        mutations = [(name, 0) for name in ('tag', 'generation', 'writes', 'submission', 'fence_value', 'source', 'identity', 'commands', 'queue', 'thread')]
        mutations += [(name, False) for name in ('appended', 'submitted', 'completed')]
        mutations += [('tag', 11), ('refusal', True), ('refusal', 1), ('hazards', 1), ('source_state', True), ('source_state', 8),
                      ('source_flags', 5), ('source_flags', 33), ('source_flags', 0), ('format', 27), ('width', 960), ('height', 540),
                      ('heap_known', False), ('heap_flags', 0x8d), ('heap_flags', 0x84), ('heap_type', 2), ('heap_cpu', 1),
                      ('heap_pool', 1), ('creation_node', 2), ('visible_node', 2), ('private_display', False), ('excluded_aliases', -1)]
        mutations += [('mips', 2), ('array', 2), ('samples', 2), ('quality', 1), ('layout', 1), ('alignment', 4194304), ('heap_type', True)]
        for name, value in mutations:
            with self.subTest(name=name, value=value):
                capture = control()
                capture['copy'][name] = value
                self.assertFalse(audit.analyze(capture, PIXELS)['ordinary_preoverwrite_copy_pass'])

    def test_cpu_receipt_and_actual_image_cannot_be_substituted(self):
        for name, value in (('gpu_pair_verified', True), ('runtime_submission', True), ('pip_enabled', True), ('cpu_boundary', None),
                            ('readback_result', True), ('readback_result', -1), ('pixels_saved', False)):
            capture = control()
            capture[name] = value
            self.assertFalse(audit.analyze(capture, PIXELS)['ordinary_preoverwrite_copy_pass'])
        for name, value in (('width', 960), ('height', 540), ('bytes', 1)):
            pixels = copy.copy(PIXELS)
            pixels[name] = value
            self.assertFalse(audit.analyze(control(), pixels)['ordinary_preoverwrite_copy_pass'])
        capture = control()
        capture['cpu_boundary']['events'][2]['revision'] += 1
        self.assertFalse(audit.analyze(capture, PIXELS)['ordinary_preoverwrite_copy_pass'])

    def test_native_ppm_bytes_are_checked(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'image.ppm'
            path.write_bytes(b'P6\n2 1\n255\n'+bytes(6))
            self.assertEqual(audit.ppm(path), dict(width=2, height=1, bytes=6))
            path.write_bytes(b'P3\n2 1\n255\n'+bytes(6))
            with self.assertRaises(ValueError):
                audit.ppm(path)

    def test_native_refusal_survives_the_expected_missing_image(self):
        capture = control()
        capture['copy'].update(refusal=8, hazards=65, appended=False, submitted=False, completed=False, submission=0, fence_value=0)
        capture['pixels_saved'] = False
        result = audit.analyze(capture, None)
        self.assertFalse(result['ordinary_preoverwrite_copy_pass'])
        self.assertIn('reason=8 hazards=65', result['errors'][0])


if __name__ == '__main__':
    unittest.main()
