import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from test_analyze_ordinary_second import control
from test_analyze_ordinary_upscaler import raster_observation

script=Path(__file__).resolve().parents[1]/'scripts/analyze_ordinary_native_packet.py'
spec=importlib.util.spec_from_file_location('native_packet_audit',script);audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
def bits(values):return list(struct.unpack('<'+'I'*len(values),struct.pack('<'+'f'*len(values),*values)))
def fixture():
    c=control();o=raster_observation(c);identity=[float(i%5==0) for i in range(16)]
    for slot,view in enumerate(c['views']):
        view['view']=list(identity);view['view'][12]=-view['world'][12]
        constants=o['streamline']['events'][slot*2]['constants']
        matrices={offset:list(identity) for offset in audit.OFFSETS}
        for offset,key in [('0x34','world'),('0x3dc','view'),('0x45c','projection'),('0x4dc','inverse_projection')]:matrices[offset]=list(view[key])
        matrices['0x49c']=list(constants['projection']);matrices['0x51c']=list(constants['inverse_projection'])
        matrices['0x55c']=audit.multiply(view['view'],view['projection']);matrices['0x59c']=audit.multiply(view['view'],constants['projection'])
        view['native_packet']=dict(model='native_camera_packet_v1',entry=c['ordinary_pair'][('left_entry','right_entry')[slot]],stable_double_read=True,resource_leases=False,matrix_words={k:bits(v) for k,v in matrices.items()},jitter_words=bits(constants['jitter']),jitter_index=17 if slot==0 else 0)
    return c,o

class NativePacketTests(unittest.TestCase):
    def test_stable_exact_native_lenses_and_combined_matrices_are_only_cpu_evidence(self):
        c,o=fixture();r=audit.analyze(c,o);self.assertTrue(r['native_packet_lens_pass'],r)
        for key in ('all_dependent_matrices_verified','previous_native_camera_verified','native_history_ownership_verified','gpu_pair_verified','runtime_submission'):self.assertFalse(r[key])

    def test_packet_identity_words_lenses_jitter_and_combined_matrices_are_independent(self):
        for mutation in ('entry','unstable','lease','word','nan','base','inverse','combined','base_combined','jitter','current','missing','sdk_failure'):
            c,o=fixture();p=c['views'][1]['native_packet'];m=p['matrix_words']
            if mutation=='entry':p['entry']+=1
            elif mutation=='unstable':p['stable_double_read']=False
            elif mutation=='lease':p['resource_leases']=True
            elif mutation=='word':m['0x34'][0]=True
            elif mutation=='nan':p['jitter_words'][0]=0x7fc00000
            elif mutation=='base':m['0x49c'][0]=bits([2])[0]
            elif mutation=='inverse':m['0x51c'][0]=0
            elif mutation=='combined':m['0x55c'][12]=0
            elif mutation=='base_combined':m['0x59c'][12]=0
            elif mutation=='jitter':p['jitter_words']=bits([.25,-.5])
            elif mutation=='current':m['0x45c'][8]=bits([.001])[0]
            elif mutation=='missing':del c['views'][1]['native_packet']
            else:o['streamline']['private_context']['receipts'][0]['evaluation_result']=1
            with self.subTest(mutation=mutation):self.assertFalse(audit.analyze(c,o)['native_packet_lens_pass'])

    def test_opaque_matrix_words_are_preserved_without_claiming_their_meaning(self):
        c,o=fixture();c['views'][1]['native_packet']['matrix_words']['0x7f4'][0]=0x7fc00000
        r=audit.analyze(c,o);self.assertTrue(r['native_packet_lens_pass'],r);self.assertNotIn('0x7f4',r['inventory'][1]['finite_matrices']);self.assertFalse(r['all_dependent_matrices_verified'])
        json.dumps(r,allow_nan=False)

    def test_cli_keeps_prior_private_reports_and_missing_packet_remains_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'private_eye_audit.json').write_text('preserved')
            c,o=fixture()
            for i in range(4):
                (root/f'second_{i}.json').write_text(json.dumps(c));(root/f'upscaler_{i}.json').write_text(json.dumps(o))
            command=[sys.executable,'-I',str(script),str(root)];r=subprocess.run(command,cwd=root,capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr+r.stdout)
            self.assertEqual((root/'private_eye_audit.json').read_text(),'preserved')
            del c['views'][1]['native_packet'];(root/'second_3.json').write_text(json.dumps(c));r=subprocess.run(command,cwd=root,capture_output=True,text=True);self.assertEqual(r.returncode,1,r.stderr+r.stdout)
            self.assertFalse(json.loads((root/'native_packet_audit.json').read_text())['all_controls_pass'])

if __name__=='__main__':unittest.main()
