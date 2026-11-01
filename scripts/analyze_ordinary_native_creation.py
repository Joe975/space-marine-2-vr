"""Audit fresh bounded native/COM creation ownership; no native pass/history use."""
import argparse
import importlib.util
import json
from pathlib import Path
import re

_spec=importlib.util.spec_from_file_location('texture_contract',Path(__file__).resolve().with_name('analyze_ordinary_texture_contract.py'))
contract_audit=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(contract_audit)
uint=contract_audit.temporal_audit.uint


def analyze(camera,sdk,temporal,creation,first=None,previous=None):
    errors=[]
    try:
        if not contract_audit.analyze(camera,sdk,temporal,previous)['native_texture_contract_observation_pass']:raise ValueError('accepted actual native camera/SDK/texture observation required')
        if creation['model']!='ordinary_native_texture_creation_v1' or any(creation[k] is not True for k in ('armed','attempted','instruction_guards','creation_pair_verified')) or type(creation['refusal']) is not int or creation['refusal']!=0:raise ValueError('native creation missing, unguarded or refused')
        if creation['retirement']!='process_lifetime' or any(creation[k] is not False for k in ('used_in_native_pass','native_gpu_histories_verified','gpu_pair_verified','runtime_submission')):raise ValueError('unsupported native history/use/retirement/runtime claim')
        if type(creation['dispatch']) is not int or creation['dispatch']!=camera['dispatch']:raise ValueError('native creation report belongs to another observed dispatch')
        initial=(camera,sdk,temporal,creation) if first is None else first
        first_camera,_,first_temporal,first_creation=initial
        event=next(e for e in first_temporal['events'] if e['slot']==1 and e['mode']==2)
        pair=first_camera['ordinary_pair'];expected=dict(creation_dispatch=first_camera['dispatch'],creation_thread=event['thread'],render_id=pair['render_id'],revision=pair['revision'],token=pair['token'],counter=event['before']['counter'],slot=1,entry=event['entry'],list=first_camera['list'],renderer=first_camera['renderer'])
        if any(type(creation[k]) is not int or creation[k]!=value for k,value in expected.items()):raise ValueError('fresh native allocation does not join the initial actual producing right-eye call')
        if not uint(creation['device_identity']) or not creation['device_identity']:raise ValueError('native creation lacks actual device identity')
        native=event['texture_contract_before'];base=native['module_base'];wrapper=native['wrappers'][1]
        if native['factory_create']-base!=0x2119040 or wrapper['vtable']-base!=0x37a4b18 or [address-base for address in wrapper['virtual_18_20_b0_c0']]!=[0x264ab70,0x264ab80,0x2113cc0,0x264b7b0]:raise ValueError('native creation input implementation differs from guarded shipping methods')
        rows=[target['initialization_words'] for target in native['targets']];targets=native['targets'];variant=native['bank_variant']
        if type(variant) is not int or not 0<=variant<30 or type(creation['creation_variant']) is not int or creation['creation_variant']!=variant:raise ValueError('native creation lacks the actual initial active resolution variant')
        if any(not 1<=len(table)<=30 or any(row[2:]!=[1,1,38,1] or not 0<row[0]<=4096 or not 0<row[1]<=2160 for row in table) for table in rows) or rows[0]!=rows[1] or any(target['definition'][0]!=0x20000b0 or target['no_uav'] or len(target['wrappers'])!=len(target['initialization_words']) for target in targets):raise ValueError('exact bounded native bloom creation definitions/initializers required')
        if targets[0]['variants']!=targets[1]['variants'] or targets[0]['variants'] not in (0,1) or (not targets[0]['variants'] and len(rows[0])!=1):raise ValueError('native resolution variant tables differ')
        selected_row=variant if targets[0]['variants'] else 0
        if selected_row>=len(rows[0]) or type(creation['initializer_row']) is not int or creation['initializer_row']!=selected_row or targets[event['before']['counter']&1]['wrappers'][selected_row]!=event['arguments'][2]:raise ValueError('creation initializer does not join active pool variant and actual destination')
        dimensions=rows[0][selected_row][:2];textures=creation['textures']
        if len(textures)!=2:raise ValueError('exactly two bounded right-eye native allocations required')
        borrowed_native={w['address'] for e in first_temporal['events'] for w in e['texture_contract_before']['wrappers'] if w['address']}
        borrowed_resources={o['resource'] for e in first_temporal['events'] for o in e['outputs_before'] if o['resource']}
        borrowed_identities={v['identity'] for e in first_temporal['events'] for o in e['outputs_before'] for v in o['rtvs'] if v['identity']}
        for index,texture in enumerate(textures):
            if not isinstance(texture['name'],str) or not re.fullmatch(r'SM2VR_ORDINARY_RIGHT_BLOOM_\d+_\d+_'+str(index),texture['name']) or any(texture[k] is not True for k in ('native_reference','initialized','resource_lease')):raise ValueError('fresh native name/reference/initialization/COM lease missing')
            if any(not uint(texture[k]) or not texture[k] for k in ('native','resource','identity')) or texture['native'] in borrowed_native or texture['resource'] in borrowed_resources or texture['identity'] in borrowed_identities:raise ValueError('borrowed native/GPU source substituted for fresh creation ownership')
            if type(texture['refs_before']) is not int or not 0<=texture['refs_before']<2**31-1 or type(texture['refs_after']) is not int or texture['refs_after']!=texture['refs_before']+1:raise ValueError('native reference increment not observed')
            allocation=texture['allocation']
            numeric=('api','initial_state','thread','serial','device','resource','kind','heap','heap_flags','offset','size','heap_size','alignment','creation_begin_ns','callsite','actual_heap_flags','width','height','format','flags','arrays','mips','samples')
            if not all(uint(allocation[k]) for k in numeric) or allocation['properties_known'] is not True or allocation['api'] not in (0,2) or allocation['kind']!=1 or allocation['heap'] or allocation['offset'] or (allocation['heap_flags']|allocation['actual_heap_flags'])&0x21:raise ValueError('actual private committed allocation evidence required')
            if allocation['thread']!=creation['creation_thread'] or allocation['device']!=creation['device_identity'] or allocation['resource']!=texture['identity'] or not all(allocation[k] for k in ('serial','creation_begin_ns','callsite','size','alignment')):raise ValueError('actual fresh resource allocation attribution differs')
            alignment=allocation['alignment']
            if alignment&(alignment-1) or allocation['size']%alignment or allocation['heap_size']!=allocation['size'] or [allocation['width'],allocation['height']]!=dimensions or allocation['format']!=9 or allocation['flags']!=1 or any(allocation[k]!=1 for k in ('arrays','mips','samples')):raise ValueError('actual allocation range/descriptor differs from native creation inputs')
        for key in ('name','native','resource','identity'):
            if textures[0][key]==textures[1][key]:raise ValueError('right-eye ping-pong allocations alias')
        if textures[0]['allocation']['serial']==textures[1]['allocation']['serial']:raise ValueError('two native creations reused one actual allocation event')
        if first is not None:
            if textures!=first_creation['textures'] or creation['device_identity']!=first_creation['device_identity'] or creation['creation_variant']!=first_creation['creation_variant'] or creation['initializer_row']!=first_creation['initializer_row']:raise ValueError('bounded native allocations/leases/origin changed between controls')
    except (KeyError,TypeError,ValueError,IndexError,StopIteration) as error:errors.append(str(error))
    return dict(native_creation_pair_pass=not errors,errors=errors,used_in_native_pass=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False,retirement_verified=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args();controls=[];first=None;previous=None
    for index in range(4):
        try:
            observed=tuple(json.loads((args.directory/f'{name}_{index}.json').read_text()) for name in ('second','upscaler','temporal','creation'))
            controls.append(analyze(*observed,first=first,previous=previous));first=first or observed;previous=observed[:3]
        except (OSError,ValueError) as error:controls.append(dict(native_creation_pair_pass=False,errors=[str(error)]))
    result=dict(all_controls_pass=all(control['native_creation_pair_pass'] for control in controls),controls=controls,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False,retirement_verified=False)
    (args.directory/'native_creation_audit.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n');print(json.dumps(result,indent=2,allow_nan=False));return 0 if result['all_controls_pass'] else 1


if __name__=='__main__':raise SystemExit(main())
