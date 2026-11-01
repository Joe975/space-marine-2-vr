import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest

ROOT=Path(__file__).resolve().parents[1]
def module(name,file):
    spec=importlib.util.spec_from_file_location(name,ROOT/'scripts'/file);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
oracle=module('native_math_oracle_test','native_camera_math_oracle.py')
export=module('native_math_export_test','export_asymmetric_native_math_fixture.py')
REPORT=json.loads((ROOT/'tests/fixtures/ordinary_asymmetric_native_oracle.json').read_text())

class NativeMathOracleTests(unittest.TestCase):
    def reject(self,mutate):
        report=copy.deepcopy(REPORT);mutate(report)
        with self.assertRaises(ValueError):export.render_fixture(report)
    def test_frozen_fixture_reproduces(self):
        self.assertEqual(export.render_fixture(REPORT),(ROOT/'tests/fixtures/ordinary_asymmetric_native_oracle.h').read_text())
    def test_no_live_game_or_stronger_claims(self):
        for key,value in [('actual_game_run',True),('native_scene_verified',True),('gpu_pair_verified',True),('headset_ready',True),('runtime_submission',True),('native_gpu_histories_verified',True),('synthetic_inputs',False),('main_schedule_modeled',False),('actual_native_code_emulated',False)]:
            with self.subTest(key=key):self.reject(lambda r:r.update({key:value}))
    def test_exact_binary_and_helper_coverage(self):
        self.reject(lambda r:r.update(exe_sha256='0'*64))
        self.reject(lambda r:r['functions']['0x1d75d30'].update(sha256='0'*64))
        self.reject(lambda r:r['visited'].pop())
    def test_native_baseline_is_required(self):
        self.reject(lambda r:r['calibration'].pop())
        self.reject(lambda r:r['calibration'][0].update(pass_=False))
        self.reject(lambda r:r['calibration'][0]['errors'].update({'0x49c':.01}))
        self.reject(lambda r:r['calibration'][0]['errors'].update({'0x49c':float('nan')}))
    def test_native_add_projection_phase_is_required(self):
        self.reject(lambda r:r.pop('cache_add_phase_checks'))
        self.reject(lambda r:r.update(cache_add_phase_checks=34))
    def test_case_history_and_input_words(self):
        self.reject(lambda r:r['cases'][1].update(previous=2))
        self.reject(lambda r:r['cases'][1].update(eye=1))
        self.reject(lambda r:r['cases'][0].update(width=0))
        self.reject(lambda r:r['cases'][0]['expected'].pop('0x9b4'))
        self.reject(lambda r:r['cases'][0]['expected']['0x9b4'].__setitem__(0,0x7f800000))
        def changed_projection(r):
            for target in (r['cases'][0]['projection'],r['cases'][0]['expected']['0x49c']):target[8]+=100
        self.reject(changed_projection)
    def test_lens_overflow_and_invalid_depth(self):
        for fov,near,far in [((0,1,-1,1),.1,100),((-1,1,-1,1),0,100),((-1,1,-1,1),1,1),((-1,1,-1,float('nan')),.1,100),((-3e38,3e38,-1,1),1,100),((-1,1,-1,1),2e38,3e38)]:
            with self.subTest(fov=fov,near=near):
                with self.assertRaises(ValueError):oracle.projection(fov,near,far)
        self.assertTrue(all(map(math.isfinite,oracle.projection((-1,1,-1,1),1e38,2e38))))
    def test_emulation_entry_and_instruction_allowlist(self):
        sandbox=oracle.Sandbox.__new__(oracle.Sandbox)
        with self.assertRaises(ValueError):sandbox.call(0x1234)
        with self.assertRaises(ValueError):sandbox.call(0x1d75d30,1,2,3,4,5)
        class Engine:
            stopped=False
            def emu_stop(self):self.stopped=True
        sandbox.base=0x140000000;sandbox.instructions=0;sandbox.visited=set();sandbox.error=None;engine=Engine()
        sandbox._code(engine,sandbox.base+0x1d75d30,3,None);self.assertFalse(engine.stopped)
        sandbox._code(engine,sandbox.base+0x1803230,1,None);self.assertTrue(engine.stopped)
        self.assertIn('unexpected native instruction',sandbox.error)
        sandbox.pe=types.SimpleNamespace(OPTIONAL_HEADER=types.SimpleNamespace(SizeOfImage=0x6000000))
        self.assertFalse(sandbox._unmapped(engine,0,0x9999,8,0,None))
    def test_bad_executable_never_claims_replay_and_cannot_overwrite(self):
        with self.assertRaises(ValueError):oracle.Sandbox(Path('relative.exe'))
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);exe=root/'untrusted.exe';exe.write_bytes(b'not the authenticated executable');out=root/'evidence'
            result=subprocess.run([sys.executable,'-I',str(ROOT/'scripts/native_camera_math_oracle.py'),'--exe',str(exe),'--output',str(out)],capture_output=True,text=True)
            self.assertEqual(result.returncode,1);data=(out/'oracle.json').read_bytes();report=json.loads(data)
            self.assertFalse(report['pass_']);self.assertFalse(report['actual_native_code_emulated']);self.assertNotIn('exe_sha256',report)
            result=subprocess.run([sys.executable,'-I',str(ROOT/'scripts/native_camera_math_oracle.py'),'--exe',str(exe),'--output',str(out)],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0);self.assertEqual(data,(out/'oracle.json').read_bytes())

if __name__=='__main__':unittest.main()
