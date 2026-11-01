"""Join actual upscaler API arguments to ordinary cameras; never certify histories."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
_spec=importlib.util.spec_from_file_location('ordinary_camera_audit',Path(__file__).resolve().with_name('analyze_ordinary_second.py'))
_camera=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(_camera)
camera_audit=_camera.analyze


def analyze(camera, observation):
    return _analyze(camera, observation, returned=False)


def analyze_returned(camera, observation):
    return _analyze(camera, observation, returned=True)


def _analyze(camera, observation, returned):
    errors=[];evaluations={};tagged={0:{},1:{}};unknown=0;owners={};frame_events=[]
    try:
        if not camera_audit(camera)['ordinary_camera_provenance_pass']:
            raise ValueError('ordinary camera provenance did not pass')
        o=observation;b=o['streamline'];p=camera['ordinary_pair']
        models=tuple('ordinary_upscaler_observation_v'+str(i) for i in range(2,7)) if returned else ('ordinary_upscaler_observation_v1',)
        constants_model=o['model'] in tuple('ordinary_upscaler_observation_v'+str(i) for i in range(3,7))
        if (o['model'] not in models or o['cpu_only'] is not True
                or o['histories_verified'] is not False or o['runtime_submission'] is not False
                or o['dispatch']!=camera['dispatch'] or b['installed'] is not True
                or b['dropped']!=0 or b['resource_leases'] is not False or b['histories_verified'] is not False):
            raise ValueError('incomplete or unsupported actual upscaler observation')
        events=b['events']
        if not 2<=len(events)<=32:raise ValueError('bounded actual API observations required')
        for e in events:
            slot=e['slot']
            if (type(slot) is not int or slot not in (0,1) or e['dispatch']!=camera['dispatch']
                    or e['render_id']!=p['render_id'] or e['revision']!=p['revision'] or e['token']!=p['token']
                    or e['list']!=camera['list'] or e['entry']!=p['add_list']+8+slot*0xa60
                    or e['scope_current'] is not True or e['readable'] is not True or e['result']!=0
                    or type(e['thread']) is not int or e['thread']<=0 or type(e['command']) is not int
                    or (e['command']!=0 if e['kind']==3 else e['command']<=0)
                    or type(e['viewport']) is not int or not 0<=e['viewport']<2**32
                    or type(e['unknown_inputs']) is not bool or type(e['kind']) is not int or e['kind'] not in ((0,1,2,3) if constants_model else (0,1,2))):
                raise ValueError('actual API event does not match its current producing eye')
            unknown+=int(e['unknown_inputs'])
            if e['kind'] in (0,2,3):
                if not returned and e['frame']!=p['token']:
                    raise ValueError('actual API frame token differs from producing camera')
                if returned:
                    r=e['returned_frame']
                    if (e['returned_frame_current'] is not True or type(e['frame_object']) is not int or e['frame_object']<=0
                            or type(e['frame']) is not int or not 0<=e['frame']<2**32
                            or type(e['frame_after']) is not int or e['frame_after']!=e['frame']
                            or r['object']!=e['frame_object'] or type(r['token']) is not int or r['token']!=e['frame']):
                        raise ValueError('actual API argument lacks its stable returned SDK frame owner')
                    for key in ('ordinal','epoch','object','thread'):
                        if type(r[key]) is not int or r[key]<=0:raise ValueError('incomplete successful SDK return identity')
                    if r['index'] is not None and (type(r['index']) is not int or not 0<=r['index']<2**32):
                        raise ValueError('invalid explicit SDK frame index')
                    frame_events.append(r)
                    if e['kind']==0 and e['feature']==0:owners[slot]=r
            if e['kind']==0 and e['feature']==0:  # SDK 2.7.30 kFeatureDLSS
                if slot in evaluations:raise ValueError('multiple DLSS evaluations for one eye')
                evaluations[slot]=e['viewport']
            if len(e['tags'])>16:raise ValueError('tag observation overflow')
            for t in e['tags']:
                for key in ('type','lifecycle','state','resource_type','width','height','native'):
                    if type(t[key]) is not int or t[key]<0:raise ValueError('invalid POD resource tag')
                if not t['native']:raise ValueError('missing actual tagged native pointer')
                tagged[slot][t['type']]=t['native']
        if set(evaluations)!={0,1}:raise ValueError('one actual DLSS evaluation for each ordinary eye required')
        for e in events:
            if e['kind'] in (1,2,3) and e['viewport']!=evaluations[e['slot']]:
                # V5 deliberately keeps the game's unrelated HUD-less colour tag
                # on native viewport 0. It is not an input to private DLSS.
                if not (o['model'] in ('ordinary_upscaler_observation_v5','ordinary_upscaler_observation_v6') and e['kind']==1 and e['viewport']==0 and len(e['tags'])==1 and e['tags'][0]['type']==2):
                    raise ValueError('tag/constants viewport differs from its actual eye evaluation')
        if returned and (set(owners)!={0,1} or any(r!=owners[0] for r in frame_events)):
            raise ValueError('both eyes and frame-based tags must consume one observed SDK frame return')
    except (KeyError,TypeError,ValueError,IndexError) as error:errors.append(str(error))
    shared_viewport=evaluations.get(0) if len(evaluations)==2 and evaluations[0]==evaluations[1] else None
    shared_tags=sorted(k for k,v in tagged[0].items() if tagged[1].get(k)==v)
    return dict(api_observation_pass=not errors,errors=errors,evaluation_viewports=evaluations,
                shared_evaluation_viewport=shared_viewport,shared_native_tag_types=shared_tags,
                unknown_input_events=unknown,resource_leases=False,histories_verified=False,
                sdk_frame_owner_verified=returned and not errors,
                candidate_clock_matches_sdk=(owners[0]['token']==camera['ordinary_pair']['token']) if 0 in owners else None,
                sdk_frame=owners.get(0),simulation_snapshot_verified=False,
                gpu_pair_verified=False,runtime_submission=False)


def analyze_constants(camera, observation):
    result=analyze_returned(camera,observation);errors=list(result['errors']);constants={};positions={}
    try:
        if observation['model'] not in tuple('ordinary_upscaler_observation_v'+str(i) for i in range(3,7)):raise ValueError('actual constants observations required')
        if errors:raise ValueError('actual camera/API/SDK ownership must pass before camera constants')
        events=observation['streamline']['events'];evaluated=set()
        for e in events:
            slot=e['slot']
            if e['kind']==0 and e['feature']==0:
                evaluated.add(slot)
                if slot not in constants:raise ValueError('actual constants must precede each eye evaluation inside its Scene')
            if e['kind']!=3:continue
            if slot in constants or slot in evaluated:raise ValueError('exactly one preceding actual constants call per eye required')
            if e['unknown_inputs'] or e['tags']:raise ValueError('constants extensions or resource tags are unsupported')
            c=e['constants']
            if type(c['version']) is not int or c['version'] not in (1,2):raise ValueError('unsupported constants version')
            for key in ('reset','depth_inverted'):
                if type(c[key]) is not int or c[key] not in (0,1):raise ValueError('invalid SDK boolean')
            for key,size in [('projection',16),('inverse_projection',16),('clip_to_previous',16),('previous_to_clip',16),('position',3),('right',3),('up',3),('forward',3),('jitter',2),('motion_scale',2)]:
                a=c[key]
                if len(a)!=size or not all(type(f) in (int,float) and math.isfinite(f) and abs(f)<3.4028234e38 for f in a):raise ValueError('invalid constants array: '+key)
            for key in ('near','far','fov','aspect'):
                if type(c[key]) not in (int,float) or not math.isfinite(c[key]) or abs(c[key])>=3.4028234e38:raise ValueError('invalid scalar: '+key)
            if not (0<c['near']<c['far'] and 0<c['fov']<math.pi and c['aspect']>0):raise ValueError('invalid camera lens constants')
            constants[slot]=c;positions[slot]=c['position']
        if set(constants)!={0,1}:raise ValueError('actual constants for both ordinary views required')
        for slot in (0,1):
            world=camera['views'][slot]['world']
            if any(abs(constants[slot]['position'][i]-world[12+i])>3e-4 for i in range(3)):
                raise ValueError('submitted SDK position differs from actual native eye '+str(slot))
    except (KeyError,TypeError,ValueError,IndexError) as error:errors.append(str(error))
    result.update(constants_observation_pass=not errors,constants_errors=errors,submitted_positions=positions,
                  projection_parity_verified=False,temporal_transform_ownership_verified=False)
    return result


def analyze_options(camera, observation):
    result=analyze_returned(camera,observation);errors=list(result['errors']);options={}
    try:
        if observation['model'] not in ('ordinary_upscaler_observation_v4','ordinary_upscaler_observation_v5','ordinary_upscaler_observation_v6'):raise ValueError('actual DLSS options observations required')
        if errors:raise ValueError('actual camera/API/SDK ownership must pass before context options')
        for e in observation['streamline']['events']:
            if e['kind']!=0 or e['feature']!=0:continue
            o=e['dlss_options']
            if e['options_current'] is not True or o['viewport']!=e['viewport']:raise ValueError('missing or changed actual viewport options around evaluation')
            for key in ('epoch','serial','thread'):
                if type(o[key]) is not int or o[key]<=0:raise ValueError('missing actual option submission identity')
            if type(o['version']) is not int or o['version'] not in (1,2,3):raise ValueError('unsupported actual options layout')
            if type(o['mode']) is not int or not 0<=o['mode']<7:raise ValueError('unsupported DLSS mode')
            for key in ('width','height'):
                if type(o[key]) is not int or not 0<o[key]<2**32-1:raise ValueError('missing native output size')
            for key in ('sharpness','pre_exposure','exposure_scale'):
                if type(o[key]) not in (int,float) or not math.isfinite(o[key]) or abs(o[key])>=3.4028234e38:raise ValueError('invalid native options scalar')
            if not (0<=o['sharpness']<=1 and o['pre_exposure']>0 and o['exposure_scale']>0):raise ValueError('invalid native exposure/sharpness')
            for key in ('hdr','invert_x','invert_y'):
                if type(o[key]) is not int or o[key] not in (0,1):raise ValueError('invalid options boolean')
            if len(o['presets'])!=5 or any(type(p) is not int or not 0<=p<16 for p in o['presets']):raise ValueError('invalid native presets')
            for key,version,maximum in [('ultra_quality_preset',2,16),('auto_exposure',2,2),('alpha_upscaling',3,2)]:
                if o['version']<version:
                    if o[key] is not None:raise ValueError('unobserved historical options tail must be null')
                elif type(o[key]) is not int or not 0<=o[key]<maximum:raise ValueError('invalid versioned options tail')
            options[e['slot']]=o
        if set(options)!={0,1}:raise ValueError('actual successful options for both evaluations required')
        if result['shared_evaluation_viewport'] is not None and options[0]!=options[1]:raise ValueError('shared context changed options between eyes')
    except (KeyError,TypeError,ValueError,IndexError) as error:errors.append(str(error))
    result.update(options_observation_pass=not errors,options_errors=errors,evaluation_options=options,private_contexts_verified=False)
    return result


def analyze_private(camera, observation, previous=None):
    result=analyze_constants(camera,observation);options=analyze_options(camera,observation)
    errors=list(result['constants_errors'])+list(options['options_errors']);receipt=None;jitter_parity=None;raster_model=observation.get('model')=='ordinary_upscaler_observation_v6'
    try:
        if observation['model'] not in ('ordinary_upscaler_observation_v5','ordinary_upscaler_observation_v6') or errors:raise ValueError('private camera/SDK/constants/options gates must pass')
        private=observation['streamline']['private_context'];viewport=0x534d3201
        if (private['viewport']!=viewport or private['private_options_submitted'] is not True or private['poisoned'] is not False
                or private['retained_until_shutdown'] is not True or private['histories_verified'] is not False
                or not 1<=len(private['receipts'])<=4):raise ValueError('missing bounded private-context observation')
        matches=[r for r in private['receipts'] if r['dispatch']==camera['dispatch']]
        if len(matches)!=1:raise ValueError('one exact native dispatch private receipt required')
        receipt=matches[0];sdk=result['sdk_frame']
        if (receipt['sdk_ordinal']!=sdk['ordinal'] or receipt['token']!=sdk['token'] or receipt['viewport']!=viewport
                or receipt['refusal']!=0 or receipt['constants_result']!=0 or receipt['evaluation_result']!=0
                or receipt['options_result'] not in (None,0) or type(receipt['reset']) is not bool or type(receipt['continued']) is not bool
                or receipt['reset']==receipt['continued'] or type(receipt['previous_ordinal']) is not int
                or (receipt['previous_ordinal']!=0 if receipt['reset'] else receipt['previous_ordinal']+1!=sdk['ordinal'])):
            raise ValueError('private setup/evaluation/reset receipt does not join actual SDK frame')
        if result['evaluation_viewports']!={0:0,1:viewport}:raise ValueError('native left and private right evaluations must be distinct')
        events=observation['streamline']['events'];right=[e for e in events if e['kind']==3 and e['slot']==1]
        if len(right)!=1 or right[0]['constants']['reset']!=int(receipt['reset']):raise ValueError('actual private constants reset differs from its accepted predecessor policy')
        constants={e['slot']:e['constants'] for e in events if e['kind']==3}
        identity=[float(i%5==0) for i in range(16)]
        multiply=lambda a,b:[sum(a[row*4+k]*b[k*4+col] for k in range(4)) for row in range(4) for col in range(4)]
        for slot in (0,1):
            native=camera['views'][slot];c=constants[slot]
            for key in ('projection','inverse_projection'):
                if len(native[key])!=16 or any(type(f) not in (int,float) or not math.isfinite(f) for f in native[key]):raise ValueError('invalid actual native projection')
                if not raster_model and any(abs(a-b)>3e-5 for a,b in zip(native[key],c[key])):raise ValueError('actual native/SDK projection mismatch')
            if raster_model:
                evaluations=[e for e in events if e['kind']==0 and e['feature']==0 and e['slot']==slot];colour=[t for t in evaluations[0]['tags'] if t['type']==3]
                if len(colour)!=1:raise ValueError('one actual scaling-input extent required')
                tag=colour[0];extent=tag['extent']
                if len(extent)!=4 or any(type(n) is not int or not 0<=n<2**32 for n in extent) or not tag['width'] or not tag['height']:raise ValueError('missing actual native resource/extent dimensions')
                top,left,width,height=extent
                if width or height:
                    if not width or not height or left+width>tag['width'] or top+height>tag['height']:raise ValueError('input extent exceeds actual resource')
                else:
                    if top or left:raise ValueError('unknown partial input extent')
                    width,height=tag['width'],tag['height']
                if width>32768 or height>32768 or type(receipt['render_width']) is not int or type(receipt['render_height']) is not int or receipt['render_width']!=width or receipt['render_height']!=height or receipt['raster_relation'] is not True:raise ValueError('raster relation lacks its exact observed scaling-input extent')
                if any(abs(f)>.501 for f in c['jitter']):raise ValueError('unsupported native jitter range')
                observed_jitter=[(native['projection'][8]-c['projection'][8])*width*.5,-(native['projection'][9]-c['projection'][9])*height*.5]
                if any(abs(a-b)>1e-3 for a,b in zip(observed_jitter,c['jitter'])):raise ValueError('actual SDK jitter does not describe native raster/input extent')
                expected_native=list(c['projection']);expected_native[8]+=2*c['jitter'][0]/width;expected_native[9]-=2*c['jitter'][1]/height
                if any(abs(a-b)>3e-5 for a,b in zip(native['projection'],expected_native)) or any(abs(a-b)>1e-5 for a,b in zip(multiply(native['projection'],native['inverse_projection']),identity)) or any(abs(a-b)>1e-5 for a,b in zip(multiply(c['projection'],c['inverse_projection']),identity)):raise ValueError('native raster and jitter-free SDK matrices do not satisfy the observed jitter relation')
            for key,row in [('right',0),('up',1),('forward',2)]:
                if any(abs(native['world'][row*4+i]-c[key][i])>3e-4 for i in range(3)):raise ValueError('actual native/SDK camera basis mismatch')
        if raster_model:
            if any(abs(a-b)>1e-5 for a,b in zip(constants[0]['projection'],constants[1]['projection'])):raise ValueError('bounded symmetric SDK lenses differ')
            for key,slot in [('source_jitter',0),('right_jitter',1)]:
                if len(receipt[key])!=2 or any(type(f) not in (int,float) or not math.isfinite(f) for f in receipt[key]) or any(abs(a-b)>1e-5 for a,b in zip(receipt[key],constants[slot]['jitter'])):raise ValueError('actual submitted eye jitter differs from private receipt')
            jitter_parity=all(abs(a-b)<=1e-3 for a,b in zip(constants[0]['jitter'],constants[1]['jitter']))
            if type(receipt['jitter_parity']) is not bool or receipt['jitter_parity']!=jitter_parity:raise ValueError('unequal native eye jitter cannot be reported as parity')
        if receipt['reset']:expected=identity
        else:
            if previous is None:raise ValueError('actual accepted preceding right-eye capture required')
            pc,po=previous;prior=analyze_constants(pc,po);prior_options=analyze_options(pc,po)
            predecessors=[r for r in po['streamline']['private_context']['receipts'] if r['dispatch']==pc['dispatch']]
            if (po['model']!=observation['model'] or not prior['constants_observation_pass'] or not prior_options['options_observation_pass'] or prior['evaluation_viewports']!={0:0,1:viewport}
                    or len(predecessors)!=1 or any(predecessors[0][k]!=0 for k in ('refusal','constants_result','evaluation_result'))
                    or predecessors[0]['viewport']!=viewport or predecessors[0]['sdk_ordinal']!=prior['sdk_frame']['ordinal'] or predecessors[0]['token']!=prior['sdk_frame']['token']
                    or prior['sdk_frame']['ordinal']!=receipt['previous_ordinal'] or prior['sdk_frame']['epoch']!=sdk['epoch'] or (prior['sdk_frame']['token']+1)%2**32!=sdk['token']
                    or (pc['scenes'][1]['counter_after']+1)%2**32!=camera['scenes'][1]['counter_before']):raise ValueError('private predecessor lacks accepted consecutive actual SDK/native frame ownership')
            pw=pc['views'][1]['world'];inv=[pw[col*4+row] if row<3 and col<3 else 0.0 for row in range(4) for col in range(4)];inv[15]=1
            for col in range(3):inv[12+col]=-sum(pw[12+k]*pw[col*4+k] for k in range(3))
            prior_projection=next(e['constants']['projection'] for e in po['streamline']['events'] if e['kind']==3 and e['slot']==1)
            expected=multiply(multiply(constants[1]['inverse_projection'],multiply(camera['views'][1]['world'],inv)),prior_projection)
        if any(abs(a-b)>1e-3 for a,b in zip(right[0]['constants']['clip_to_previous'],expected)) or any(abs(a-b)>1e-3 for a,b in zip(multiply(right[0]['constants']['clip_to_previous'],right[0]['constants']['previous_to_clip']),identity)):
            raise ValueError('private previous transforms do not match its own accepted right camera/reset')
        for e in events:
            if e['kind']==0 and e['feature']==0:
                types=[t['type'] for t in e['tags']]
                if any(types.count(t)!=1 for t in (0,1,3,4)):raise ValueError('complete actual local DLSS depth/motion/input/output tags required')
        a=options['evaluation_options'][0];b=options['evaluation_options'][1]
        if any(a[k]!=b[k] for k in a if k not in ('viewport','serial')):raise ValueError('private context did not preserve actual native options')
    except (KeyError,TypeError,ValueError,IndexError) as error:errors.append(str(error))
    result.update(private_sdk_input_bundle_pass=not errors,private_errors=errors,private_receipt=receipt,
                  options_observation_pass=options['options_observation_pass'],private_contexts_verified=False,
                  native_history_ownership_verified=False,native_sdk_projection_match=not errors and not raster_model,
                  native_sdk_projection_relation_verified=not errors and raster_model,native_raster_jitter_parity=jitter_parity,
                  gpu_pair_verified=False,runtime_submission=False)
    return result


def analyze_continuity(pairs):
    """Four actual captures: first reset, then three accepted own-eye predecessors."""
    controls=[];previous=None;errors=[]
    for pair in pairs:
        controls.append(analyze_private(*pair,previous=previous));previous=pair
    if len(controls)!=4:errors.append('exactly four bounded consecutive captures required')
    for index,value in enumerate(controls):
        receipt=value.get('private_receipt') or {}
        if not value.get('private_sdk_input_bundle_pass') or receipt.get('reset') is not (index==0) or receipt.get('continued') is not (index!=0):
            errors.append(f'control {index} lacks accepted initial reset/consecutive private history')
        try:
            if index and pairs[index][0]['append']['render_id']!=pairs[index-1][0]['append']['render_id']+1:
                errors.append(f'control {index} lacks adjacent native producer jobs')
        except (KeyError,TypeError,ValueError):errors.append(f'control {index} lacks native producer identity')
    return dict(all_controls_pass=not errors,controls=controls,continuity_errors=errors,
                sdk_camera_continuity_verified=not errors,histories_verified=False,
                native_history_ownership_verified=False,gpu_pair_verified=False,runtime_submission=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);mode=parser.add_mutually_exclusive_group();mode.add_argument('--returned-frame',action='store_true',help='audit explicit actual SDK return ownership; preserve v1 strict clock audit');mode.add_argument('--constants',action='store_true',help='audit actual submitted per-eye constants before evaluation');mode.add_argument('--options',action='store_true',help='audit actual native DLSS options stable around each evaluation');mode.add_argument('--private-eye',action='store_true',help='audit bounded native-left/private-right SDK input bundle');parser.add_argument('--require-continuity',action='store_true',help='require initial private reset and three accepted consecutive own-eye predecessors');args=parser.parse_args();controls=[];previous=None;captured=[]
    if args.require_continuity and not args.private_eye:parser.error('--require-continuity requires --private-eye')
    for i in range(4):
        try:
            read=lambda name:json.loads((args.directory/name).read_text())
            audit=analyze_private if args.private_eye else analyze_options if args.options else analyze_constants if args.constants else analyze_returned if args.returned_frame else analyze
            pair=(read(f'second_{i}.json'),read(f'upscaler_{i}.json'))
            captured.append(pair);controls.append(audit(*pair,previous=previous) if args.private_eye else audit(*pair));previous=pair
        except (OSError,ValueError) as error:controls.append(dict(api_observation_pass=False,errors=[str(error)]))
    passed=all(c.get('private_sdk_input_bundle_pass' if args.private_eye else 'options_observation_pass' if args.options else 'constants_observation_pass' if args.constants else 'api_observation_pass',False) for c in controls)
    result=dict(all_controls_pass=passed,controls=controls,histories_verified=False,runtime_submission=False)
    if args.require_continuity:
        if len(captured)==4:result=analyze_continuity(captured)
        else:result.update(all_controls_pass=False,sdk_camera_continuity_verified=False,continuity_errors=['missing or unreadable capture'])
        passed=result['all_controls_pass']
    (args.directory/('private_continuity_audit.json' if args.require_continuity else 'private_eye_audit.json' if args.private_eye else 'options_audit.json' if args.options else 'constants_audit.json' if args.constants else 'sdk_frame_audit.json' if args.returned_frame else 'upscaler_audit.json')).write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));return 0 if passed else 1


if __name__=='__main__':raise SystemExit(main())
