"""Audit actual owned right-bloom use, distinct from observation-only v1 gates."""
import argparse
import importlib.util
import json
from pathlib import Path

_spec=importlib.util.spec_from_file_location('owned_bloom_pool',Path(__file__).resolve().with_name('analyze_ordinary_pool_membership.py'))
pool_audit=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(pool_audit)
temporal_audit=pool_audit.creation_audit.contract_audit.temporal_audit
uint=temporal_audit.uint
SHAPE=('model','read_only','native_calls','resource_leases','complete','module_base','factory','factory_vtable','factory_create','pool','pool_mode','banks','bank','bank_count','bank_dimensions','bank_variant','records','record_count','targets')


def check(condition,message):
    if not condition:raise ValueError(message)


def same_shape(a,b):
    # Unrelated native targets may be appended between jobs. The backing array,
    # selected records, definitions and wrappers must still match exactly.
    return (type(a['record_count']) is int and type(b['record_count']) is int and
            0 < b['record_count'] <= a['record_count'] <= 4096 and
            all(a[key]==b[key] for key in SHAPE if key!='record_count'))


def banks(camera,previous):
    renderer=camera['renderer']
    check(uint(renderer) and 0<renderer<2**64-0x2b0,'bounded persistent renderer required')
    for slot,scene in enumerate(camera['scenes']):
        before=scene['persistent_before'];after=scene['persistent_after'];address=renderer+0x200+slot*0x58
        for bank in (before,after):check(type(bank['address']) is int and bank['address']==address and len(bank['words'])==11 and all(uint(w) for w in bank['words']),'exact persistent bank address/all eleven words required')
        check(before['words']==scene['temporal_before']['previous_words'] and after['words']==scene['temporal_after']['next_words'],'native bank load/export differs')
        if previous:check(before==previous[0]['scenes'][slot]['persistent_after'],'same-slot preceding native export required')
        elif slot:check(not any(before['words']),'bootstrap right bank must be empty')


def sdk_frame(sdk):
    values=[e['returned_frame'] for e in sdk['streamline']['events'] if e['slot']==1 and e['kind']==0 and e['feature']==0]
    check(len(values)==1 and values[0] is not None,'exact accepted right SDK evaluation required')
    return {key:values[0][key] for key in ('epoch','ordinal','token')}


def analyze(camera,sdk,temporal,creation,bloom,first=None,previous=None):
    errors=[];reset=False;private=False
    try:
        check(temporal_audit.camera_audit.analyze(camera,sdk,previous[:2] if previous else None)['native_transaction_pass'],'actual accepted native camera/SDK transaction required')
        check(bloom['model']=='ordinary_owned_bloom_use_v1' and bloom['installed'] is True and bloom['instruction_guards'] is True and type(bloom['dispatch']) is int and bloom['dispatch']==camera['dispatch'],'guarded actual owned-bloom receipt required')
        for report in (bloom,creation,temporal):
            for key in ('native_gpu_histories_verified','gpu_pair_verified','runtime_submission'):check(report[key] is False,'unsupported full native history/GPU pair/runtime claim')
        check(bloom['retirement']=='process_lifetime','owned native references must remain pinned')
        record=bloom['record'];pair=camera['ordinary_pair'];right=camera['scenes'][1]
        expected=dict(render_id=pair['render_id'],revision=pair['revision'],token=pair['token'],renderer=camera['renderer'],list=camera['list'],entry=right['entry'],slot=1,scene_ordinal=2,counter=right['counter_before'],selector_caller=0x180a6e1,colour_caller=0x180ae76,refusal=0,temporal_calls=1,colour_calls=1,tone_calls=1)
        check(all(type(record[k]) is int and record[k]==v for k,v in expected.items()),'owned use does not join exact producing Scene/selector/colour call')
        check(uint(record['thread']) and record['thread'] and all(record[k] is True for k in ('temporal','colour','tone','accepted')),'filter/colour/tone/completion acceptance required')
        check(record['selector_arguments']==[camera['renderer'],0xc4,0xc5],'exact native bloom selector arguments required')
        scope=record['scope_evidence'];flags=scope['view_flags'];ordinary=camera['views'][1]['flags']
        check(scope['model']=='ordinary_scene_view_inventory_v1' and type(scope['mask']) is int and scope['mask']==0 and scope['reads']==15 and type(scope['count']) is int and 2<=scope['count']<=64 and len(flags)==scope['count'] and all(uint(v) for v in flags),'complete stable bounded native view inventory required')
        check(scope['active']==right['entry'] and scope['counter']==right['counter_before'] and scope['flags']==ordinary==camera['views'][0]['flags'] and flags[:2]==[ordinary,ordinary] and not ordinary&0x50000044 and all(v&0x50000044 and not v&0x40 for v in flags[2:]),'exactly two ordinary native slots and excluded non-PIP auxiliary slots required')
        check(record['sdk']==sdk_frame(sdk),'actual returned SDK frame does not join accepted native use')
        banks(camera,previous)
        if first is None:
            check(previous is None and pool_audit.analyze(camera,sdk,temporal,creation)['native_pool_membership_pass'],'unchanged bootstrap must pass original strict observation/creation/pool audit')
            check(record['bootstrap'] is True and record['selected'] is False and record['reset'] is False and record['previous_dispatch']==0,'bootstrap cannot claim private history use')
            event=next(e for e in temporal['events'] if e['slot']==1 and e['mode']==2)
            check(record['destination']==event['arguments'][2] and record['resource']==event['outputs_after'][1]['resource'] and record['observed_previous']==event['arguments'][3] and record['invoked_previous']==event['arguments'][3],'bootstrap must preserve actual native invocation')
        else:
            check(previous is not None,'owned use requires its accepted bootstrap/predecessor')
            check(temporal['model']=='ordinary_native_temporal_calls_v2' and temporal['installed'] is True and temporal['cpu_only'] is True and temporal['output_labels']==temporal_audit.OUTPUT_LABELS and all(type(temporal[k]) is int and temporal[k]==0 for k in ('refused','errors','dropped')) and temporal['resource_leases'] is False,'explicit lossless v2 observed/invoked temporal evidence required')
            check(record['bootstrap'] is False and record['selected'] is True,'later job must actually select an owned native destination')
            check(camera['dispatch']==previous[0]['dispatch']+1 and pair['render_id']==previous[0]['ordinary_pair']['render_id']+1 and record['counter']==(previous[4]['record']['counter']+1)&0xffffffff and record['previous_dispatch']==previous[0]['dispatch'],'consecutive native camera job/dispatch/counter required')
            earlier_sdk=previous[4]['record']['sdk'];check(record['sdk']['epoch']==earlier_sdk['epoch'] and record['sdk']['ordinal']==earlier_sdk['ordinal']+1 and record['sdk']['token']==(earlier_sdk['token']+1)&0xffffffff,'accepted private SDK generation/ordinal/token must continue')
            check(record['thread']==first[4]['record']['thread'] and camera['renderer']==first[0]['renderer'],'owned origin renderer/thread changed')
            check(creation['model']=='ordinary_native_texture_creation_v2' and creation['used_in_native_pass'] is True and creation['first_use_dispatch']==first[0]['dispatch']+1 and record['creation_dispatch']==first[0]['dispatch'],'actual owned invocation must have a distinct used-creation receipt')
            dynamic={'model','dispatch','used_in_native_pass','first_use_dispatch'}
            check({k:v for k,v in creation.items() if k not in dynamic}=={k:v for k,v in first[3].items() if k not in dynamic},'immutable owned creation/leases/pool proof changed')
            textures=creation['textures'];parity=record['counter']&1;owned=textures[parity]
            for key in ('destination','resource','identity'):check(type(record[key]) is int and record[key]==owned['native' if key=='destination' else key],'selected owned parity/backing differs from actual creation')
            initial_event=next(e for e in first[2]['events'] if e['slot']==1 and e['mode']==2);initial_contract=initial_event['texture_contract_before'];native=record['contract']
            check(same_shape(native,initial_contract) and native['selected_id']==0xc4+parity,'fresh native pool/variant/definitions/dimensions changed')
            row=creation['initializer_row'];native_target=native['targets'][parity];borrowed=native_target['wrappers'][row]
            check(native['wrappers'][1]['address']==borrowed,'positive native selected-wrapper control missing')
            pool_audit.lookup(record['source_membership'],native['pool'],borrowed,native['record_count'],True,native_target['record'])
            check(len(record['owned_membership'])==2,'fresh absence proofs required for both owned wrappers')
            for lookup,texture in zip(record['owned_membership'],textures):pool_audit.lookup(lookup,native['pool'],texture['native'],native['record_count'],False)
            check(len(temporal['events'])==2 and [(e['slot'],e['mode']) for e in temporal['events']]==[(0,2),(1,2)],'only measured mode2 pair covered by this bounded bloom gate')
            serial=0
            for slot,event in enumerate(temporal['events']):
                scene=camera['scenes'][slot];args=event['arguments'];invoked=event['invoked_arguments'];before=event['before']
                check(type(event['serial']) is int and event['serial']>serial and event['refusal']==0 and type(event['refusal']) is int and event['caller']==0x180a711,'exact ordered native temporal caller required');serial=event['serial']
                origin=dict(dispatch=camera['dispatch'],scene_ordinal=slot+1,entry=scene['entry'],list=camera['list'],renderer=camera['renderer'],render_id=pair['render_id'],revision=pair['revision'],token=pair['token'],thread=record['thread'])
                check(all(type(event[k]) is int and event[k]==v for k,v in origin.items()),'temporal invocation does not join producing eye/job/thread')
                check(len(args)==len(invoked)==7 and all(uint(v) for v in args+invoked) and args[0]==camera['renderer'] and args[4]==camera['renderer']+0x10 and args[6]==2 and args[2],'native temporal ABI differs')
                check(before==event['after'] and before['counter']==scene['counter_before'] and before['active']==scene['entry'] and before['previous']==args[3] and before['next']==args[2],'actual native bank changed or does not join observed arguments')
                check(all(len(before[k])==11 and all(uint(w) for w in before[k]) for k in ('previous_bank','next_bank')) and len(before['descriptor_inputs'])==5 and all(uint(w) for w in before['descriptor_inputs']),'all native bank/descriptor words required')
                check(before['previous_bank'][2]==args[3] and before['next_bank'][2]==args[2],'native previous/next bank must preserve real caller inputs')
                check(event['texture_contract_before']==event['texture_contract_after'] and same_shape(native,event['texture_contract_before']),'actual native contract changed across filter')
                for label,actual in (('outputs',args),('invoked_outputs',invoked)):
                    wrappers=[actual[i] for i in (1,2,3,5)]+before['descriptor_inputs']
                    for phase in ('before','after'):
                        check(len(event[f'{label}_{phase}'])==9,'all nine actual/invoked native resource POD records required')
                        for output,wrapper in zip(event[f'{label}_{phase}'],wrappers):temporal_audit.resource(output,wrapper)
                    check([o['resource'] for o in event[f'{label}_before']]==[o['resource'] for o in event[f'{label}_after']],'native resource changed during filter')
                actual_wrappers=[args[i] for i in (1,2,3,5)]+before['descriptor_inputs'];contracts=event['texture_contract_before']['wrappers']
                check(len(contracts)==9 and [w['address'] for w in contracts]==actual_wrappers,'native wrapper contracts differ from observed arguments')
                check(invoked[:3]==args[:3] and invoked[4:]==args[4:] and event['controlled'] is bool(slot),'only right predecessor may change')
                if slot==0:
                    check(invoked==args and args[2]==borrowed and args[3]==previous[2]['events'][0]['arguments'][2],'left original pool parity/predecessor must remain unchanged')
                    check(event['outputs_after'][1]['resource'] not in [t['resource'] for t in textures],'left aliases owned right backing')
                else:
                    reset=previous[4]['record']['bootstrap'];check(record['reset'] is reset,'only first private use resets')
                    check(args[2]==record['destination'] and args[3]==record['observed_previous']==previous[4]['record']['destination'] and invoked[3]==record['invoked_previous']==(0 if reset else args[3]),'actual filter did not reset or consume its accepted own-right predecessor')
                    check(contracts[1]['vtable']==initial_contract['wrappers'][1]['vtable'] and contracts[1]['virtual_18_20_b0_c0']==initial_contract['wrappers'][1]['virtual_18_20_b0_c0'] and contracts[1]['flags']==0x20000b0|(1<<50),'owned native wrapper class/initialized flags differ')
                    for phase in ('before','after'):
                        output=event[f'invoked_outputs_{phase}'][1];identity,complete=temporal_audit.resource(output,args[2])
                        check(complete and identity==owned['identity'] and output['resource']==owned['resource'] and output['rtvs'],'actual owned destination lacks matching descriptor identity')
                        for view in output['rtvs']:check([view['width'],view['height']]==native_target['initialization_words'][row][:2] and all(view[k]==v for k,v in dict(storage=9,format=10,resource_dimension=3,flags=1,mips=1,arrays=1,samples=1,planes=1,subresources=1).items()),'actual private bloom RTV dimensions/format/flags differ')
                        check(event[f'outputs_{phase}'][1]==output,'observed/invoked destination metadata differs')
                    check(event['invoked_outputs_before'][2]['resource']==(0 if reset else previous[4]['record']['resource']),'actual invoked predecessor resource differs from accepted own-right output')
                    if not reset:check(invoked[3]!=args[2] and event['invoked_outputs_before'][2]['resource']!=event['outputs_after'][1]['resource'],'right ping-pong predecessor aliases current write destination')
            private=True
        check(record['colour_input']==record['tone_input']==record['destination']==right['persistent_after']['words'][2],'actual outer colour/tone input must join native right persistent export')
    except (KeyError,TypeError,ValueError,IndexError,StopIteration) as error:errors.append(str(error))
    return dict(owned_bloom_use_pass=not errors,errors=errors,bootstrap=first is None,private_right_bloom_observed=private and not errors,first_private_reset=reset and not errors,
                all_native_histories_covered=False,gpu_completion_verified=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False,retirement_verified=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args();controls=[];first=None;previous=None
    for index in range(4):
        try:
            observed=tuple(json.loads((args.directory/f'{name}_{index}.json').read_text()) for name in ('second','upscaler','temporal','creation','bloom'))
            result=analyze(*observed,first=first,previous=previous);controls.append(result);first=first or observed;previous=observed
        except (OSError,ValueError) as error:controls.append(dict(owned_bloom_use_pass=False,errors=[str(error)]))
    result=dict(all_controls_pass=all(c['owned_bloom_use_pass'] for c in controls),controls=controls,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)
    output=args.directory/'owned_bloom_use_audit.json';output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n');print(json.dumps(result,indent=2,allow_nan=False));return 0 if result['all_controls_pass'] else 1


if __name__=='__main__':raise SystemExit(main())
