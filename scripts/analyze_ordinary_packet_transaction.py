"""Independently audit bounded native camera writes; never GPU-history ownership."""
import argparse
import importlib.util
import json
import math
from pathlib import Path

_spec=importlib.util.spec_from_file_location('native_packet_audit',Path(__file__).resolve().with_name('analyze_ordinary_native_packet.py'))
p=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(p)
SUPPORTED=('0x34','0x3dc','0x41c','0x45c','0x49c','0x4dc','0x51c','0x55c','0x59c','0x61c','0x65c','0x6b4','0x6f4','0x734','0x774','0x7b4','0x7f4','0x834','0x8f4','0x934','0x974','0x9b4','0x9f4')
UNKNOWN=['0x5dc','0x874','0x8b4']

def expected_matrices(packet,constants,context,previous=None,reset=False):
    actual={key:p.words(value,16) for key,value in packet['matrix_words'].items()}
    world=actual['0x34'];view=actual['0x3dc'];projection=constants['projection'];jitter=p.jitter_matrix(constants['jitter'],context['width'],context['height'])
    if not p.same(p.multiply(world,view),[float(i%5==0) for i in range(16)],1e-3):raise ValueError('native world/view pair is not inverse')
    rotation=list(view);rotation[12:15]=[0.,0.,0.]
    combined=p.multiply(view,projection);raster=p.multiply(projection,jitter);rotated=p.multiply(rotation,projection)
    remap=[float(i%5==0) for i in range(16)];remap[0]=remap[5]=context['auxiliary_scale']/projection[0];remap[10]=context['depth_remap'];remap[14]=1-context['depth_remap']
    if reset:prior_projection,prior_view,prior_combined,prior_raster=projection,view,combined,p.multiply(view,raster)
    elif previous:
        prior={key:p.words(value,16) for key,value in previous['matrix_words'].items()}
        prior_projection,prior_view,prior_combined,prior_raster=(prior[key] for key in ('0x49c','0x3dc','0x59c','0x55c'))
    else:
        # First LEFT capture has no external predecessor. Its native pre-jitter
        # previous matrix is observed internally; no external history claim.
        prior_projection,prior_view=actual['0x834'],actual['0x8f4'];prior_combined=p.multiply(actual['0x934'],p.inverse(jitter));prior_raster=actual['0x974']
    motion,reciprocal=p.native_motion(combined,prior_combined)
    auxiliary=p.multiply(p.multiply(projection,remap),jitter)
    return {'0x34':world,'0x3dc':view,'0x41c':rotation,'0x45c':raster,'0x49c':projection,'0x4dc':p.inverse(raster),'0x51c':p.inverse(projection),
        '0x55c':p.multiply(view,raster),'0x59c':combined,'0x61c':p.multiply(rotated,jitter),'0x65c':rotated,'0x6b4':auxiliary,'0x6f4':p.inverse(auxiliary),
        '0x734':p.multiply(p.multiply(rotated,remap),jitter),'0x774':p.multiply(combined,remap),'0x7b4':p.multiply(rotated,remap),'0x7f4':p.multiply(p.multiply(prior_combined,remap),jitter),
        '0x834':prior_projection,'0x8f4':prior_view,'0x934':p.multiply(prior_combined,jitter),'0x974':prior_raster,'0x9b4':motion,'0x9f4':reciprocal}

def analyze(camera,observation,previous=None):
    errors=[];verified=[]
    try:
        private=p._private.analyze_private(camera,observation,previous=previous)
        if not private['private_sdk_input_bundle_pass'] or private['native_raster_jitter_parity'] is not True or not p.analyze(camera,observation,previous)['native_packet_lens_pass'] or not p.analyze_temporal(camera,observation,previous)['native_temporal_packet_pass']:
            raise ValueError('actual private SDK, matched jitter, native lens/motion and own-eye predecessor gates must pass')
        t=camera['packet_transaction'];receipt=private['private_receipt']
        if any(type(t[key]) is not int or t[key]<0 for key in ('dispatch','render_id','renderer','list','counter')):raise ValueError('exact native transaction identities required')
        if t['model']!='ordinary_native_packet_transaction_v1' or t['dispatch']!=camera['dispatch'] or t['render_id']!=camera['append']['render_id'] or t['renderer']!=camera['renderer'] or t['list']!=camera['list'] or t['counter']!=camera['scenes'][0]['counter_before']:
            raise ValueError('transaction does not join exact actual producer/dispatcher/native counter')
        for key in ('reference_pass','applied','accepted','source_unchanged'):
            if t[key] is not True:raise ValueError('missing accepted native transaction '+key)
        for key in ('rollback','rollback_failed','native_gpu_histories_verified','gpu_pair_verified','runtime_submission'):
            if t[key] is not False:raise ValueError('refused or unsupported stronger transaction claim '+key)
        if type(t['refusal']) is not int or t['refusal']!=0 or t['continued'] is not (previous is not None) or t['unsupported_offsets']!=UNKNOWN:
            raise ValueError('wrong native transaction refusal/continuity/unsupported field contract')
        if type(t['width']) is not int or type(t['height']) is not int or t['width']!=receipt['render_width'] or t['height']!=receipt['render_height']:
            raise ValueError('native render dimensions do not match actual SDK input extents')
        for key in ('auxiliary_scale','depth_remap'):
            if type(t[key]) not in (int,float) or not math.isfinite(t[key]):raise ValueError('finite actual native auxiliary context required')
        if t['auxiliary_scale']<=0 or not 0<t['depth_remap']<=1:raise ValueError('unsupported native auxiliary context')
        if t['left_source']!=camera['scenes'][0]['packet_before'] or t['patched']!=camera['scenes'][1]['packet_before'] or t['patched']!=camera['views'][1]['native_packet']:
            raise ValueError('source/patched receipt is not the actual before-dispatch and Scene packet')
        original,patched=t['original'],t['patched']
        if patched['jitter_index']!=t['left_source']['jitter_index'] or patched['jitter_words']!=t['left_source']['jitter_words']:raise ValueError('native eye jitter phase/metadata differ')
        if original['entry']!=patched['entry'] or original['model']!='native_camera_packet_v2' or original['stable_double_read'] is not True or original['resource_leases'] is not False:
            raise ValueError('exact owned original native packet required')
        for key in UNKNOWN+['0x34','0x3dc']:
            if original['matrix_words'][key]!=patched['matrix_words'][key]:raise ValueError('unsupported field or native pose changed: '+key)
        for slot in range(2):
            packet=camera['scenes'][slot]['packet_before'];constants=next(e['constants'] for e in observation['streamline']['events'] if e['kind']==3 and e['slot']==slot)
            prior=previous[0]['scenes'][slot]['packet_before'] if previous else None
            expected=expected_matrices(packet,constants,t,prior,reset=slot==1 and previous is None)
            for key in SUPPORTED:
                if not p.same(p.words(packet['matrix_words'][key],16),expected[key],1e-3 if key in ('0x9b4','0x9f4') else 3e-4):raise ValueError(f'eye {slot} native field {key} does not match independent matrix reconstruction')
            if slot==1 and previous is None:
                if packet['motion_flags']!=[0,1] or not p.same(p.words(packet['motion_words'],6),[0.]*6,1e-5):raise ValueError('first right native packet lacks coherent reset motion metadata')
            verified.append(slot)
    except (KeyError,TypeError,ValueError,IndexError,StopIteration) as error:errors.append(str(error))
    return dict(native_transaction_pass=not errors,verified_eye_slots=verified,errors=errors,
        all_dependent_matrices_verified=False,angular_moving_parity_verified=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args();controls=[];previous=None
    for index in range(4):
        try:
            pair=tuple(json.loads((args.directory/f'{name}_{index}.json').read_text()) for name in ('second','upscaler'))
            controls.append(analyze(*pair,previous=previous));previous=pair
        except (OSError,ValueError) as error:controls.append(dict(native_transaction_pass=False,errors=[str(error)]))
    result=dict(all_controls_pass=all(c['native_transaction_pass'] for c in controls),controls=controls,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)
    (args.directory/'native_transaction_audit.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n');print(json.dumps(result,indent=2,allow_nan=False));return 0 if result['all_controls_pass'] else 1

if __name__=='__main__':raise SystemExit(main())
