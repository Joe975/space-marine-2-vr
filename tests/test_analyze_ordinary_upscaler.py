import copy
import importlib.util
from pathlib import Path
import sys
import json
import subprocess
import tempfile
import unittest
from test_analyze_ordinary_second import control
scripts=Path(__file__).resolve().parents[1]/'scripts'
sys.path.insert(0,str(scripts))
spec=importlib.util.spec_from_file_location('ordinary_upscaler',scripts/'analyze_ordinary_upscaler.py');audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)


def observation(c):
    p=c['ordinary_pair'];events=[]
    for slot in range(2):
        events.append(dict(kind=0,dispatch=c['dispatch'],render_id=p['render_id'],revision=p['revision'],token=p['token'],slot=slot,entry=p['add_list']+8+slot*0xa60,list=c['list'],frame=p['token'],feature=0,thread=1,command=0x1234,viewport=0,readable=True,scope_current=True,unknown_inputs=False,result=0,tags=[dict(type=0,lifecycle=2,state=128,resource_type=0,width=0,height=0,native=0x5678)]))
    return dict(model='ordinary_upscaler_observation_v1',cpu_only=True,dispatch=c['dispatch'],histories_verified=False,runtime_submission=False,streamline=dict(installed=True,dropped=0,resource_leases=False,histories_verified=False,events=events))


def returned_observation(c,actual=200):
    o=observation(c);o['model']='ordinary_upscaler_observation_v2'
    for e in o['streamline']['events']:
        e.update(frame=actual,frame_after=actual,frame_object=0x9876,returned_frame_current=True,
                 returned_frame=dict(ordinal=11,epoch=1,object=0x9876,token=actual,thread=2,index=actual))
    return o


def constants_observation(c):
    o=returned_observation(c);o['model']='ordinary_upscaler_observation_v3';events=[]
    identity=[float(i%5==0) for i in range(16)]
    for evaluation in o['streamline']['events']:
        e=copy.deepcopy(evaluation);e.update(kind=3,command=0,tags=[])
        e['constants']=dict(version=1,reset=0,depth_inverted=1,near=.1,far=1000,fov=1,aspect=1.5,
            projection=identity,inverse_projection=identity,clip_to_previous=identity,previous_to_clip=identity,
            position=c['views'][e['slot']]['world'][12:15],right=[1,0,0],up=[0,1,0],forward=[0,0,1],jitter=[0,0],motion_scale=[1,1])
        events.extend((e,evaluation))
    o['streamline']['events']=events;return o


def options_observation(c,version=3):
    o=returned_observation(c);o['model']='ordinary_upscaler_observation_v4'
    for e in o['streamline']['events']:
        e.update(options_current=True,dlss_options=dict(epoch=1,serial=12,viewport=0,thread=2,version=version,mode=1,width=3840,height=2160,
            sharpness=0,pre_exposure=1,exposure_scale=1,hdr=1,invert_x=0,invert_y=0,presets=[0]*5,
            ultra_quality_preset=0 if version>=2 else None,auto_exposure=0 if version>=2 else None,alpha_upscaling=0 if version>=3 else None))
    return o


def private_observation(c):
    o=constants_observation(c);o['model']='ordinary_upscaler_observation_v5';viewport=0x534d3201
    mode=options_observation(c)['streamline']['events'][0]['dlss_options']
    identity=[float(i%5==0) for i in range(16)]
    for v in c['views']:v.update(projection=identity,inverse_projection=identity)
    for e in o['streamline']['events']:
        if e['slot']==1:e['viewport']=viewport
        if e['kind']==3:
            if e['slot']==1:e['constants']['reset']=1
        else:
            e['options_current']=True;e['dlss_options']=copy.deepcopy(mode);e['dlss_options']['viewport']=e['viewport'];e['dlss_options']['serial']+=2*e['slot']
            tag=e['tags'][0];e['tags']=[dict(tag,type=t,native=0x5000+t) for t in (0,1,3,4)]
    sdk=o['streamline']['events'][0]['returned_frame']
    o['streamline']['private_context']=dict(viewport=viewport,private_options_submitted=True,poisoned=False,retained_until_shutdown=True,histories_verified=False,
        receipts=[dict(dispatch=c['dispatch'],sdk_ordinal=sdk['ordinal'],token=sdk['token'],viewport=viewport,reset=True,continued=False,previous_ordinal=0,refusal=0,options_result=0,constants_result=0,evaluation_result=0)])
    return o


def raster_observation(c):
    o=private_observation(c);o['model']='ordinary_upscaler_observation_v6'
    for v in c['views']:
        for key in ('projection','inverse_projection'):v[key]=list(v[key])
    jitter=[.25,-.388888896];o['streamline']['events'][0]['constants']['jitter']=jitter
    p=c['views'][0]['projection'];inv=c['views'][0]['inverse_projection'];p[8]=2*jitter[0]/1920;p[9]=-2*jitter[1]/1080;inv[8]=-p[8];inv[9]=-p[9]
    for e in o['streamline']['events']:
        if e['kind']==0:
            for tag in e['tags']:tag.update(width=1920,height=1080,extent=[0,0,0,0])
    o['streamline']['private_context']['receipts'][0].update(raster_relation=True,jitter_parity=False,render_width=1920,render_height=1080,source_jitter=jitter,right_jitter=[0,0])
    return o


class OrdinaryUpscalerTests(unittest.TestCase):
    def test_actual_same_viewport_observation_is_not_independent_histories(self):
        c=control();r=audit.analyze(c,observation(c));self.assertTrue(r['api_observation_pass'],r);self.assertEqual(r['shared_evaluation_viewport'],0);self.assertEqual(r['shared_native_tag_types'],[0]);self.assertFalse(r['histories_verified'])
    def test_each_eye_must_match_actual_camera_and_successful_api(self):
        c=control();o=observation(c)
        changes=dict(dispatch=11,render_id=10,revision=21,token=7,slot=0,entry=0,list=0,frame=7,scope_current=False,readable=False,result=1,thread=0,command=0,viewport=None,kind=3,feature=1)
        for key,value in changes.items():
            bad=copy.deepcopy(o);bad['streamline']['events'][1][key]=value
            with self.subTest(key=key):self.assertFalse(audit.analyze(c,bad)['api_observation_pass'])
    def test_missing_duplicate_overflow_and_stronger_claims_refuse(self):
        c=control();o=observation(c)
        for key,value in [('installed',False),('dropped',1),('resource_leases',True),('histories_verified',True)]:
            bad=copy.deepcopy(o);bad['streamline'][key]=value;self.assertFalse(audit.analyze(c,bad)['api_observation_pass'])
        for events in [[],o['streamline']['events'][:1],o['streamline']['events']*2]:
            bad=copy.deepcopy(o);bad['streamline']['events']=events;self.assertFalse(audit.analyze(c,bad)['api_observation_pass'])
        o['histories_verified']=True;self.assertFalse(audit.analyze(c,o)['api_observation_pass'])
    def test_different_viewports_and_pointers_still_do_not_prove_history_ownership(self):
        c=control();o=observation(c);o['streamline']['events'][1]['viewport']=1;o['streamline']['events'][1]['tags'][0]['native']+=1
        r=audit.analyze(c,o);self.assertTrue(r['api_observation_pass']);self.assertIsNone(r['shared_evaluation_viewport']);self.assertEqual(r['shared_native_tag_types'],[]);self.assertFalse(r['histories_verified']);self.assertFalse(r['resource_leases'])
        o['streamline']['events'][0]['unknown_inputs']=True;r=audit.analyze(c,o);self.assertEqual(r['unknown_input_events'],1);self.assertFalse(r['histories_verified'])
    def test_isolated_cli_joins_all_four_files_from_another_working_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);c=control();o=observation(c)
            for i in range(4):
                (root/f'second_{i}.json').write_text(json.dumps(c));(root/f'upscaler_{i}.json').write_text(json.dumps(o))
            result=subprocess.run([sys.executable,'-I',str(scripts/'analyze_ordinary_upscaler.py'),str(root)],cwd=root,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr+result.stdout)
            report=json.loads((root/'upscaler_audit.json').read_text());self.assertTrue(report['all_controls_pass']);self.assertFalse(report['histories_verified'])
    def test_actual_returned_sdk_frame_does_not_rewrite_candidate_camera_clock(self):
        c=control()
        for token in (0,200,2**32-1):
            o=returned_observation(c,token);r=audit.analyze_returned(c,o)
            self.assertTrue(r['api_observation_pass'],r);self.assertTrue(r['sdk_frame_owner_verified']);self.assertFalse(r['candidate_clock_matches_sdk']);self.assertEqual(c['ordinary_pair']['token'],8)
            self.assertFalse(r['simulation_snapshot_verified']);self.assertFalse(r['histories_verified']);self.assertFalse(audit.analyze(c,o)['api_observation_pass'])
        o=observation(c);o['streamline']['events'][0]['frame']=9
        self.assertFalse(audit.analyze(c,o)['api_observation_pass'],'original v1 clock mismatch remains a strict failure')
    def test_current_returned_object_and_both_sides_of_api_are_required(self):
        c=control();o=returned_observation(c)
        for key,value in [('returned_frame_current',False),('returned_frame',None),('frame_object',0),('frame_after',201),('frame',201)]:
            bad=copy.deepcopy(o);bad['streamline']['events'][0][key]=value
            with self.subTest(key=key):self.assertFalse(audit.analyze_returned(c,bad)['api_observation_pass'])
        for key,value in [('ordinal',0),('epoch',0),('object',0),('token',201),('thread',0),('index',-1)]:
            bad=copy.deepcopy(o);bad['streamline']['events'][0]['returned_frame'][key]=value
            with self.subTest(return_key=key):self.assertFalse(audit.analyze_returned(c,bad)['api_observation_pass'])
        bad=copy.deepcopy(o);bad['streamline']['events'].reverse();bad['streamline']['events'][1]['returned_frame']=None
        self.assertFalse(audit.analyze_returned(c,bad)['api_observation_pass'],'incomplete right-first evidence must refuse without crashing')
    def test_matching_numeric_tokens_alone_cannot_join_two_sdk_returns(self):
        c=control();o=returned_observation(c)
        for key in ('ordinal','epoch','object','thread','index'):
            bad=copy.deepcopy(o);bad['streamline']['events'][1]['returned_frame'][key]+=1
            if key=='object':bad['streamline']['events'][1]['frame_object']+=1
            self.assertFalse(audit.analyze_returned(c,bad)['api_observation_pass'])
        for key in ('returned_frame','frame_object','frame_after','returned_frame_current'):
            bad=copy.deepcopy(o)
            for e in bad['streamline']['events']:del e[key]
            self.assertFalse(audit.analyze_returned(c,bad)['api_observation_pass'])
    def test_frame_tags_must_consume_the_same_return_and_viewport(self):
        c=control();o=returned_observation(c);tag=copy.deepcopy(o['streamline']['events'][1]);tag['kind']=2;o['streamline']['events'].append(tag)
        self.assertTrue(audit.analyze_returned(c,o)['api_observation_pass'])
        tag['returned_frame']['ordinal']+=1;self.assertFalse(audit.analyze_returned(c,o)['api_observation_pass']);tag['returned_frame']['ordinal']-=1
        tag['viewport']=1;self.assertFalse(audit.analyze_returned(c,o)['api_observation_pass'])
    def test_isolated_explicit_returned_frame_cli_preserves_separate_audit(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);c=control();o=returned_observation(c)
            for i in range(4):
                (root/f'second_{i}.json').write_text(json.dumps(c));(root/f'upscaler_{i}.json').write_text(json.dumps(o))
            result=subprocess.run([sys.executable,'-I',str(scripts/'analyze_ordinary_upscaler.py'),str(root),'--returned-frame'],cwd=root,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr+result.stdout);self.assertFalse((root/'upscaler_audit.json').exists())
            report=json.loads((root/'sdk_frame_audit.json').read_text());self.assertTrue(report['all_controls_pass']);self.assertTrue(report['controls'][0]['sdk_frame_owner_verified']);self.assertFalse(report['histories_verified'])
    def test_actual_constants_precede_each_eye_and_follow_native_positions(self):
        c=control();r=audit.analyze_constants(c,constants_observation(c))
        self.assertTrue(r['constants_observation_pass'],r);self.assertTrue(r['sdk_frame_owner_verified']);self.assertEqual(r['submitted_positions'][1],[.064,0,0])
        for key in ('histories_verified','projection_parity_verified','temporal_transform_ownership_verified','gpu_pair_verified','runtime_submission'):self.assertFalse(r[key])
    def test_shared_wrong_missing_or_late_camera_constants_refuse(self):
        c=control();o=constants_observation(c)
        for position in ([0,0,0],[.064,0,.01]):
            bad=copy.deepcopy(o);bad['streamline']['events'][2]['constants']['position']=position
            r=audit.analyze_constants(c,bad);self.assertFalse(r['constants_observation_pass']);self.assertTrue(r['sdk_frame_owner_verified'])
        for events in (o['streamline']['events'][1:],list(reversed(o['streamline']['events'])),o['streamline']['events'][:1]+o['streamline']['events']):
            bad=copy.deepcopy(o);bad['streamline']['events']=events;self.assertFalse(audit.analyze_constants(c,bad)['constants_observation_pass'])
        self.assertFalse(audit.analyze_constants(c,returned_observation(c))['constants_observation_pass'])
    def test_constants_arrays_scalars_flags_and_unknowns_fail_closed(self):
        c=control();o=constants_observation(c)
        changes=[('projection',[1]),('inverse_projection',[float('nan')]*16),('clip_to_previous',[float('inf')]*16),('position',[3.4028234663852886e38]*3),('version',3),('reset',2),('depth_inverted',True),('near',0),('far',.01),('fov',4),('aspect',-1)]
        for key,value in changes:
            bad=copy.deepcopy(o);bad['streamline']['events'][0]['constants'][key]=value
            with self.subTest(key=key):self.assertFalse(audit.analyze_constants(c,bad)['constants_observation_pass'])
        bad=copy.deepcopy(o);bad['streamline']['events'][0]['unknown_inputs']=True;self.assertFalse(audit.analyze_constants(c,bad)['constants_observation_pass'])
    def test_constants_require_actual_sdk_owner_and_no_invented_command_buffer(self):
        c=control();o=constants_observation(c)
        for key,value in [('command',1),('returned_frame_current',False),('frame_after',201),('viewport',1)]:
            bad=copy.deepcopy(o);bad['streamline']['events'][2][key]=value;self.assertFalse(audit.analyze_constants(c,bad)['constants_observation_pass'])
        for key in ('ordinal','epoch','object'):
            bad=copy.deepcopy(o);bad['streamline']['events'][2]['returned_frame'][key]+=1;self.assertFalse(audit.analyze_constants(c,bad)['constants_observation_pass'])
        o['model']='ordinary_upscaler_observation_v2';self.assertFalse(audit.analyze_returned(c,o)['api_observation_pass'],'historical v2 does not accept kind 3')
    def test_isolated_constants_cli_retains_separate_audit(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);c=control();o=constants_observation(c)
            for i in range(4):
                (root/f'second_{i}.json').write_text(json.dumps(c));(root/f'upscaler_{i}.json').write_text(json.dumps(o))
            result=subprocess.run([sys.executable,'-I',str(scripts/'analyze_ordinary_upscaler.py'),str(root),'--constants'],cwd=root,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr+result.stdout);self.assertFalse((root/'sdk_frame_audit.json').exists())
            report=json.loads((root/'constants_audit.json').read_text());self.assertTrue(report['all_controls_pass']);self.assertTrue(report['controls'][0]['constants_observation_pass']);self.assertFalse(report['histories_verified'])
    def test_native_options_for_each_evaluation_do_not_prove_private_contexts(self):
        c=control()
        for version in (1,2,3):
            r=audit.analyze_options(c,options_observation(c,version));self.assertTrue(r['options_observation_pass'],r);self.assertFalse(r['private_contexts_verified']);self.assertFalse(r['histories_verified'])
    def test_missing_changed_malformed_and_fabricated_historical_options_fail(self):
        c=control();o=options_observation(c)
        for key,value in [('options_current',False),('dlss_options',None)]:
            bad=copy.deepcopy(o);bad['streamline']['events'][1][key]=value;self.assertFalse(audit.analyze_options(c,bad)['options_observation_pass'])
        for key,value in [('epoch',0),('serial',0),('thread',0),('viewport',1),('version',4),('mode',7),('width',0),('height',2**32-1),('sharpness',2),('pre_exposure',float('nan')),('exposure_scale',0),('hdr',True),('presets',[0]),('alpha_upscaling',None)]:
            bad=copy.deepcopy(o);bad['streamline']['events'][1]['dlss_options'][key]=value
            with self.subTest(key=key):self.assertFalse(audit.analyze_options(c,bad)['options_observation_pass'])
        for version,key in [(1,'ultra_quality_preset'),(1,'auto_exposure'),(2,'alpha_upscaling')]:
            bad=options_observation(c,version);bad['streamline']['events'][0]['dlss_options'][key]=0;self.assertFalse(audit.analyze_options(c,bad)['options_observation_pass'])
        self.assertFalse(audit.analyze_options(c,returned_observation(c))['options_observation_pass'])
    def test_shared_context_options_cannot_change_between_eyes(self):
        c=control();o=options_observation(c);o['streamline']['events'][1]['dlss_options']['serial']+=1
        r=audit.analyze_options(c,o);self.assertFalse(r['options_observation_pass']);self.assertTrue(r['sdk_frame_owner_verified'])
    def test_isolated_actual_options_cli_keeps_other_audits(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);c=control();o=options_observation(c)
            for i in range(4):
                (root/f'second_{i}.json').write_text(json.dumps(c));(root/f'upscaler_{i}.json').write_text(json.dumps(o))
            result=subprocess.run([sys.executable,'-I',str(scripts/'analyze_ordinary_upscaler.py'),str(root),'--options'],cwd=root,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr+result.stdout);self.assertFalse((root/'constants_audit.json').exists())
            report=json.loads((root/'options_audit.json').read_text());self.assertTrue(report['all_controls_pass']);self.assertFalse(report['histories_verified'])
    def test_private_input_bundle_is_not_native_history_or_gpu_readiness(self):
        c=control();r=audit.analyze_private(c,private_observation(c));self.assertTrue(r['private_sdk_input_bundle_pass'],r);self.assertTrue(r['native_sdk_projection_match'])
        for key in ('histories_verified','private_contexts_verified','native_history_ownership_verified','gpu_pair_verified','runtime_submission'):self.assertFalse(r[key])
    def test_private_bundle_requires_exact_camera_sdk_setup_and_local_tags(self):
        c=control();o=private_observation(c)
        for key,value in [('sdk_ordinal',12),('token',201),('viewport',0),('refusal',1),('options_result',1),('constants_result',1),('evaluation_result',1),('reset',False),('continued',True),('previous_ordinal',10)]:
            bad=copy.deepcopy(o);bad['streamline']['private_context']['receipts'][0][key]=value
            with self.subTest(key=key):self.assertFalse(audit.analyze_private(c,bad)['private_sdk_input_bundle_pass'])
        for mutate in ('pose','basis','projection','previous','missing_tag','native_options'):
            bad=copy.deepcopy(o);right=bad['streamline']['events'][2]
            if mutate=='pose':right['constants']['position'][0]=0
            elif mutate=='basis':right['constants']['right'][0]=-1
            elif mutate=='projection':right['constants']['projection'][0]=2
            elif mutate=='previous':right['constants']['clip_to_previous'][12]=.064
            elif mutate=='missing_tag':bad['streamline']['events'][3]['tags'].pop()
            elif mutate=='native_options':bad['streamline']['events'][3]['dlss_options']['auto_exposure']=1
            self.assertFalse(audit.analyze_private(c,bad)['private_sdk_input_bundle_pass'],mutate)
    def test_only_unrelated_native_hudless_tag_can_keep_viewport_zero(self):
        c=control();o=private_observation(c);hud=copy.deepcopy(o['streamline']['events'][3]);hud.update(kind=1,viewport=0,tags=[dict(hud['tags'][0],type=2)])
        o['streamline']['events'].append(hud);self.assertTrue(audit.analyze_private(c,o)['private_sdk_input_bundle_pass'])
        for type_id in (0,1,3,4):
            hud['tags'][0]['type']=type_id;self.assertFalse(audit.analyze_private(c,o)['private_sdk_input_bundle_pass'])
        hud['tags'][0]['type']=2;hud['kind']=2;self.assertFalse(audit.analyze_private(c,o)['private_sdk_input_bundle_pass'])
    def test_continued_private_history_requires_actual_previous_right_capture(self):
        c=control();o=private_observation(c);r=o['streamline']['private_context']['receipts'][0];r.update(reset=False,continued=True,previous_ordinal=10);o['streamline']['events'][2]['constants']['reset']=0
        self.assertFalse(audit.analyze_private(c,o)['private_sdk_input_bundle_pass'])
        self.assertFalse(audit.analyze_private(c,o,previous=(control(),private_observation(control())))['private_sdk_input_bundle_pass'])
    def test_continued_transform_uses_accepted_previous_right_not_previous_left(self):
        pc=control();pc['dispatch']=9;pc['append']['render_id']=pc['ordinary_pair']['render_id']=8
        for v in pc['views']:v['world'][12]-=1
        for s in pc['scenes']:s['counter_before']=s['counter_after']=11
        po=private_observation(pc)
        for e in po['streamline']['events']:e.update(frame=199,frame_after=199);e['returned_frame'].update(ordinal=10,token=199,index=199)
        po['streamline']['private_context']['receipts'][0].update(sdk_ordinal=10,token=199)
        c=control();o=private_observation(c);o['streamline']['private_context']['receipts'][0].update(reset=False,continued=True,previous_ordinal=10)
        right=o['streamline']['events'][2]['constants'];right['reset']=0;right['clip_to_previous']=list(right['clip_to_previous']);right['previous_to_clip']=list(right['previous_to_clip']);right['clip_to_previous'][12]=1;right['previous_to_clip'][12]=-1
        self.assertTrue(audit.analyze_private(c,o,previous=(pc,po))['private_sdk_input_bundle_pass'])
        right['clip_to_previous'][12]=1.064;self.assertFalse(audit.analyze_private(c,o,previous=(pc,po))['private_sdk_input_bundle_pass'],'borrowing the previous left pose adds an erroneous eye separation');right['clip_to_previous'][12]=1
        po['streamline']['private_context']['receipts'][0]['refusal']=9;self.assertFalse(audit.analyze_private(c,o,previous=(pc,po))['private_sdk_input_bundle_pass'],'a failed prior setup cannot become an accepted private predecessor')
    def test_isolated_private_cli_preserves_all_other_gate_results(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for i in range(4):
                c=control();c['dispatch']+=241*i;o=private_observation(c)
                (root/f'second_{i}.json').write_text(json.dumps(c));(root/f'upscaler_{i}.json').write_text(json.dumps(o))
            result=subprocess.run([sys.executable,'-I',str(scripts/'analyze_ordinary_upscaler.py'),str(root),'--private-eye'],cwd=root,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr+result.stdout);self.assertFalse((root/'constants_audit.json').exists())
            report=json.loads((root/'private_eye_audit.json').read_text());self.assertTrue(report['all_controls_pass']);self.assertFalse(report['histories_verified'])


    def test_explicit_jitter_relation_keeps_unequal_native_eyes_visible(self):
        c=control();o=raster_observation(c);r=audit.analyze_private(c,o)
        self.assertTrue(r['private_sdk_input_bundle_pass'],r);self.assertTrue(r['native_sdk_projection_relation_verified']);self.assertFalse(r['native_sdk_projection_match']);self.assertFalse(r['native_raster_jitter_parity']);self.assertFalse(r['projection_parity_verified']);self.assertFalse(r['histories_verified'])
        old=copy.deepcopy(o);old['model']='ordinary_upscaler_observation_v5';self.assertFalse(audit.analyze_private(c,old)['private_sdk_input_bundle_pass'],'historical v5 raw-projection equality stays failed')
    def test_raster_relation_rejects_wrong_sign_dimensions_matrices_or_parity(self):
        for change in ('jitter_sign','copied_left_jitter','missing_extent','wrong_extent','wrong_dimensions','native_inverse','native_lens','false_parity'):
            c=control();o=raster_observation(c)
            if change=='jitter_sign':o['streamline']['events'][0]['constants']['jitter'][1]*=-1
            elif change=='copied_left_jitter':o['streamline']['events'][2]['constants']['jitter']=list(o['streamline']['events'][0]['constants']['jitter'])
            elif change=='missing_extent':del o['streamline']['events'][1]['tags'][2]['extent']
            elif change=='wrong_extent':o['streamline']['events'][1]['tags'][2]['extent']=[0,1920,1920,1080]
            elif change=='wrong_dimensions':o['streamline']['private_context']['receipts'][0]['render_width']=3840
            elif change=='native_inverse':c['views'][0]['inverse_projection'][8]=0
            elif change=='native_lens':c['views'][1]['projection'][0]=2
            elif change=='false_parity':o['streamline']['private_context']['receipts'][0]['jitter_parity']=True
            with self.subTest(change=change):self.assertFalse(audit.analyze_private(c,o)['private_sdk_input_bundle_pass'])
    def test_raster_relation_uses_actual_partial_input_extent(self):
        c=control();o=raster_observation(c);r=o['streamline']['private_context']['receipts'][0];r.update(render_width=960,render_height=540)
        for e in o['streamline']['events']:
            if e['kind']==0:e['tags'][2]['extent']=[0,0,960,540]
        p=c['views'][0]['projection'];inv=c['views'][0]['inverse_projection'];p[8]*=2;p[9]*=2;inv[8]=-p[8];inv[9]=-p[9]
        self.assertTrue(audit.analyze_private(c,o)['private_sdk_input_bundle_pass'])
        o['streamline']['events'][1]['tags'][2]['extent']=[0,0,0,0];self.assertFalse(audit.analyze_private(c,o)['private_sdk_input_bundle_pass'])
    def test_actual_matched_native_jitter_is_reported_without_promoting_gpu_history(self):
        c=control();o=raster_observation(c);jitter=list(o['streamline']['events'][0]['constants']['jitter']);o['streamline']['events'][2]['constants']['jitter']=jitter
        for key in ('projection','inverse_projection'):c['views'][1][key]=list(c['views'][0][key])
        o['streamline']['private_context']['receipts'][0].update(right_jitter=jitter,jitter_parity=True)
        r=audit.analyze_private(c,o);self.assertTrue(r['private_sdk_input_bundle_pass'],r);self.assertTrue(r['native_raster_jitter_parity']);self.assertFalse(r['gpu_pair_verified']);self.assertFalse(r['histories_verified'])


class OrdinaryContinuityTests(unittest.TestCase):
    def sequence(self):
        pairs=[]
        for index in range(4):
            c=control();c['dispatch']+=index
            for key in ('render_id','token'):
                c['append'][key]+=index;c['ordinary_pair'][key]+=index
            for scene in c['scenes']:
                scene['counter_before']+=index;scene['counter_after']+=index
            o=raster_observation(c)
            for event in o['streamline']['events']:
                event['frame']+=index;event['frame_after']+=index
                event['returned_frame']['ordinal']+=index;event['returned_frame']['token']+=index;event['returned_frame']['index']+=index
            receipt=o['streamline']['private_context']['receipts'][0]
            receipt.update(sdk_ordinal=11+index,token=200+index,reset=index==0,continued=index!=0,previous_ordinal=10+index if index else 0)
            o['streamline']['events'][2]['constants']['reset']=int(index==0)
            pairs.append((c,o))
        return pairs

    def test_continuous_camera_inputs_do_not_prove_gpu_or_native_histories(self):
        result=audit.analyze_continuity(self.sequence());self.assertTrue(result['all_controls_pass'],result)
        self.assertTrue(result['sdk_camera_continuity_verified'])
        for key in ('histories_verified','native_history_ownership_verified','gpu_pair_verified','runtime_submission'):self.assertFalse(result[key])

    def test_spaced_reset_and_missing_or_failed_predecessor_refuse(self):
        c=control();o=raster_observation(c)
        self.assertFalse(audit.analyze_continuity([(c,o)]*4)['all_controls_pass'])
        for mutation in ('reset','counter','sdk','producer','failure','missing','malformed'):
            pairs=self.sequence();c,o=pairs[2];receipt=o['streamline']['private_context']['receipts'][0]
            if mutation=='reset':receipt.update(reset=True,continued=False,previous_ordinal=0);o['streamline']['events'][2]['constants']['reset']=1
            elif mutation=='counter':c['scenes'][1]['counter_before']+=1
            elif mutation=='sdk':receipt['previous_ordinal']+=1
            elif mutation=='producer':c['append']['render_id']+=1;c['ordinary_pair']['render_id']+=1
            elif mutation=='failure':receipt['evaluation_result']=1
            elif mutation=='malformed':del c['append']
            else:pairs.pop()
            with self.subTest(mutation=mutation):self.assertFalse(audit.analyze_continuity(pairs)['all_controls_pass'])

    def test_continuity_cli_has_separate_report_and_preserves_old_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'private_eye_audit.json').write_text('preserved')
            for i,(c,o) in enumerate(self.sequence()):
                (root/f'second_{i}.json').write_text(json.dumps(c));(root/f'upscaler_{i}.json').write_text(json.dumps(o))
            result=subprocess.run([sys.executable,'-I',str(scripts/'analyze_ordinary_upscaler.py'),str(root),'--private-eye','--require-continuity'],cwd=root,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr+result.stdout)
            self.assertTrue(json.loads((root/'private_continuity_audit.json').read_text())['sdk_camera_continuity_verified'])
            self.assertEqual((root/'private_eye_audit.json').read_text(),'preserved')


if __name__=='__main__':unittest.main()
