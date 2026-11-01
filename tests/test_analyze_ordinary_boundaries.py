import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('ordinary_boundaries', Path(__file__).resolve().parents[1]/'scripts/analyze_ordinary_boundaries.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def control():
    native_list, entry, flags = 0x100000, 0x100168, 0x7f7c013
    primary = dict(add_list=native_list+0x160, render_list=native_list, entry=entry, slot=0, revision=7, render_id=8, token=0)
    view = dict(slot=0, flags=flags, world=[float(i % 5 == 0) for i in range(16)])
    scene = dict(ordinal=1, slot=0, entry=entry, active_entry_after=entry, flags=flags, flags_after=flags,
                 refusal=0, counter_before=42, counter_after=42, revision_before=7, revision_after=7)
    events = []
    for stage in range(3):
        rtv = dict(resource=0x300000, identity=0x300008, handle=10, known=True, created=2, bound=15, command_list=0x400000,
                   width=3840, height=2160, storage=27, format=28, dimension=4, flags=1, subresources=1)
        events.append(dict(serial=stage+1, stage=stage, dispatch=12, ordinal=1, slot=0, entry=entry, render_id=8, token=0,
                           revision=7, wrapper=0x200000, resource=0x300000, status=5, scene_begin_epoch=1, scene_begin_serial=10,
                           epoch=1, serial_at_capture=20, lease_verified=False, final_writer_verified=False, rtvs=[rtv]))
    return dict(model='ordinary_colour_boundaries_v1', cpu_only=True, pip_enabled=False, gpu_pair_verified=False,
                runtime_submission=False, display_width=3840, display_height=2160, dispatch=12, list=native_list,
                renderer=0x500000, scene_calls=1, scene_refused=0, scene_dropped=0, refused=0, errors=0,
                ring_evictions=100, primary=primary, scenes=[scene], views=[view], views_after=[copy.deepcopy(view)], events=events)


class OrdinaryBoundaryTests(unittest.TestCase):
    def test_three_actual_cpu_boundaries_with_no_gpu_claim(self):
        result = audit.analyze(control())
        self.assertTrue(result['ordinary_cpu_boundary_pass'], result)
        for name in ('gpu_pair_verified', 'output_lease_verified', 'final_writer_verified', 'simulation_snapshot_verified', 'runtime_submission'):
            self.assertIs(result[name], False)

    def test_mode_scope_and_refusals(self):
        for name, value in (('model', 'pip'), ('pip_enabled', True), ('cpu_only', False), ('gpu_pair_verified', True),
                            ('runtime_submission', True), ('scene_calls', 2), ('scene_refused', 1), ('scene_dropped', True),
                            ('refused', 1), ('errors', 1), ('primary', None), ('events', []), ('display_width', 0)):
            with self.subTest(name=name):
                capture = control()
                capture[name] = value
                self.assertFalse(audit.analyze(capture)['ordinary_cpu_boundary_pass'])

    def test_every_primary_receipt_and_scene_field(self):
        mutations = dict(primary=(('add_list', 1), ('render_list', 1), ('entry', 1), ('slot', True), ('revision', 8), ('render_id', 9), ('token', True)),
                         scene=(('ordinal', True), ('slot', True), ('entry', 1), ('active_entry_after', 1), ('flags', 0x20040), ('flags_after', 1),
                                ('refusal', 1), ('counter_before', True), ('counter_after', 41), ('revision_before', 8), ('revision_after', None)))
        for target, changes in mutations.items():
            for name, value in changes:
                with self.subTest(target=target, name=name):
                    capture = control()
                    (capture['primary'] if target == 'primary' else capture['scenes'][0])[name] = value
                    self.assertFalse(audit.analyze(capture)['ordinary_cpu_boundary_pass'])

    def test_each_stage_receipt_and_exact_full_resolution(self):
        changes = (('stage', True), ('dispatch', 11), ('ordinal', 2), ('slot', 1), ('entry', 1), ('render_id', 9), ('token', 1), ('revision', 8),
                   ('wrapper', 1), ('resource', 1), ('status', 4), ('scene_begin_epoch', 2), ('scene_begin_serial', 16),
                   ('epoch', True), ('serial_at_capture', 9), ('lease_verified', True), ('final_writer_verified', True), ('rtvs', []))
        for name, value in changes:
            with self.subTest(name=name):
                capture = control()
                capture['events'][1][name] = value
                self.assertFalse(audit.analyze(capture)['ordinary_cpu_boundary_pass'])
        for name, value in (('identity', 1), ('resource', 1), ('known', False), ('width', 960), ('height', 540), ('created', 21),
                            ('bound', 10), ('bound', 21), ('command_list', 0), ('flags', True), ('subresources', 2), ('format', 0)):
            with self.subTest(descriptor=name):
                capture = control()
                capture['events'][1]['rtvs'][0][name] = value
                self.assertFalse(audit.analyze(capture)['ordinary_cpu_boundary_pass'])

    def test_duplicate_stage_descriptor_and_changed_world(self):
        capture = control()
        capture['events'][1] = copy.deepcopy(capture['events'][0])
        self.assertFalse(audit.analyze(capture)['ordinary_cpu_boundary_pass'])
        capture = control()
        capture['events'][1]['rtvs'] *= 2
        self.assertFalse(audit.analyze(capture)['ordinary_cpu_boundary_pass'])
        capture = control()
        capture['views_after'][0]['world'][12] = .001
        self.assertFalse(audit.analyze(capture)['ordinary_cpu_boundary_pass'])


if __name__ == '__main__':
    unittest.main()
