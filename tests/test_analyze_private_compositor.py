import copy
import importlib.util
from pathlib import Path
import unittest
from test_analyze_ordinary_inline import control as inline_control, PIXELS

spec = importlib.util.spec_from_file_location('private_compositor', Path(__file__).resolve().parents[1]/'scripts/analyze_private_compositor.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def control():
    result = inline_control()
    result.update(model='ordinary_private_compositor_v1', shader_override=False, desktop_mirror_verified=False, retirement='process_lifetime')
    c = result['copy']
    result['redirect'] = dict(tag=c['tag'], generation=c['generation'], commands=c['commands'], target=c['identity'], source=0x800000,
                              format=c['format'], width=c['width'], height=c['height'], bindings=1, restores=1, refusal=0,
                              active=False, restored=True, restore_required=True, private_backing=True, source_flags=1,
                              source_heap_known=True, source_heap_flags=0x8d)
    return result


class PrivateCompositorAuditTests(unittest.TestCase):
    def test_owned_private_output_keeps_native_shared_source_distinct(self):
        result = audit.analyze(control(), PIXELS)
        self.assertTrue(result['ordinary_private_output_pass'], result)
        for name in ('gpu_pair_verified', 'colour_parity_verified', 'histories_verified', 'desktop_mirror_verified',
                     'general_retirement_verified', 'runtime_submission'):
            self.assertIs(result[name], False)

    def test_every_restore_identity_and_safety_field(self):
        changes = [(name, 0) for name in ('tag', 'generation', 'bindings', 'source', 'target', 'commands', 'format', 'width', 'height')]
        changes += [('tag', 13), ('generation', 9), ('commands', 2), ('target', 3), ('format', 29), ('width', 960),
                    ('height', 540), ('active', True), ('restored', False), ('private_backing', False), ('refusal', 1),
                    ('restore_required', 1), ('restores', 0), ('restores', 2), ('source_flags', 0), ('source_heap_known', False)]
        for name, value in changes:
            with self.subTest(name=name, value=value):
                capture = control()
                capture['redirect'][name] = value
                self.assertFalse(audit.analyze(capture, PIXELS)['ordinary_private_output_pass'])
        capture = control()
        capture['redirect']['source'] = capture['redirect']['target']
        self.assertFalse(audit.analyze(capture, PIXELS)['ordinary_private_output_pass'])

    def test_native_already_restored_binding_needs_no_extra_restore(self):
        capture = control()
        capture['redirect'].update(restore_required=False, restores=0)
        self.assertTrue(audit.analyze(capture, PIXELS)['ordinary_private_output_pass'])

    def test_no_borrowed_mode_completion_or_private_backing_claim(self):
        for name, value in (('model', 'pip'), ('shader_override', True), ('desktop_mirror_verified', True),
                            ('retirement', 'retired'), ('cpu_boundary', None), ('pixels_saved', False)):
            capture = control()
            capture[name] = value
            self.assertFalse(audit.analyze(capture, PIXELS)['ordinary_private_output_pass'])
        capture = control()
        capture['copy']['heap_flags'] = 0x8d
        self.assertFalse(audit.analyze(capture, PIXELS)['ordinary_private_output_pass'])
        capture = control()
        capture['copy']['completed'] = False
        self.assertFalse(audit.analyze(capture, PIXELS)['ordinary_private_output_pass'])


if __name__ == '__main__':
    unittest.main()
