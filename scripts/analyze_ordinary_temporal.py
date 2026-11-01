"""Audit actual native temporal parameter/bank observations, never GPU ownership."""
import argparse
import importlib.util
import json
from pathlib import Path

_spec=importlib.util.spec_from_file_location('ordinary_transaction',Path(__file__).resolve().with_name('analyze_ordinary_packet_transaction.py'))
camera_audit=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(camera_audit)
CALLERS={0:0x180a648,1:0x180a84e,2:0x180a711}
BANK={0:1,1:5,2:2}
OUTPUT_LABELS=['source','destination','previous','extra','descriptor+0x10','descriptor+0x18','descriptor+0x20','descriptor+0x68','descriptor+0x170']

def uint(value):return type(value) is int and 0<=value<2**64


def separation(inventory,links,has_previous):
    """A necessary observed-resource gate, never proof of full GPU ownership."""
    errors=[]
    if not inventory:errors.append('no observed native temporal destinations')
    for mode in inventory:
        if mode['shared_destination_wrapper'] or mode['shared_destination_resource'] or mode['shared_destination_canonical_identity']:
            errors.append(f"mode {mode['mode']} eyes share the native temporal destination")
    if has_previous and not links:errors.append('no observed native temporal predecessor links')
    for link in links:
        if not link['uses_previous_argument']:continue
        if not link['same_eye_previous_wrapper_matches'] or not link['same_eye_previous_resource_matches']:
            errors.append(f"slot {link['slot']} mode {link['mode']} lacks its own preceding destination")
        if link['other_eye_previous_resource_matches']:
            errors.append(f"slot {link['slot']} mode {link['mode']} uses the other eye's preceding destination")
    return errors

def resource(value,wrapper):
    """Validate copied metadata; a cached descriptor is not a resource lease."""
    if not all(uint(value[key]) for key in ('resource','status','epoch','serial')) or not isinstance(value['rtvs'],list):raise ValueError('invalid temporal resource POD')
    if not wrapper:
        if value['resource'] or value['status']!=0 or value['rtvs']:raise ValueError('null wrapper acquired a resource observation')
        return None,True
    if not value['resource'] or value['status'] not in (2,5) or len(value['rtvs'])>8:raise ValueError('nonnull native wrapper lacks stable readable resource POD')
    if value['status']==2:
        if value['rtvs']:raise ValueError('unobserved resource has fabricated RTVs')
        return None,False
    if not value['rtvs'] or not value['epoch']:raise ValueError('observed native resource lacks descriptor epoch/views')
    identities=set();complete=True
    for view in value['rtvs']:
        fields=('resource','identity','handle','created','bound','command_list','width','height','storage','format','resource_dimension','flags','mips','arrays','samples','planes','subresources')
        if not all(uint(view[key]) for key in fields) or type(view['known']) is not bool or view['resource']!=value['resource'] or not view['handle'] or not 0<view['created']<=value['serial'] or view['bound']>value['serial']:raise ValueError('native resource descriptor attribution differs from the actual cached record')
        if not view['known'] or not view['identity']:complete=False
        else:identities.add(view['identity'])
        if not all(view[key] for key in ('width','height','mips','arrays','samples','planes','subresources')):complete=False
        else:
            expected=1 if view['resource_dimension']==1 else view['mips']*view['planes']*(1 if view['resource_dimension']==4 else view['arrays'])
            if view['subresources']!=expected:raise ValueError('native resource flags/subresource record is incoherent')
    if len(identities)>1:raise ValueError('one native pointer maps to multiple descriptor identities')
    return next(iter(identities),None),complete

def analyze(camera,sdk,temporal,previous=None):
    errors=[];inventory=[];identities={};events={};metadata_complete=True;links=[]
    try:
        if not camera_audit.analyze(camera,sdk,previous[:2] if previous else None)['native_transaction_pass']:raise ValueError('accepted actual native camera/SDK transaction gates required')
        if temporal['model']!='ordinary_native_temporal_calls_v1' or temporal['cpu_only'] is not True or temporal['output_labels']!=OUTPUT_LABELS or temporal['installed'] is not True or any(type(temporal[k]) is not int or temporal[k]!=0 for k in ('refused','errors','dropped')):raise ValueError('temporal observer absent or recorded refused/lost evidence')
        for key in ('resource_leases','native_gpu_histories_verified','gpu_pair_verified','runtime_submission'):
            if temporal[key] is not False:raise ValueError('unsupported GPU ownership claim: '+key)
        if not temporal['events']:raise ValueError('no actual native temporal calls observed; native history coverage remains unproven')
        pair=camera['ordinary_pair'];serial=0
        for event in temporal['events']:
            if any(key in event for key in ('controlled','invoked_arguments','invoked_outputs_before','invoked_outputs_after')):raise ValueError('explicit invocation-control evidence requires the v2 owned-use audit, not unchanged v1 observation')
            mode=event['mode'];slot=event['slot']
            if type(mode) is not int or mode not in BANK or type(slot) is not int or slot not in (0,1) or (slot,mode) in events:raise ValueError('unknown/duplicate native temporal mode in one eye')
            scene=camera['scenes'][slot];args=event['arguments'];before=event['before'];after=event['after']
            if not uint(event['serial']) or event['serial']<=serial or not uint(event['thread']) or not event['thread'] or type(event['refusal']) is not int or event['refusal']!=0 or event['caller']!=CALLERS[mode]:raise ValueError('native temporal caller/order/refusal is not exact')
            serial=event['serial']
            expected={'dispatch':camera['dispatch'],'scene_ordinal':slot+1,'entry':scene['entry'],'list':camera['list'],'renderer':camera['renderer'],'render_id':pair['render_id'],'revision':pair['revision'],'token':pair['token']}
            if any(type(event[k]) is not int or event[k]!=v for k,v in expected.items()):raise ValueError('temporal call does not join the actual producing Scene/pair')
            if len(args)!=7 or not all(uint(v) for v in args) or args[0]!=camera['renderer'] or args[4]!=camera['renderer']+0x10 or args[6]!=mode or not args[2]:raise ValueError('actual native seven-argument ABI differs')
            if before!=after or type(before['counter']) is not int or before['counter']!=scene['counter_before'] or before['active']!=scene['entry'] or before['previous']!=args[3] or before['next']!=args[2]:raise ValueError('native scope/bank changed across the actual temporal call')
            for label in ('previous_bank','next_bank'):
                if len(before[label])!=11 or not all(uint(v) for v in before[label]):raise ValueError('exact native temporal bank words required')
            if before['previous_bank'][BANK[mode]]!=args[3] or before['next_bank'][BANK[mode]]!=args[2]:raise ValueError('mode-specific previous/next native bank differs from call arguments')
            inputs=before['descriptor_inputs']
            if len(inputs)!=5 or not all(uint(v) for v in inputs):raise ValueError('actual five native descriptor resource inputs required')
            wrappers=[args[i] for i in (1,2,3,5)]+inputs
            for label in ('outputs_before','outputs_after'):
                if len(event[label])!=9:raise ValueError('all nine native wrapper observations required')
                for index,(output,wrapper) in enumerate(zip(event[label],wrappers)):
                    identity,complete=resource(output,wrapper);metadata_complete=metadata_complete and complete
                    if label=='outputs_before':identities[slot,mode,index]=identity
                    elif event['outputs_before'][index]['resource']!=output['resource']:raise ValueError('native wrapper resource changed during temporal work')
            events[slot,mode]=event
        left={mode for slot,mode in events if slot==0};right={mode for slot,mode in events if slot==1}
        if left!=right or not left:raise ValueError('native active temporal modes differ between eyes')
        for mode in sorted(left):
            a,b=events[0,mode],events[1,mode]
            inventory.append(dict(mode=mode,left_destination=a['arguments'][2],right_destination=b['arguments'][2],shared_destination_wrapper=a['arguments'][2]==b['arguments'][2],
                shared_destination_resource=a['outputs_after'][1]['resource']==b['outputs_after'][1]['resource'],
                shared_destination_canonical_identity=bool(identities[0,mode,1] and identities[0,mode,1]==identities[1,mode,1]),
                left_canonical_identity=identities[0,mode,1],right_canonical_identity=identities[1,mode,1]))
        if previous:
            prior={(e['slot'],e['mode']):e for e in previous[2]['events'] if e['refusal']==0}
            for (slot,mode),event in events.items():
                earlier=prior.get((slot,mode));other=prior.get((1-slot,mode));used=event['arguments'][3]!=0 and camera['scenes'][slot]['packet_before']['motion_flags'][1]==0
                links.append(dict(slot=slot,mode=mode,uses_previous_argument=used,same_eye_previous_wrapper_matches=bool(earlier and event['arguments'][3]==earlier['arguments'][2]),
                    same_eye_previous_resource_matches=bool(earlier and event['outputs_before'][2]['resource']==earlier['outputs_after'][1]['resource']),
                    other_eye_previous_resource_matches=bool(other and event['outputs_before'][2]['resource']==other['outputs_after'][1]['resource'])))
    except (KeyError,TypeError,ValueError,IndexError) as error:errors.append(str(error))
    separation_errors=separation(inventory,links,previous is not None)
    return dict(native_temporal_observation_pass=not errors,errors=errors,mode_inventory=inventory,previous_links=links,descriptor_metadata_complete=metadata_complete and bool(events),
        observed_native_history_separation_pass=not errors and not separation_errors,history_separation_errors=separation_errors,
        all_native_histories_covered=False,resource_leases=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args();controls=[];previous=None
    for index in range(4):
        try:
            observed=tuple(json.loads((args.directory/f'{name}_{index}.json').read_text()) for name in ('second','upscaler','temporal'))
            controls.append(analyze(*observed,previous=previous));previous=observed
        except (OSError,ValueError) as error:controls.append(dict(native_temporal_observation_pass=False,errors=[str(error)]))
    result=dict(all_controls_pass=all(c['native_temporal_observation_pass'] for c in controls),
        observed_native_history_separation_pass=all(c.get('observed_native_history_separation_pass',False) for c in controls),
        controls=controls,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)
    (args.directory/'native_temporal_calls_audit.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n');print(json.dumps(result,indent=2,allow_nan=False));return 0 if result['all_controls_pass'] else 1

if __name__=='__main__':raise SystemExit(main())
