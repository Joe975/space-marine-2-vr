import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('ordinary_second',Path(__file__).resolve().parents[1]/'scripts/analyze_ordinary_second.py');audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)

def control():
    world=[1.,0,0,0,0,1.,0,0,0,0,1.,0,0,0,0,1.];right=list(world);right[12]=.064
    c=dict(model='ordinary_second_camera_v2',cpu_only=True,pip_enabled=False,gpu_pair_verified=False,histories_verified=False,runtime_submission=False,dispatch=10,renderer=0x40000,list=0x10000,scene_calls=2,scene_refused=0,scene_dropped=0)
    a=dict(render_id=9,producer_revision=17,token=8,thread=1,list=c['list']+0x160,renderer=c['renderer'],left=0,right=1,left_flags=0x3f7c013,right_flags=0x3f7c013,refusal=0,appended=True,source_unchanged=True,bank_zero=True);c['append']=a
    c['ordinary_pair']=dict(render_id=9,token=8,revision=20,producer_list=a['list'],producer_revision=17,add_list=c['list']+0x160,left_entry=a['list']+8,right_entry=a['list']+8+0xa60,left_slot=0,right_slot=1)
    c['views']=[dict(slot=i,flags=a['left_flags'],world=w) for i,w in enumerate((world,right))];c['scenes']=[]
    for i in range(2):
        t=lambda:dict(previous_words=[0]*11,next_words=[0,0,123,0,0,456,0,0,0,0,0])
        c['scenes'].append(dict(slot=i,entry=a['list']+8+i*0xa60,active_entry_after=a['list']+8+i*0xa60,refusal=0,counter_before=12,counter_after=12,temporal_before=t(),temporal_after=t()))
    return c

class OrdinarySecondAuditTests(unittest.TestCase):
    def test_exact_two_ordinary_cameras_do_not_prove_private_histories(self):
        r=audit.analyze(control());self.assertTrue(r['ordinary_camera_provenance_pass'],r);self.assertEqual(r['shared_temporal_word_indices'],[2,5]);self.assertIs(r['histories_verified'],False);self.assertIs(r['gpu_pair_verified'],False)
    def test_append_and_producing_job_cannot_be_substituted(self):
        changes=[('append',k,v) for k,v in [('render_id',10),('token',7),('list',1),('renderer',1),('left',1),('right',0),('appended',False),('source_unchanged',False),('refusal',1),('right_flags',0x40)]]
        changes += [('ordinary_pair',k,0) for k in ('revision','left_entry','right_entry','render_id')]
        for section,key,value in changes:
            c=control();c[section][key]=value
            with self.subTest(section=section,key=key):self.assertFalse(audit.analyze(c)['ordinary_camera_provenance_pass'])
    def test_native_scene_and_camera_separation_required(self):
        for key,value in [('slot',0),('entry',0),('active_entry_after',0),('refusal',1),('counter_after',13),('temporal_before',None)]:
            c=control();c['scenes'][1][key]=value;self.assertFalse(audit.analyze(c)['ordinary_camera_provenance_pass'])
        for idx,value in [(12,0),(0,2),(13,.1)]:
            c=control();c['views'][1]['world'][idx]=value;self.assertFalse(audit.analyze(c)['ordinary_camera_provenance_pass'])
        c=control();c['histories_verified']=True;self.assertFalse(audit.analyze(c)['ordinary_camera_provenance_pass'])
    def test_copy_lineage_required_when_native_lists_differ(self):
        c=control();c['append']['list']=0x80000;c['ordinary_pair']['producer_list']=0x80000
        self.assertTrue(audit.analyze(c)['ordinary_camera_provenance_pass'])
        for key in ('producer_list','producer_revision','add_list'):
            bad=control();bad['ordinary_pair'][key]+=1
            with self.subTest(key=key):self.assertFalse(audit.analyze(bad)['ordinary_camera_provenance_pass'])
        c=control();c['model']='ordinary_second_camera_v1'
        self.assertFalse(audit.analyze(c)['ordinary_camera_provenance_pass'])
    def test_all_shared_nonzero_words_are_reported(self):
        c=control()
        for scene in c['scenes']:scene['temporal_after']['next_words'][9]=999
        self.assertEqual(audit.analyze(c)['shared_temporal_word_indices'],[2,5,9])
    def test_private_word_addresses_alone_are_not_independent_history_proof(self):
        c=control();c['scenes'][1]['temporal_after']['next_words'][2]=789;c['scenes'][1]['temporal_after']['next_words'][5]=987
        r=audit.analyze(c);self.assertTrue(r['ordinary_camera_provenance_pass']);self.assertEqual(r['shared_temporal_word_indices'],[]);self.assertIs(r['histories_verified'],False)

if __name__=='__main__':unittest.main()
