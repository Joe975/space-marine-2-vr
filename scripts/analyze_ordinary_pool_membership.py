"""Independently audit guarded native pool lookup POD for unused owned bloom."""
import argparse
import importlib.util
import json
from pathlib import Path

_spec=importlib.util.spec_from_file_location('native_creation_pool',Path(__file__).resolve().with_name('analyze_ordinary_native_creation.py'))
creation_audit=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(creation_audit)
uint=creation_audit.uint


def native_hash(key):
    mask=(1<<64)-1
    key=((key^(key>>33))*0xff51afd7ed558ccd)&mask
    key=((key^(key>>33))*0xc4ceb9fe1a85ec53)&mask
    return (key^(key>>33))&0xffffffff


def lookup(value,pool,key,records,found,record=-1):
    if value['known'] is not True or value['found'] is not found or type(value['refusal']) is not int or value['refusal']!=0:raise ValueError('stable exact native pool membership result required')
    for field,expected in dict(pool=pool,key=key,record_count=records,record=record).items():
        if type(value[field]) is not int or value[field]!=expected:raise ValueError('native lookup origin/key/record differs')
    count=value['count'];capacity=value['capacity'];nodes=value['nodes'];head=value['head']
    if type(count) is not int or not 0<count<=1048576 or type(capacity) is not int or not 0<capacity<=1048576 or not uint(value['buckets']) or not value['buckets'] or value['buckets']>2**64-1-capacity*8:raise ValueError('bounded nonempty native pool table required')
    if type(value['bucket']) is not int or value['bucket']!=native_hash(key)%capacity or not uint(head) or not isinstance(nodes,list) or len(nodes)>64:raise ValueError('native hash/bucket/chain bounds differ')
    cursor=head;seen=set()
    for index,node in enumerate(nodes):
        if not uint(node['address']) or not node['address'] or node['address']>2**64-1-20 or node['address']!=cursor or node['address'] in seen or not uint(node['key']) or not uint(node['next']) or type(node['record']) is not int or not -1<=node['record']<records:raise ValueError('native collision chain is unreadable/cyclic/incoherent')
        seen.add(node['address'])
        if node['key']==key:
            if not found or index!=len(nodes)-1 or node['record']!=record:raise ValueError('native collision chain contradicts registered/absent result')
        cursor=node['next']
    if found:
        if not nodes or nodes[-1]['key']!=key:raise ValueError('registered positive control has no matching native node')
    elif cursor:raise ValueError('absence claim lacks a complete chain ending in null')


def analyze(camera,sdk,temporal,creation,first=None,previous=None):
    errors=[]
    try:
        if not creation_audit.analyze(camera,sdk,temporal,creation,first,previous)['native_creation_pair_pass']:raise ValueError('accepted actual camera/creation ownership required')
        initial=(camera,sdk,temporal,creation) if first is None else first
        event=next(e for e in initial[2]['events'] if e['slot']==1 and e['mode']==2);native=event['texture_contract_before'];proof=creation['pool_membership']
        if proof['model']!='native_pool_membership_v1' or proof['instruction_guards'] is not True or type(proof['release_rva']) is not int or proof['release_rva']!=0x2079230:raise ValueError('exact full native release instruction guard required')
        lookup(proof['source'],native['pool'],event['arguments'][2],native['record_count'],True,native['targets'][event['before']['counter']&1]['record'])
        if len(proof['owned'])!=2:raise ValueError('both owned native wrappers require actual absence proof')
        for evidence,texture in zip(proof['owned'],creation['textures']):lookup(evidence,native['pool'],texture['native'],native['record_count'],False)
        if first is not None and proof!=initial[3]['pool_membership']:raise ValueError('initial native membership proof changed between capture reports')
    except (KeyError,TypeError,ValueError,IndexError,StopIteration) as error:errors.append(str(error))
    return dict(native_pool_membership_pass=not errors,errors=errors,native_release_invoked=False,native_writes=False,
                used_in_native_pass=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args();controls=[];first=None;previous=None
    for index in range(4):
        try:
            observed=tuple(json.loads((args.directory/f'{name}_{index}.json').read_text()) for name in ('second','upscaler','temporal','creation'))
            controls.append(analyze(*observed,first=first,previous=previous));first=first or observed;previous=observed[:3]
        except (OSError,ValueError) as error:controls.append(dict(native_pool_membership_pass=False,errors=[str(error)]))
    result=dict(all_controls_pass=all(c['native_pool_membership_pass'] for c in controls),controls=controls,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)
    (args.directory/'native_pool_membership_audit.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n');print(json.dumps(result,indent=2,allow_nan=False));return 0 if result['all_controls_pass'] else 1


if __name__=='__main__':raise SystemExit(main())
