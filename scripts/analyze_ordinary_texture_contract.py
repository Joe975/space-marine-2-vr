"""Join borrowed native factory/pool/wrapper POD to actual temporal calls."""
import argparse
import importlib.util
import json
from pathlib import Path

_spec=importlib.util.spec_from_file_location('temporal_calls',Path(__file__).resolve().with_name('analyze_ordinary_temporal.py'))
temporal_audit=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(temporal_audit)
TARGETS={0:[0x16,0x17],1:[0x29,0x2a],2:[0xc4,0xc5]}


def analyze(camera,sdk,temporal,previous=None):
    errors=[];inventory=[]
    try:
        observed=temporal_audit.analyze(camera,sdk,temporal,previous)
        if not observed['native_temporal_observation_pass']:raise ValueError('accepted actual camera/SDK/temporal observation required')
        for event in temporal['events']:
            before=event['texture_contract_before'];after=event['texture_contract_after']
            if before!=after or before['model']!='native_texture_contract_v1' or before['complete'] is not True or before['read_only'] is not True or before['native_calls'] is not False or before['resource_leases'] is not False:raise ValueError('native texture contract missing, incomplete, changed or claims native calls/leases')
            for field in ('module_base','factory','factory_vtable','factory_create','pool','banks','records'):
                if not temporal_audit.uint(before[field]) or not before[field]:raise ValueError('unreadable native factory/pool context')
            base=before['module_base'];mode=event['mode'];ids=TARGETS[mode];counter=event['before']['counter'];selected=counter&1
            if type(before['pool_mode']) is not int or before['pool_mode'] not in (0,1,2) or type(before['bank_count']) is not int or not 1<=before['bank_count']<=256 or type(before['bank']) is not int or not 0<=before['bank']<before['bank_count'] or type(before['record_count']) is not int or not 1<=before['record_count']<=4096:raise ValueError('unbounded/unknown native pool bank or record')
            if len(before['bank_dimensions'])!=2 or not all(type(v) is int and 0<v<=32768 for v in before['bank_dimensions']) or before['selected_id']!=ids[selected]:raise ValueError('native bank dimensions or counter-selected target differs')
            targets=before['targets']
            if len(targets)!=2 or [target['id'] for target in targets]!=ids:raise ValueError('exact native even/odd target pair required')
            for target in targets:
                if target['readable'] is not True or len(target['definition'])!=5 or not all(temporal_audit.uint(word) for word in target['definition']) or type(target['record']) is not int or not 0<=target['record']<before['record_count']:raise ValueError('native target definition/record unreadable')
                rows=target['initialization_words'];wrappers=target['wrappers']
                if type(target['no_uav']) is not int or target['no_uav'] not in (0,1) or type(target['variants']) is not int or target['variants'] not in (0,1) or not 1<=len(rows)<=30 or len(wrappers)!=len(rows)*(target['no_uav']+1) or not all(temporal_audit.uint(pointer) and pointer for pointer in wrappers):raise ValueError('native initializer row/wrapper bounds or multiplicity differ')
                for row in rows:
                    if len(row)!=6 or not all(type(word) is int and 0<=word<2**32 for word in row):raise ValueError('exact six-word native initializer rows required')
            actual=[event['arguments'][i] for i in (1,2,3,5)]+event['before']['descriptor_inputs']
            native_wrappers=before['wrappers']
            if len(native_wrappers)!=9:raise ValueError('all nine borrowed native wrapper contracts required')
            for wrapper,pointer in zip(native_wrappers,actual):
                if wrapper['address']!=pointer or type(wrapper['address']) is not int or type(wrapper['readable']) is not bool or len(wrapper['virtual_18_20_b0_c0'])!=4 or not temporal_audit.uint(wrapper['flags']):raise ValueError('borrowed native wrapper contract does not join actual call inputs')
                if not pointer:
                    if wrapper['readable'] or wrapper['vtable'] or wrapper['flags'] or any(wrapper['virtual_18_20_b0_c0']):raise ValueError('null wrapper acquired a native creation contract')
                elif not wrapper['readable'] or not temporal_audit.uint(wrapper['vtable']) or not wrapper['vtable'] or not all(temporal_audit.uint(method) and method for method in wrapper['virtual_18_20_b0_c0']):raise ValueError('native wrapper vtable/initializer incomplete')
            target=targets[selected];destination=event['arguments'][2]
            indices=[i for i,pointer in enumerate(target['wrappers']) if pointer==destination]
            if len(indices)!=1:raise ValueError('actual native temporal destination does not join exactly one selected pool wrapper')
            if 'bank_variant' in before:
                variant=before['bank_variant']
                if type(variant) is not int or not 0<=variant<30:raise ValueError('native active resolution variant is unbounded')
                expected=(variant if target['variants'] else 0)*(target['no_uav']+1)+target['no_uav']
                if indices[0]!=expected:raise ValueError('actual destination differs from native active bank resolution variant')
            if event['arguments'][3] and event['arguments'][3] not in targets[1-selected]['wrappers']:raise ValueError('actual preceding texture does not join the opposite native parity target')
            row=target['initialization_words'][indices[0]//(target['no_uav']+1)]
            descriptors=event['outputs_after'][1]['rtvs']
            if not descriptors or any(not view['known'] or (view['width'],view['height'])!=tuple(row[:2]) for view in descriptors):raise ValueError('actual CreateRTV dimensions do not corroborate selected native initializer words')
            methods=native_wrappers[1]['virtual_18_20_b0_c0']
            for address in [before['factory_vtable'],before['factory_create'],native_wrappers[1]['vtable']]+methods:
                if not base<=address<base+0x7000000:raise ValueError('observed native factory/wrapper methods lie outside the inspected executable image range')
            inventory.append(dict(slot=event['slot'],mode=mode,selected_id=before['selected_id'],pool_mode=before['pool_mode'],bank=before['bank'],bank_dimensions=before['bank_dimensions'],record=target['record'],definition=target['definition'],destination_flags=native_wrappers[1]['flags'],
                initializer_words=row,initializer_arguments=[row[0],row[1],row[2],row[4],1,row[3]],factory_create_rva=before['factory_create']-base,destination_vtable_rva=native_wrappers[1]['vtable']-base,virtual_18_20_b0_c0_rvas=[method-base for method in methods]))
    except (KeyError,TypeError,ValueError,IndexError) as error:errors.append(str(error))
    return dict(native_texture_contract_observation_pass=not errors,errors=errors,inventory=inventory,native_allocation_contract_verified=False,native_allocations=False,resource_leases=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args();controls=[];previous=None
    for index in range(4):
        try:
            observed=tuple(json.loads((args.directory/f'{name}_{index}.json').read_text()) for name in ('second','upscaler','temporal'))
            controls.append(analyze(*observed,previous=previous));previous=observed
        except (OSError,ValueError) as error:controls.append(dict(native_texture_contract_observation_pass=False,errors=[str(error)]))
    result=dict(all_controls_pass=all(control['native_texture_contract_observation_pass'] for control in controls),controls=controls,native_allocation_contract_verified=False,native_allocations=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)
    (args.directory/'native_texture_contract_audit.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n');print(json.dumps(result,indent=2,allow_nan=False));return 0 if result['all_controls_pass'] else 1


if __name__=='__main__':raise SystemExit(main())
