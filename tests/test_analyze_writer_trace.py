import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('writer', Path(__file__).resolve().parents[1] / 'scripts/analyze_writer_trace.py')
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)


class WriterEvidenceTests(unittest.TestCase):
    def test_foreign_queue_completion_requires_actual_receipts_and_full_accounting(self):
        q = dict(device_known=True, early_device_return=True, primary_known=True, all_observed_methods_match=True,
                 device_canonical=1, primary_canonical=2, primary_execute=3, primary_serial=4, method_count=1,
                 primary_type=0, creation_hook_mask=3, device9=True, failed=0, observer_errors=0, close_observer_errors=0,
                 unknown_foreign=0, foreign_target_writes=0, untracked_target_writes=0, foreign_fence_failures=0,
                 foreign_unknown_lists=2, foreign_precreation_copies=0, foreign_fenced_lists=2, foreign_fenced_submissions=1,
                 foreign_completions=[dict(submission=20, lists=2, signaled=1, completed=1, queue_canonical=5, device=1, wait_ms=.1)])
        c = dict(queue_observation=q, gpu_observed_batch=dict(submission_sequence=21))
        self.assertEqual(writer.queue_errors(c), [])
        for key, value in [('foreign_fence_failures', 1), ('foreign_fenced_lists', 1), ('foreign_unknown_lists', 3),
                           ('all_observed_methods_match', False), ('observer_errors', 1), ('foreign_target_writes', 1)]:
            bad = copy.deepcopy(c);bad['queue_observation'][key] = value
            self.assertTrue(writer.queue_errors(bad), (key, value))
        for key, value in [('submission', 21), ('completed', 0), ('completed', 2**64-1), ('device', 9),
                           ('queue_canonical', 2), ('lists', 1), ('wait_ms', float('nan'))]:
            bad = copy.deepcopy(c);bad['queue_observation']['foreign_completions'][0][key] = value
            self.assertTrue(writer.queue_errors(bad), (key, value))
        bad = copy.deepcopy(c);bad['queue_observation']['foreign_completions'] = []
        self.assertTrue(writer.queue_errors(bad))

    @staticmethod
    def access_receipt():
        return dict(model='private_committed_gpu_rt_v3', aliases=0, uavs=0, indirect_compute=0, indirect_raster=0, writers=[0]*5, extended=[0]*8, allocation=None)

    @staticmethod
    def binding_receipt():
        return dict(binding_capabilities_known=True, list4_supported=True, binding_hook_mask=7,
                    signature_hook_installed=True, recording_binding_coverage_verified=True,
                    command_object_receipts_verified=True, base_implementations_verified=True,
                    extended_capabilities_known=True, list1_supported=True, list6_supported=True, extended_hook_mask=255)

    def test_access_exclusions_require_actual_private_gpu_allocation(self):
        value = self.access_receipt()
        capture = dict(readback_boundary=dict(access_exclusions=value, resource=123), gpu_observed_batch=dict(resource=123),
                       pip_resource_desc=dict(flags=1, dimension=3, mip_levels=1, depth_or_array_size=1, sample_count=1, sample_quality=0, layout=0))
        self.assertEqual(writer.access_errors(capture), [])
        value['aliases'] = 1
        self.assertTrue(writer.access_errors(capture))
        value['allocation'] = dict(api=2, kind=1, serial=1, device=2, resource=99, resource_interface=123,
            heap=0, heap_serial=0, offset=0, size=65536, heap_size=65536, alignment=65536, heap_flags=0,
            actual_heap_flags=0, properties_known=True, heap_type=1, cpu_page=0, memory_pool=0, creation_node=1, visible_node=1)
        value['uavs'] = 2
        value['writers'] = [3, 0, 0, 4, 5]
        value['allocation']['actual_heap_flags'] = 0x84
        self.assertEqual(writer.access_errors(capture), [])
        value['writers'][1] = 3
        value['indirect_compute'] = 1
        value['indirect_raster'] = 2
        value['extended'] = [1, 2, 3, 0, 0, 4, 5, 6]
        self.assertEqual(writer.access_errors(capture), [])
        for key, new in [('api', 1), ('kind', 2), ('serial', 0), ('device', 0), ('resource', 0), ('resource_interface', 124),
                         ('heap', 1), ('heap_serial', 1), ('offset', 1), ('size', 0), ('size', 2**64-1),
                         ('heap_size', 131072), ('alignment', 0), ('alignment', 3), ('alignment', 131072),
                         ('heap_flags', 1), ('actual_heap_flags', 32), ('properties_known', False),
                         ('heap_type', 4), ('cpu_page', 3), ('memory_pool', 1), ('creation_node', 0), ('visible_node', 2)]:
            changed = copy.deepcopy(capture)
            changed['readback_boundary']['access_exclusions']['allocation'][key] = new
            self.assertTrue(writer.access_errors(changed), (key, new))
        for key in value['allocation']:
            changed = copy.deepcopy(capture)
            changed['readback_boundary']['access_exclusions']['allocation'][key] = True
            if key != 'properties_known':
                self.assertTrue(writer.access_errors(changed), key)
            del changed['readback_boundary']['access_exclusions']['allocation'][key]
            self.assertTrue(writer.access_errors(changed), key)
        for key, new in [('aliases', True), ('uavs', -1), ('writers', [0, 1, 0, 0, 0]), ('writers', [0, 0, 1, 0, 0]),
                         ('writers', [0]*4), ('model', 'assumed'), ('indirect_compute', True), ('indirect_raster', -1), ('indirect_raster', 0),
                         ('extended', [0]*7), ('extended', [0,0,0,1,0,0,0,0]), ('extended', [0,0,0,0,1,0,0,0]), ('extended', [True]+[0]*7)]:
            changed = copy.deepcopy(capture)
            changed['readback_boundary']['access_exclusions'][key] = new
            self.assertTrue(writer.access_errors(changed), (key, new))
        for key, new in [('flags', 5), ('dimension', 1), ('mip_levels', 2), ('depth_or_array_size', 2), ('sample_count', 2), ('sample_quality', 1), ('layout', 1)]:
            changed = copy.deepcopy(capture)
            changed['pip_resource_desc'][key] = new
            self.assertTrue(writer.access_errors(changed), (key, new))

    @staticmethod
    def native_pool():
        return dict(model='present_slot_observation_v1', context=1000, manager=2000, records=3000, record_index=0,
                    camera=4000, pool=5000, slots=6000, slot=6000, texture_index=0, texture=7000, resource=123,
                    width=1280, height=720, record_recycle=0, record_deferred_release=0, pool_release_marked=0,
                    owner_lock_held=True, writer_boundary_lease_verified=False, gpu_retirement_verified=False)

    def test_native_pool_observation_rejects_markers_identity_changes_and_lifetime_claims(self):
        capture = dict(pip_native_pool=self.native_pool(), pip_record_index=0, gpu_observed_batch=dict(resource=123),
                       pip_resource_desc=dict(width=1280, height=720))
        self.assertEqual(writer.pool_errors(capture), [])
        cases = [('model', 'assumed'), ('record_index', True), ('record_index', -1), ('record_index', 1),
                 ('texture_index', True), ('texture_index', -1), ('texture_index', 1024), ('slot', 6001),
                 ('resource', 124), ('width', 1279), ('height', True), ('record_recycle', 1),
                 ('record_deferred_release', 1), ('pool_release_marked', True), ('pool_release_marked', 1),
                 ('owner_lock_held', False), ('writer_boundary_lease_verified', True), ('gpu_retirement_verified', True)]
        for key in ('context', 'manager', 'records', 'camera', 'pool', 'slots', 'slot', 'texture', 'resource'):
            cases += [(key, 0), (key, True), (key, 2**64)]
        for key, value in cases:
            changed = copy.deepcopy(capture)
            changed['pip_native_pool'][key] = value
            self.assertTrue(writer.pool_errors(changed), (key, value))
        for key in capture['pip_native_pool']:
            changed = copy.deepcopy(capture)
            del changed['pip_native_pool'][key]
            self.assertTrue(writer.pool_errors(changed), key)
        self.assertTrue(writer.pool_errors({}))

    def test_barrier_receipt_requires_capability_evidence_and_no_unsupported_hazards(self):
        capture = dict(gpu_observed_batch=dict(barrier_hazards=0, writer_counts=dict(indirect_possible=0)), readback_boundary=dict(barrier_observation=dict(
            model='legacy_with_recording_identity_v6', capabilities_known=True, enhanced_supported=True,
            enhanced_hook_installed=True, base_writer_hook_mask=31, hazards=0)))
        capture['readback_boundary']['barrier_observation'].update(self.binding_receipt())
        capture['readback_boundary']['access_exclusions'] = self.access_receipt()
        self.assertEqual(writer.barrier_errors(capture), [])
        legacy = copy.deepcopy(capture)
        legacy['readback_boundary']['barrier_observation'].update(enhanced_supported=False, enhanced_hook_installed=False)
        self.assertEqual(writer.barrier_errors(legacy), [])
        for key, value in [('model', 'assumed'), ('capabilities_known', False), ('enhanced_supported', 1),
                           ('enhanced_hook_installed', False), ('base_writer_hook_mask', True), ('base_writer_hook_mask', 0),
                           ('base_writer_hook_mask', 30), ('base_writer_hook_mask', 63), ('model', 'legacy_with_sticky_unsupported_v1'),
                           ('binding_capabilities_known', False), ('list4_supported', 1), ('binding_hook_mask', True),
                           ('binding_hook_mask', 1), ('binding_hook_mask', 3), ('signature_hook_installed', False),
                           ('recording_binding_coverage_verified', False), ('model', 'legacy_with_private_access_v3'),
                           ('command_object_receipts_verified', False), ('base_implementations_verified', False),
                           ('command_object_receipts_verified', 1), ('base_implementations_verified', 1), ('model', 'legacy_with_extended_access_v5'),
                           ('extended_capabilities_known', False), ('list1_supported', 1), ('list6_supported', 0),
                           ('extended_hook_mask', True), ('extended_hook_mask', 254), ('model', 'legacy_with_indirect_access_v4'),
                           ('hazards', True), ('hazards', 1), ('hazards', 2), ('hazards', 4), ('hazards', 8), ('hazards', 16)]:
            changed = copy.deepcopy(capture)
            changed['readback_boundary']['barrier_observation'][key] = value
            self.assertTrue(writer.barrier_errors(changed), key)
        capture['gpu_observed_batch']['barrier_hazards'] = 2
        self.assertTrue(writer.barrier_errors(capture))
        self.assertTrue(writer.barrier_errors({}))
        for mask in range(31):
            changed = copy.deepcopy(legacy)
            changed['readback_boundary']['barrier_observation']['base_writer_hook_mask'] = mask
            self.assertTrue(writer.barrier_errors(changed), mask)
        del legacy['readback_boundary']['barrier_observation']['base_writer_hook_mask']
        self.assertTrue(writer.barrier_errors(legacy))

    def test_actual_descriptor_independently_constrains_state_and_decay(self):
        capture = dict(pip_resource_desc=dict(dimension=3, width=1280, height=720, depth_or_array_size=1,
            mip_levels=1, format=28, sample_count=1, sample_quality=0, layout=0, flags=5, plane_count=1, subresource_count=1),
            readback_boundary=dict(state_from_raster=True, common_state_decay=False))
        self.assertEqual(writer.resource_errors(capture), [])
        for key, value in [('flags', 37), ('flags', 0), ('flags', 3), ('sample_count', 2), ('sample_quality', 1),
                           ('mip_levels', 2), ('depth_or_array_size', 2), ('plane_count', 2), ('subresource_count', 2),
                           ('width', 3840), ('height', 2160), ('format', 40), ('dimension', 2), ('layout', 1), ('flags', True)]:
            changed = copy.deepcopy(capture)
            changed['pip_resource_desc'][key] = value
            self.assertTrue(writer.resource_errors(changed), key)
        for fmt in (10, 27, 28, 29, 87, 90, 91):
            capture['pip_resource_desc']['format'] = fmt
            self.assertEqual(writer.resource_errors(capture), [])
        capture['pip_resource_desc']['flags'] |= 32
        capture['readback_boundary']['common_state_decay'] = True
        self.assertEqual(writer.resource_errors(capture), [])
        del capture['pip_resource_desc']['plane_count']
        self.assertTrue(writer.resource_errors(capture))

    def test_refused_run_reports_all_missing_phases_without_passing_any_gate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'phase_2').mkdir()
            (root / 'phase_2/capture.json').write_text('{truncated')
            result = writer.analyze(root, require_boundary=True)
            self.assertEqual([p['phase'] for p in result['phases']], [0, 1, 2, 3])
            self.assertTrue(all(p['errors'] for p in result['phases']))
            for key in ('metadata_pass', 'observed_raster_gate_pass', 'observed_writer_origin_gate_pass',
                        'readback_queue_boundary_gate_pass', 'readback_source_version_verified',
                        'final_writer_verified', 'simulation_frame_verified'):
                self.assertIs(result[key], False, key)

    def test_guarded_readback_requires_adjacent_actual_submissions_and_its_own_fence(self):
        capture = dict(readback_mode='pip_at_writer_batch_main_at_present', paired_readback=False,
            gpu_observed_batch=dict(serial=7, submission_sequence=30, queue=42, resource=101, lists=7,
                                   observed_target_lists=1, all_lists_tracked=True),
            readback_boundary=dict(writer_serial=7, writer_submission=30, copy_submission=31, executions=1,
                queue=42, resource=101, commands=300, fence_completed=True, retries=2,
                copy_source_state=192, copy_state_source='observed_raster_and_legacy_transitions', state_from_raster=False, common_state_decay=False,
                observed_queue_boundary_verified=True, all_writer_coverage_verified=False, source_version_verified=False))
        self.assertEqual(writer.boundary_errors(capture), [])
        decayed = copy.deepcopy(capture)
        decayed['readback_boundary'].update(copy_source_state=0, state_from_raster=True, common_state_decay=True)
        self.assertEqual(writer.boundary_errors(decayed), [])
        changed = copy.deepcopy(capture)
        changed['gpu_observed_batch']['observed_target_lists'] = 2
        self.assertTrue(writer.boundary_errors(changed))
        changed = copy.deepcopy(capture)
        changed['gpu_observed_batch']['all_lists_tracked'] = False
        self.assertTrue(writer.boundary_errors(changed))
        for key, value in [('writer_serial', 6), ('writer_submission', 29), ('copy_submission', 32),
                           ('executions', 2), ('queue', 43), ('resource', 102), ('commands', 0),
                           ('fence_completed', False), ('retries', -1), ('executions', True),
                           ('observed_queue_boundary_verified', False), ('all_writer_coverage_verified', True),
                           ('source_version_verified', True), ('copy_source_state', 16), ('copy_state_source', 'assumed'),
                           ('common_state_decay', True), ('state_from_raster', 'assumed'), ('state_from_raster', True)]:
            changed = copy.deepcopy(capture)
            changed['readback_boundary'][key] = value
            self.assertTrue(writer.boundary_errors(changed), key)
        del capture['readback_boundary']['fence_completed']
        self.assertTrue(writer.boundary_errors(capture))

    def test_four_phases_require_distinct_advancing_completed_sources(self):
        world = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 10, 20, 30, 1]
        gpu = dict(fence_completed=True, simulation_frame_verified=False, frequency=1000, serial=1,
                   resource=123, queue=456, commands=789, generation=3, writes=1, lists=7,
                   gpu_begin=100, gpu_end=102, gpu_ms=2, completion_upper_ms=3,
                   writer_counts=dict(draw=1, indexed=0, clear=0, copy=0, copy_texture=0, resolve=0, indirect_possible=0),
                   last_write_callsite=1234, execute_callsite=5678, last_write_thread=42,
                   last_write_rva=1234, execute_rva=5678)
        with tempfile.TemporaryDirectory() as temporary, patch.object(writer.tone.lens, 'geometry_errors', return_value=[]):
            root = Path(temporary)
            def write(ids, boundary=False):
                for phase, source_id in enumerate(ids):
                    folder = root / f'phase_{phase}'
                    folder.mkdir(exist_ok=True)
                    batch = dict(gpu, serial=phase+1, observed_writer_origin=dict(source_render_id=source_id,
                                 source_token=0, record=0, unique_recent_pose=True, unique_over_experiment=True,
                                 source_world=world, render_world=world))
                    capture = dict(gpu_observed_batch=batch, pip_record_index=0, pose_tags_enabled=True,
                                   lens_phase=phase, scene_tonemap=False)
                    if boundary:
                        capture['pip_native_pool'] = self.native_pool()
                        batch['barrier_hazards'] = 0
                        capture['barrier_stub'] = dict(model='legacy_with_recording_identity_v6', capabilities_known=True,
                            enhanced_supported=True, enhanced_hook_installed=True, base_writer_hook_mask=31, hazards=0)
                        capture['barrier_stub'].update(self.binding_receipt())
                        capture['pip_resource_desc'] = dict(dimension=3, width=1280, height=720, depth_or_array_size=1,
                            mip_levels=1, format=28, sample_count=1, sample_quality=0, layout=0, flags=5, plane_count=1, subresource_count=1)
                        batch['lists'] = 1
                        batch['observed_target_lists'] = 1
                        batch['all_lists_tracked'] = True
                        capture['readback_mode'] = 'pip_at_writer_batch_main_at_present'
                        capture['paired_readback'] = False
                        batch['submission_sequence'] = phase * 10 + 1
                        capture['readback_boundary'] = dict(writer_serial=batch['serial'],
                            writer_submission=batch['submission_sequence'], copy_submission=batch['submission_sequence'] + 1,
                            executions=1, queue=batch['queue'], resource=batch['resource'], commands=999,
                            fence_completed=True, retries=0, observed_queue_boundary_verified=True,
                            copy_source_state=192, copy_state_source='observed_raster_and_legacy_transitions', state_from_raster=False, common_state_decay=False,
                            all_writer_coverage_verified=False, source_version_verified=False)
                        capture['readback_boundary']['barrier_observation'] = capture.pop('barrier_stub')
                        capture['readback_boundary']['access_exclusions'] = self.access_receipt()
                    (folder / 'capture.json').write_text(json.dumps(capture))
            write([10, 20, 30, 40])
            result = writer.analyze(root, require_origin=True)
            self.assertIs(result['observed_writer_origin_gate_pass'], True)
            for phase in range(4):
                path = root / f'phase_{phase}/capture.json'
                capture = json.loads(path.read_text())
                capture['pose_tags_enabled'] = False
                path.write_text(json.dumps(capture))
            self.assertIs(writer.analyze(root, require_origin=True)['observed_writer_origin_gate_pass'], False)
            for phase in range(4):
                path = root / f'phase_{phase}/capture.json'
                capture = json.loads(path.read_text())
                origin = capture['gpu_observed_batch']['observed_writer_origin']
                del origin['unique_recent_pose'], origin['unique_over_experiment']
                origin.update(identity_model='native_copy_slot_v1', camera_revision=phase+1,
                              camera_render_list=0x1000, camera_pip_entry=0x1bc8)
                capture['native_dispatch'] = dict(camera_list_receipt=dict(model='native_add_slot_v1',
                    cpu_producing_frame_verified=True, render_id=origin['source_render_id'], token=0,
                    revision=phase+1, render_list=0x1000, add_list=0x1160, main_slot=0, pip_slot=1,
                    main_entry=0x1168, pip_entry=0x1bc8))
                path.write_text(json.dumps(capture))
            self.assertIs(writer.analyze(root, require_origin=True)['observed_writer_origin_gate_pass'], True)
            path = root / 'phase_1/capture.json'
            capture = json.loads(path.read_text())
            capture['native_dispatch']['camera_list_receipt']['revision'] += 1
            path.write_text(json.dumps(capture))
            self.assertIs(writer.analyze(root, require_origin=True)['observed_writer_origin_gate_pass'], False)
            write([10, 20, 30, 40])
            self.assertIs(writer.analyze(root, require_boundary=True)['readback_queue_boundary_gate_pass'], False)
            write([10, 20, 30, 40], boundary=True)
            result = writer.analyze(root, require_boundary=True)
            self.assertIs(result['readback_queue_boundary_gate_pass'], True)
            self.assertIs(result['resource_descriptor_audit_pass'], False)
            self.assertIs(writer.analyze(root, require_resource_desc=True)['resource_descriptor_audit_pass'], True)
            self.assertIs(writer.analyze(root, require_barrier_coverage=True)['barrier_observation_audit_pass'], True)
            self.assertIs(writer.analyze(root, require_native_pool=True)['native_pool_observation_audit_pass'], True)
            self.assertIs(writer.analyze(root, require_queue=True)['foreign_queue_completion_audit_pass'], False)
            for phase in range(4):
                queue_file = root / f'phase_{phase}/capture.json'
                queue_capture = json.loads(queue_file.read_text())
                queue_capture['queue_observation'] = dict(device_known=True, early_device_return=True, primary_known=True, all_observed_methods_match=True,
                    device_canonical=1, primary_canonical=2, primary_execute=3, primary_serial=4, method_count=1, primary_type=0,
                    creation_hook_mask=3, device9=True, failed=0, observer_errors=0, close_observer_errors=0, unknown_foreign=0,
                    foreign_target_writes=0, untracked_target_writes=0, foreign_fence_failures=0, foreign_unknown_lists=2,
                    foreign_precreation_copies=0, foreign_fenced_lists=2, foreign_fenced_submissions=1,
                    foreign_completions=[dict(submission=1, lists=2, signaled=1, completed=1, queue_canonical=5, device=1, wait_ms=.1)])
                if phase == 0:
                    queue_capture['queue_observation'].update(foreign_unknown_lists=0, foreign_fenced_lists=0, foreign_fenced_submissions=0, foreign_completions=[])
                queue_file.write_text(json.dumps(queue_capture))
            self.assertIs(writer.analyze(root, require_queue=True)['foreign_queue_completion_audit_pass'], True)
            queue_file = root / 'phase_1/capture.json'
            queue_capture = json.loads(queue_file.read_text())
            queue_capture['queue_observation']['foreign_completions'][0]['wait_ms'] = .2
            self.assertEqual(writer.queue_errors(queue_capture), [])
            queue_file.write_text(json.dumps(queue_capture))
            self.assertIs(writer.analyze(root, require_queue=True)['foreign_queue_completion_audit_pass'], False)
            for phase in range(4):
                colour_file = root / f'phase_{phase}/capture.json'
                colour_capture = json.loads(colour_file.read_text())
                colour_capture['scene_tonemap'] = bool(phase % 2)
                colour_file.write_text(json.dumps(colour_capture))
            self.assertIs(writer.analyze(root, require_native_pool=True)['native_pool_observation_audit_pass'], False)
            colour_result = writer.analyze(root, require_native_pool=True, colour_sweep=True)
            self.assertIs(colour_result['native_pool_observation_audit_pass'], True)
            self.assertIs(colour_result['final_writer_verified'], False)
            colour_file = root / 'phase_2/capture.json'
            colour_capture = json.loads(colour_file.read_text())
            colour_capture['scene_tonemap'] = True
            colour_file.write_text(json.dumps(colour_capture))
            self.assertIs(writer.analyze(root, require_native_pool=True, colour_sweep=True)['native_pool_observation_audit_pass'], False)
            write([10, 20, 30, 40], boundary=True)
            file = root / 'phase_1/capture.json'
            changed = json.loads(file.read_text())
            changed_pool = copy.deepcopy(changed)
            changed_pool['pip_native_pool']['manager'] += 1
            self.assertEqual(writer.pool_errors(changed_pool), [])
            file.write_text(json.dumps(changed_pool))
            self.assertIs(writer.analyze(root, require_native_pool=True)['native_pool_observation_audit_pass'], False)
            missing_pool = copy.deepcopy(changed)
            del missing_pool['pip_native_pool']
            file.write_text(json.dumps(missing_pool))
            self.assertIs(writer.analyze(root, require_barrier_coverage=True)['barrier_observation_audit_pass'], True)
            self.assertIs(writer.analyze(root, require_native_pool=True)['native_pool_observation_audit_pass'], False)
            missing_barrier = copy.deepcopy(changed)
            changed_capabilities = copy.deepcopy(changed)
            changed_capabilities['readback_boundary']['barrier_observation'].update(enhanced_supported=False, enhanced_hook_installed=False)
            file.write_text(json.dumps(changed_capabilities))
            self.assertEqual(writer.barrier_errors(changed_capabilities), [])
            self.assertIs(writer.analyze(root, require_barrier_coverage=True)['barrier_observation_audit_pass'], False)
            del missing_barrier['readback_boundary']['barrier_observation']
            file.write_text(json.dumps(missing_barrier))
            self.assertIs(writer.analyze(root, require_resource_desc=True)['resource_descriptor_audit_pass'], True)
            self.assertIs(writer.analyze(root, require_barrier_coverage=True)['barrier_observation_audit_pass'], False)
            del changed['pip_resource_desc']
            file.write_text(json.dumps(changed))
            self.assertIs(writer.analyze(root, require_resource_desc=True)['resource_descriptor_audit_pass'], False)
            self.assertIs(writer.analyze(root, require_barrier_coverage=True)['barrier_observation_audit_pass'], False)
            self.assertIs(writer.analyze(root, require_boundary=True)['readback_queue_boundary_gate_pass'], True)
            changed['gpu_observed_batch']['submission_sequence'] = 1
            changed['readback_boundary']['writer_submission'] = 1
            changed['readback_boundary']['copy_submission'] = 2
            file.write_text(json.dumps(changed))
            self.assertIs(writer.analyze(root, require_boundary=True)['readback_queue_boundary_gate_pass'], False)
            for claim in ('final_writer_verified', 'simulation_frame_verified', 'readback_source_version_verified'):
                self.assertIs(result[claim], False)
            for ids in ([10, 20, 20, 40], [10, 30, 20, 40], [10, 20, None, 40]):
                write(ids)
                self.assertIs(writer.analyze(root, require_origin=True)['observed_writer_origin_gate_pass'], False)
            write([10, 20, 30, 40])
            gpu['writer_counts'] = dict(gpu['writer_counts'], draw=0, clear=1)
            write([10, 20, 30, 40])
            self.assertIs(writer.analyze(root, require_origin=True)['observed_writer_origin_gate_pass'], False)
            gpu['writer_counts'] = dict(gpu['writer_counts'], draw=1, clear=0)
            gpu['last_write_rva'] = 0
            write([10, 20, 30, 40])
            self.assertIs(writer.analyze(root, require_origin=True)['observed_writer_origin_gate_pass'], False)

    def test_unique_source_camera_is_separate_from_readback_version(self):
        world = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 10, 20, 30, 1]
        capture = dict(pip_record_index=0, gpu_observed_batch=dict(observed_writer_origin=dict(
            source_render_id=17, source_token=0, record=0, unique_recent_pose=True, unique_over_experiment=True,
            source_world=world, render_world=list(world))))
        self.assertEqual(writer.origin_errors(capture), [])
        for key, value in [('source_render_id', 0), ('record', 1), ('unique_recent_pose', False), ('unique_over_experiment', False),
                           ('source_token', -1), ('source_world', world[:10])]:
            changed = copy.deepcopy(capture)
            changed['gpu_observed_batch']['observed_writer_origin'][key] = value
            self.assertTrue(writer.origin_errors(changed))
        changed = copy.deepcopy(capture)
        changed['gpu_observed_batch']['observed_writer_origin']['render_world'][12] += .01
        self.assertTrue(writer.origin_errors(changed))
        capture['gpu_observed_batch']['observed_writer_origin'] = None
        self.assertTrue(writer.origin_errors(capture))

    def test_causal_slot_origin_requires_exact_frame_revision_and_physical_addresses(self):
        receipt = dict(model='native_add_slot_v1', cpu_producing_frame_verified=True, render_id=17, token=0,
                       revision=3, render_list=0x1000, add_list=0x1160, main_slot=0, pip_slot=1,
                       main_entry=0x1168, pip_entry=0x1bc8)
        world = [1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,-1.,0.,10.,20.,30.,1.]
        origin = dict(source_render_id=17, source_token=0, record=0, source_world=world, render_world=world,
                      identity_model='native_copy_slot_v1', camera_revision=3, camera_render_list=0x1000, camera_pip_entry=0x1bc8)
        capture = dict(pip_record_index=0, native_dispatch=dict(camera_list_receipt=receipt),
                       gpu_observed_batch=dict(observed_writer_origin=origin))
        self.assertEqual(writer.origin_errors(capture), [])
        for key, value in [('model','assumed'), ('cpu_producing_frame_verified',False), ('render_id',18), ('token',1),
                           ('revision',4), ('render_list',0x2000), ('add_list',0x1161), ('main_slot',1),
                           ('pip_slot',64), ('main_entry',0x1169), ('pip_entry',0x1bc9)]:
            changed = copy.deepcopy(capture)
            changed['native_dispatch']['camera_list_receipt'][key] = value
            self.assertTrue(writer.origin_errors(changed), key)
        for key in origin:
            changed = copy.deepcopy(capture)
            del changed['gpu_observed_batch']['observed_writer_origin'][key]
            self.assertTrue(writer.origin_errors(changed), key)

    def test_classified_completed_batch_has_consistent_origins_and_counts(self):
        gpu = dict(fence_completed=True, simulation_frame_verified=False, frequency=1000, serial=1,
                   resource=123, queue=456, commands=789, generation=3, writes=1, lists=7,
                   gpu_begin=100, gpu_end=102, gpu_ms=2, completion_upper_ms=3,
                   writer_counts=dict(draw=1, indexed=0, clear=0, copy=0, copy_texture=0, resolve=0, indirect_possible=0),
                   last_write_callsite=1234, execute_callsite=5678, last_write_thread=42)
        self.assertEqual(writer.writer_errors(gpu), [])
        for key, value in [('last_write_callsite', 0), ('execute_callsite', 0), ('last_write_thread', 0),
                           ('fence_completed', False), ('writes', 2)]:
            changed = copy.deepcopy(gpu)
            changed[key] = value
            self.assertTrue(writer.writer_errors(changed), key)
        for counts in ({}, dict(gpu['writer_counts'], draw=-1), dict(gpu['writer_counts'], draw=True),
                       dict(gpu['writer_counts'], unknown=0)):
            changed = copy.deepcopy(gpu)
            changed['writer_counts'] = counts
            self.assertTrue(writer.writer_errors(changed))


if __name__ == '__main__':
    unittest.main()
