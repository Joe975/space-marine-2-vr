"""Audit actual two-ordinary-camera CPU provenance; never certify GPU/history readiness."""
import argparse
import json
import math
from pathlib import Path


def analyze(capture):
    errors=[];shared=[]
    try:
        c=capture;a=c['append'];p=c['ordinary_pair'];views=c['views'];scenes=c['scenes']
        if c['model']!='ordinary_second_camera_v2' or c['cpu_only'] is not True:
            raise ValueError('wrong second ordinary camera model')
        for key in ('pip_enabled','gpu_pair_verified','histories_verified','runtime_submission'):
            if c[key] is not False:raise ValueError('unsupported stronger claim: '+key)
        for key in ('dispatch','renderer','list'):
            if type(c[key]) is not int or c[key]<=0:raise ValueError('missing native '+key)
        for key in ('render_id','thread','list','renderer'):
            if type(a[key]) is not int or a[key]<=0:raise ValueError('missing append '+key)
        if (type(a['token']) is not int or a['token']<0 or a['left']!=0 or a['right']!=1
                or a['renderer']!=c['renderer']
                or a['appended'] is not True or a['source_unchanged'] is not True or a['refusal']!=0):
            raise ValueError('invalid actual ordinary append')
        if type(a['bank_zero']) is not bool:raise ValueError('missing first-bank observation')
        for key in ('left_flags','right_flags'):
            if type(a[key]) is not int or a[key]<=0 or a[key]&0x50000044:raise ValueError('non-ordinary native flags')
        if a['left_flags']!=a['right_flags']:raise ValueError('native second flags changed')
        if (p['render_id']!=a['render_id'] or p['token']!=a['token'] or type(p['revision']) is not int or p['revision']<=0
                or type(a['producer_revision']) is not int or a['producer_revision']<=0
                or p['producer_list']!=a['list'] or p['producer_revision']!=a['producer_revision']
                or p['add_list']!=c['list']+0x160 or p['left_slot']!=0 or p['right_slot']!=1
                or p['left_entry']!=p['add_list']+8 or p['right_entry']!=p['add_list']+8+0xa60):
            raise ValueError('two ordinary slots lack matching producing-job tickets')
        if len(views)!=2 or len(scenes)!=2 or c['scene_calls']!=2 or c['scene_refused']!=0 or c['scene_dropped']!=0:
            raise ValueError('exactly two unrefused native Scene calls required')
        for i,(v,s) in enumerate(zip(views,scenes)):
            if (v['slot']!=i or v['flags']!=a['left_flags'] or s['slot']!=i
                    or s['entry']!=p['add_list']+8+i*0xa60 or s['active_entry_after']!=s['entry'] or s['refusal']!=0
                    or type(s['counter_before']) is not int or s['counter_before']<0 or s['counter_after']!=s['counter_before']
                    or s['counter_before']!=scenes[0]['counter_before']):
                raise ValueError('native scene slot/counter changed')
            w=v['world']
            if len(w)!=16 or not all(type(x) in (int,float) and math.isfinite(x) for x in w):raise ValueError('invalid camera matrix')
            for stage in ('temporal_before','temporal_after'):
                for group in ('previous_words','next_words'):
                    words=s[stage][group]
                    if len(words)!=11 or not all(type(x) is int and 0<=x<2**64 for x in words):raise ValueError('missing exact opaque temporal words')
        left,right=(v['world'] for v in views)
        if any(abs(left[n]-right[n])>1e-5 for n in range(12)) or abs(left[15]-right[15])>1e-5:
            raise ValueError('second view changes the native camera basis')
        if any(abs((right[12+n]-left[12+n])-.064*left[n])>3e-4 for n in range(3)):
            raise ValueError('missing actual 0.064-world-unit ordinary view separation')
        x=scenes[0]['temporal_after']['next_words'];y=scenes[1]['temporal_after']['next_words']
        shared=[i for i in range(11) if x[i] and x[i]==y[i]]
    except (KeyError,TypeError,ValueError,IndexError) as error:errors.append(str(error))
    return dict(ordinary_camera_provenance_pass=not errors,errors=errors,shared_temporal_word_indices=shared,
                gpu_pair_verified=False,histories_verified=False,simulation_snapshot_verified=False,runtime_submission=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args();controls=[]
    for i in range(4):
        try:
            capture=json.loads((args.directory/f'second_{i}.json').read_text());result=analyze(capture)
            if i==0 and capture['append']['bank_zero'] is not True:
                result['ordinary_camera_provenance_pass']=False;result['errors'].append('first appended ordinary state bank was not observed unused')
        except (OSError,ValueError) as e:result=dict(ordinary_camera_provenance_pass=False,errors=[str(e)])
        controls.append(result)
    passed=all(x['ordinary_camera_provenance_pass'] for x in controls)
    report=dict(all_controls_pass=passed,controls=controls,gpu_pair_verified=False,histories_verified=False,runtime_submission=False)
    (args.directory/'second_camera_audit.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));return 0 if passed else 1


if __name__=='__main__':raise SystemExit(main())
