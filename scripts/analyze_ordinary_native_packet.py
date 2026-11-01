"""Independent native packet/current/unjittered lens audit; never GPU ownership."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import struct

_spec=importlib.util.spec_from_file_location('ordinary_private_audit',Path(__file__).resolve().with_name('analyze_ordinary_upscaler.py'))
_private=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(_private)
OFFSETS=('0x34','0x3dc','0x45c','0x49c','0x4dc','0x51c','0x55c','0x59c','0x61c','0x65c','0x6b4','0x6f4','0x734','0x7b4','0x7f4')
TEMPORAL_OFFSETS=OFFSETS+('0x41c','0x5dc','0x774','0x834','0x874','0x8b4','0x8f4','0x934','0x974','0x9b4','0x9f4')

def words(values,size):
    if not isinstance(values,list) or len(values)!=size or any(type(n) is not int or not 0<=n<2**32 for n in values):raise ValueError('invalid exact native packet words')
    return list(struct.unpack('<'+'f'*size,struct.pack('<'+'I'*size,*values)))

def multiply(a,b):return [sum(a[row*4+k]*b[k*4+col] for k in range(4)) for row in range(4) for col in range(4)]

def same(a,b,tolerance=3e-5):return len(a)==len(b) and all(math.isfinite(x) and math.isfinite(y) and abs(x-y)<=tolerance for x,y in zip(a,b))

def transpose(a):return [a[col*4+row] for row in range(4) for col in range(4)]

def depth_free(a):
    # Shipping helper 0x1f8f740 keeps x,y,w and replaces the last column by
    # affine identity. Its output is not a normal depth projection inverse.
    if len(a)!=16 or not all(map(math.isfinite,a)):raise ValueError('finite native combined matrix required')
    return [a[row*4+col] if col<2 else a[row*4+3] if col==2 else float(row==3) for row in range(4) for col in range(4)]

def inverse(a):
    if len(a)!=16 or not all(map(math.isfinite,a)):raise ValueError('finite native inverse input required')
    rows=[a[row*4:row*4+4]+[float(row==col) for col in range(4)] for row in range(4)]
    for col in range(4):
        pivot=max(range(col,4),key=lambda row:abs(rows[row][col]))
        if abs(rows[pivot][col])<1e-9:raise ValueError('singular native motion matrix')
        rows[col],rows[pivot]=rows[pivot],rows[col];scale=rows[col][col];rows[col]=[v/scale for v in rows[col]]
        for row in range(4):
            if row!=col:
                scale=rows[row][col];rows[row]=[v-scale*p for v,p in zip(rows[row],rows[col])]
    return [v for row in rows for v in row[4:]]

def native_motion(current,previous):
    current=depth_free(current);previous=depth_free(previous)
    return transpose(multiply(inverse(current),previous)),transpose(multiply(inverse(previous),current))

def jitter_matrix(jitter,width,height):
    if len(jitter)!=2 or not all(map(math.isfinite,jitter)) or any(abs(v)>.501 for v in jitter) or type(width) is not int or type(height) is not int or width<=0 or height<=0:
        raise ValueError('actual finite native jitter and SDK render extent required')
    result=[float(i%5==0) for i in range(16)]
    result[12]=2*jitter[0]/width;result[13]=-2*jitter[1]/height
    return result

def analyze_temporal(camera,observation,previous=None):
    private=_private.analyze_private(camera,observation,previous=previous);eyes=[]
    identity=[float(i%5==0) for i in range(16)]
    for slot in range(2):
        errors=[];linked=False;motion_pass=False
        try:
            if not private['private_sdk_input_bundle_pass']:raise ValueError('accepted actual private SDK camera inputs required')
            scene=camera['scenes'][slot];packet=scene['packet_before'];after=scene['packet_after']
            if (scene['refusal']!=0 or packet!=after or packet['model']!='native_camera_packet_v2' or packet['entry']!=scene['entry']
                    or packet['stable_double_read'] is not True or packet['resource_leases'] is not False or set(packet['matrix_words'])!=set(TEMPORAL_OFFSETS)):
                raise ValueError('exact stable ordinary Scene packet before/after required')
            matrices={key:words(packet['matrix_words'][key],16) for key in TEMPORAL_OFFSETS}
            if len(packet['motion_flags'])!=2 or any(type(n) is not int or n not in (0,1) for n in packet['motion_flags']):raise ValueError('invalid native motion/cut flags')
            metadata=words(packet['motion_words'],6)
            if not all(map(math.isfinite,metadata)):raise ValueError('nonfinite native motion metadata')
            constants=next(e['constants'] for e in observation['streamline']['events'] if e['kind']==3 and e['slot']==slot)
            if not same(matrices['0x49c'],constants['projection']) or not same(matrices['0x51c'],constants['inverse_projection']):raise ValueError('Scene unjittered camera lens is not the actual SDK lens')
            # Main stores the unjittered predecessor before jitter generation,
            # computes shader motion from it, then 0x18044d2..0x1804531 applies
            # THIS frame's raster jitter to +0x934. Undo that known late step
            # before comparing the predecessor or rebuilding shader motion.
            receipt=private['private_receipt'];jitter=words(packet['jitter_words'],2)
            if not same(jitter,constants['jitter'],1e-3):raise ValueError('Scene native jitter differs from actual SDK camera')
            raster_jitter=jitter_matrix(jitter,receipt['render_width'],receipt['render_height'])
            unjittered_previous=multiply(matrices['0x934'],inverse(raster_jitter))
            expected,back=native_motion(matrices['0x59c'],unjittered_previous)
            if not same(matrices['0x9b4'],expected,1e-3) or not same(matrices['0x9f4'],back,1e-3):raise ValueError('native shader motion transform differs from depth-free current/previous camera')
            if not same(multiply(transpose(matrices['0x9b4']),transpose(matrices['0x9f4'])),identity,1e-3):raise ValueError('native shader motion matrices are not inverse transforms')
            motion_pass=True
            if previous is not None:
                pc,po=previous;ps=pc['scenes'][slot];prior=ps['packet_before']
                sdk=private['sdk_frame'];p_sdk=_private.analyze_returned(pc,po)
                if (not p_sdk['api_observation_pass'] or p_sdk['sdk_frame']['epoch']!=sdk['epoch'] or p_sdk['sdk_frame']['ordinal']+1!=sdk['ordinal']
                        or (p_sdk['sdk_frame']['token']+1)%2**32!=sdk['token'] or pc['append']['render_id']+1!=camera['append']['render_id']
                        or pc['renderer']!=camera['renderer'] or pc['dispatch']>=camera['dispatch'] or ps['refusal']!=0 or prior!=ps['packet_after']
                        or prior['model']!='native_camera_packet_v2' or (ps['counter_after']+1)%2**32!=scene['counter_before']):raise ValueError('native previous packet lacks actual consecutive SDK/producer/renderer/Scene ownership')
                p={key:words(prior['matrix_words'][key],16) for key in TEMPORAL_OFFSETS}
                for dest,source in [('0x834','0x49c'),('0x8f4','0x3dc'),('0x974','0x55c')]:
                    if not same(matrices[dest],p[source],3e-4):raise ValueError(f'native previous {dest} does not match preceding same-eye {source}')
                if not same(matrices['0x934'],multiply(p['0x59c'],raster_jitter),3e-4):raise ValueError('native previous 0x934 does not match preceding same-eye 0x59c with current raster jitter')
                delta=[matrices['0x34'][12+i]-p['0x34'][12+i] for i in range(3)]
                if not same(metadata[:3],delta,3e-4):raise ValueError('native translation delta is not from preceding same-eye camera')
                linked=True
        except (KeyError,TypeError,ValueError,IndexError,StopIteration) as error:errors.append(str(error))
        eyes.append(dict(slot=slot,native_motion_pass=motion_pass,previous_packet_available=previous is not None,same_eye_previous_links_pass=linked,errors=errors))
    return dict(model='ordinary_native_temporal_audit_v2',native_temporal_packet_pass=all(not eye['errors'] for eye in eyes),eyes=eyes,
                all_dependent_matrices_verified=False,angular_metadata_verified=False,
                native_history_ownership_verified=False,gpu_pair_verified=False,runtime_submission=False)

def analyze(camera,observation,previous=None):
    private=_private.analyze_private(camera,observation,previous=previous);errors=[];inventory=[]
    if not private['private_sdk_input_bundle_pass']:errors.append('accepted actual private SDK camera inputs required')
    identity=[float(i%5==0) for i in range(16)]
    for slot in range(2):
        try:
            view=camera['views'][slot];packet=view['native_packet']
            expected=OFFSETS if packet['model']=='native_camera_packet_v1' else TEMPORAL_OFFSETS if packet['model']=='native_camera_packet_v2' else ()
            if (not expected or packet['entry']!=camera['ordinary_pair'][('left_entry','right_entry')[slot]] or packet['stable_double_read'] is not True or packet['resource_leases'] is not False
                    or type(packet['jitter_index']) is not int or not 0<=packet['jitter_index']<2**32 or set(packet['matrix_words'])!=set(expected)):raise ValueError('native packet lacks exact stable native entry identity')
            matrices={offset:words(packet['matrix_words'][offset],16) for offset in expected};jitter=words(packet['jitter_words'],2)
            inventory.append(dict(slot=slot,jitter_index=packet['jitter_index'],jitter=jitter if all(map(math.isfinite,jitter)) else None,
                                  finite_matrices=[offset for offset,m in matrices.items() if all(map(math.isfinite,m))]))
            constants=[e['constants'] for e in observation['streamline']['events'] if e['kind']==3 and e['slot']==slot]
            if len(constants)!=1:raise ValueError('one actual submitted SDK camera required')
            c=constants[0]
            for offset,key in [('0x34','world'),('0x3dc','view'),('0x45c','projection'),('0x4dc','inverse_projection')]:
                if not same(matrices[offset],view[key],1e-5):raise ValueError(f'packet {offset} does not join actual native {key}')
            if not same(jitter,c['jitter'],1e-3):raise ValueError('native pixel jitter metadata differs from actual SDK camera')
            if not same(matrices['0x49c'],c['projection']) or not same(matrices['0x51c'],c['inverse_projection']):raise ValueError('native unjittered lens copies differ from actual SDK camera')
            if not same(multiply(matrices['0x49c'],matrices['0x51c']),identity,1e-5):raise ValueError('native unjittered lens copies are not inverses')
            if not same(multiply(matrices['0x3dc'],matrices['0x45c']),matrices['0x55c'],3e-4):raise ValueError('native current combined matrix does not match view and raster lens')
            if not same(multiply(matrices['0x3dc'],matrices['0x49c']),matrices['0x59c'],3e-4):raise ValueError('native unjittered combined matrix does not match view and SDK lens')
        except (KeyError,ValueError,TypeError,IndexError) as error:errors.append(f'eye {slot}: {error}')
    return dict(native_packet_lens_pass=not errors,errors=errors,inventory=inventory,
                all_dependent_matrices_verified=False,previous_native_camera_verified=False,
                native_history_ownership_verified=False,gpu_pair_verified=False,runtime_submission=False)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);parser.add_argument('--temporal',action='store_true');args=parser.parse_args();controls=[];previous=None
    for i in range(4):
        try:
            pair=tuple(json.loads((args.directory/f'{name}_{i}.json').read_text()) for name in ('second','upscaler'))
            controls.append((analyze_temporal if args.temporal else analyze)(*pair,previous=previous));previous=pair
        except (OSError,ValueError) as error:controls.append(dict(native_temporal_packet_pass=False,native_packet_lens_pass=False,errors=[str(error)]))
    result=dict(all_controls_pass=all(c['native_temporal_packet_pass' if args.temporal else 'native_packet_lens_pass'] for c in controls),controls=controls,gpu_pair_verified=False,runtime_submission=False)
    if args.temporal:result['left_reference_pass']=all(c.get('eyes') and not c['eyes'][0]['errors'] for c in controls)
    (args.directory/('native_temporal_packet_audit.json' if args.temporal else 'native_packet_audit.json')).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n');print(json.dumps(result,indent=2,allow_nan=False));return 0 if result['all_controls_pass'] else 1

if __name__=='__main__':raise SystemExit(main())
