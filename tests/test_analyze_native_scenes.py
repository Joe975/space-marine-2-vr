import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('native_scenes', Path(__file__).resolve().parents[1] / 'scripts/analyze_native_scenes.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def control():
    native_list, revision, sequence = 0x100000, 7, 12
    views = [dict(slot=slot, flags=flags, world=[float(i % 5 == 0) for i in range(16)])
             for slot, flags in ((0, 0x3f7c013), (1, 0xe50043))]
    scenes = []
    # The actual engine draws the PIP branch before the ordinary branch.
    for ordinal, slot in enumerate((1, 0), 1):
        flags = views[slot]['flags']
        scenes.append(dict(ordinal=ordinal, dispatch_sequence=sequence, flags=0x10, slot=slot,
                           renderer=0x300000, list=native_list, entry=native_list + 0x168 + slot * 0xa60,
                           active_entry_after=native_list + 0x168 + slot * 0xa60,
                           output_wrapper_before=0x200000 + slot * 0x1000, output_wrapper_after=0x200000 + slot * 0x1000,
                           render_counter_before=15, render_counter_after=15, view_flags_before=flags,
                           view_flags_after=flags, valid=True, refusal=0,
                           camera_revision_before=revision, camera_revision_after=revision))
    return dict(native_dispatch=dict(sequence=sequence, list=native_list, renderer=0x300000, cpu_only=True,
                                     scene_observer=True, scene_calls=2, scene_refused=0, scene_dropped=0,
                                     scenes=scenes, camera_revision=revision, before=views, after=copy.deepcopy(views),
                                     camera_list_receipt=dict(revision=revision, render_list=native_list,
                                                              cpu_producing_frame_verified=True, render_id=3,
                                                              main_slot=0, pip_slot=1, main_entry=native_list + 0x168,
                                                              pip_entry=native_list + 0x168 + 0xa60)))


class SceneAuditTests(unittest.TestCase):
    @staticmethod
    def outputs():
        capture = control()
        for scene in capture['native_dispatch']['scenes']:
            raw = 0x400000 + scene['slot'] * 0x1000
            view = dict(resource=raw, identity=raw + 8, handle=raw + 16, known=True, created=10, bound=15,
                        command_list=0x600000, width=1280, height=720, storage=28, format=28, dimension=4,
                        resource_dimension=3, flags=1, depth_or_array=2, mips=3, samples=1, planes=1, subresources=6)
            scene['resource_before'] = dict(resource=raw, status=5, lease_verified=False, epoch=1, serial=20, rtvs=[view])
            scene['resource_after'] = copy.deepcopy(scene['resource_before'])
            scene['resource_after']['serial'] = 30
            scene['resource_after']['rtvs'][0]['bound'] = 25
        return capture

    def test_output_correlation_requires_fresh_native_binding_but_never_a_lease(self):
        result = audit.analyze(self.outputs())
        self.assertTrue(result['cpu_scene_scope_pass'])
        self.assertTrue(result['output_mapping_pass'], result)
        self.assertIs(result['output_lease_verified'], False)
        self.assertIs(result['final_writer_verified'], False)
        self.assertFalse(audit.analyze(control())['output_mapping_pass'])

    def test_output_corruption_does_not_weaken_existing_cpu_scope(self):
        mutations = (('resource', 0), ('resource', True), ('status', 4), ('status', True), ('epoch', 2), ('epoch', True),
                     ('serial', 19), ('lease_verified', True), ('rtvs', []), ('rtvs', None))
        for key, value in mutations:
            with self.subTest(key=key, value=value):
                capture = self.outputs()
                capture['native_dispatch']['scenes'][0]['resource_after'][key] = value
                result = audit.analyze(capture)
                self.assertTrue(result['cpu_scene_scope_pass'])
                self.assertFalse(result['output_mapping_pass'])

    def test_each_output_descriptor_field_and_binding_clock_is_checked(self):
        mutations = (('resource', 123), ('identity', 123), ('handle', 0), ('known', False), ('created', 31),
                     ('width', 1), ('height', 1), ('storage', 1), ('format', 0), ('dimension', True),
                     ('resource_dimension', 4), ('flags', True), ('depth_or_array', 1), ('mips', 1), ('samples', 2),
                     ('planes', 2), ('subresources', 1), ('bound', 20), ('bound', 31), ('command_list', 0))
        for key, value in mutations:
            with self.subTest(key=key):
                capture = self.outputs()
                capture['native_dispatch']['scenes'][0]['resource_after']['rtvs'][0][key] = value
                self.assertFalse(audit.analyze(capture)['output_mapping_pass'])

    def test_aliasing_and_duplicate_descriptors_refuse_output_mapping(self):
        capture = self.outputs()
        scenes = capture['native_dispatch']['scenes']
        scenes[1]['resource_before'] = copy.deepcopy(scenes[0]['resource_before'])
        scenes[1]['resource_after'] = copy.deepcopy(scenes[0]['resource_after'])
        self.assertFalse(audit.analyze(capture)['output_mapping_pass'])
        capture = self.outputs()
        rtvs = capture['native_dispatch']['scenes'][0]['resource_after']['rtvs']
        rtvs.append(copy.deepcopy(rtvs[0]))
        self.assertFalse(audit.analyze(capture)['output_mapping_pass'])

    def test_actual_branch_order_and_cpu_only_limit(self):
        result = audit.analyze(control())
        self.assertTrue(result['cpu_scene_scope_pass'], result)
        self.assertEqual(result['ordinary_slots'], [0])
        self.assertEqual(result['pip_slots'], [1])
        for name in ('gpu_pair_verified', 'simulation_snapshot_verified', 'runtime_submission'):
            self.assertIs(result[name], False)

    def test_missing_or_malformed_scope_refuses(self):
        mutations = (
            ('scene_observer', False), ('scene_calls', 3), ('scene_calls', True),
            ('scene_refused', 1), ('scene_dropped', 1), ('camera_revision', None),
            ('cpu_only', False), ('list', 0), ('renderer', 0), ('scenes', []),
            ('before', []), ('after', []), ('camera_list_receipt', None))
        for key, value in mutations:
            with self.subTest(key=key, value=value):
                capture = control()
                capture['native_dispatch'][key] = value
                self.assertFalse(audit.analyze(capture)['cpu_scene_scope_pass'])

    def test_each_observed_receipt_field_is_checked(self):
        mutations = (('ordinal', 2), ('dispatch_sequence', 11), ('renderer', 0x999), ('list', 0x400000), ('entry', 0x999),
                     ('active_entry_after', 0x999), ('slot', 64), ('valid', False), ('refusal', 1),
                     ('flags', -1), ('render_counter_after', 16), ('render_counter_before', True),
                     ('camera_revision_before', 8), ('camera_revision_after', 8),
                     ('view_flags_before', 0x40), ('view_flags_after', 0x40),
                     ('output_wrapper_before', 0), ('output_wrapper_after', 0))
        for key, value in mutations:
            with self.subTest(key=key):
                capture = control()
                capture['native_dispatch']['scenes'][0][key] = value
                self.assertFalse(audit.analyze(capture)['cpu_scene_scope_pass'])

    def test_changed_world_even_if_native_valid_is_claimed(self):
        capture = control()
        capture['native_dispatch']['after'][0]['world'][12] += .001
        self.assertFalse(audit.analyze(capture)['cpu_scene_scope_pass'])

    def test_consistent_per_scene_but_different_render_counter_refuses(self):
        capture = control()
        scene = capture['native_dispatch']['scenes'][1]
        scene['render_counter_before'] = scene['render_counter_after'] = 16
        self.assertFalse(audit.analyze(capture)['cpu_scene_scope_pass'])

    def test_duplicate_and_missing_scenes_refuse(self):
        capture = control()
        capture['native_dispatch']['scenes'][1] = copy.deepcopy(capture['native_dispatch']['scenes'][0])
        capture['native_dispatch']['scenes'][1]['ordinal'] = 2
        self.assertFalse(audit.analyze(capture)['cpu_scene_scope_pass'])
        capture = control()
        capture['native_dispatch']['scene_calls'] = 1
        capture['native_dispatch']['scenes'].pop()
        self.assertFalse(audit.analyze(capture)['cpu_scene_scope_pass'])


if __name__ == '__main__':
    unittest.main()
