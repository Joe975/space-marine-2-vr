"""Audit copied per-slot dispatcher history banks, never GPU ownership."""
import argparse
import importlib.util
import json
from pathlib import Path

_spec=importlib.util.spec_from_file_location('ordinary_temporal_banks',Path(__file__).resolve().with_name('analyze_ordinary_temporal.py'))
temporal_audit=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(temporal_audit)
uint=temporal_audit.uint


def analyze(camera,sdk,temporal,previous=None):
    errors=[];banks=[]
    try:
        observed=temporal_audit.analyze(camera,sdk,temporal,previous)
        if not observed['native_temporal_observation_pass']:raise ValueError('accepted native camera/SDK/temporal observations required')
        if len(camera['scenes'])!=2:raise ValueError('exactly two ordinary Scene records required')
        renderer=camera['renderer']
        if not uint(renderer) or not renderer or renderer>2**64-1-0x2b0:raise ValueError('persistent bank address overflow')
        for slot,scene in enumerate(camera['scenes']):
            before=scene['persistent_before'];after=scene['persistent_after'];address=renderer+0x200+slot*0x58
            for bank in (before,after):
                if type(bank['address']) is not int or bank['address']!=address or len(bank['words'])!=11 or not all(uint(word) for word in bank['words']):raise ValueError('exact bounded per-slot bank address/words required')
            if before['words']!=scene['temporal_before']['previous_words']:raise ValueError('persistent slot bank does not join native Scene previous inputs')
            if after['words']!=scene['temporal_after']['next_words']:raise ValueError('native Scene next bank does not join dispatcher persistent export')
            if previous is not None:
                prior=previous[0]['scenes'][slot]['persistent_after']
                if prior['address']!=address or prior['words']!=before['words']:raise ValueError('following camera lacks its same-slot preceding dispatcher export')
            elif slot==1 and any(before['words']):raise ValueError('first appended ordinary slot was not empty')
            banks.append(dict(slot=slot,address=address,loaded_previous=True,exported_next=True,same_slot_continuity=previous is not None))
    except (KeyError,TypeError,ValueError,IndexError) as error:errors.append(str(error))
    return dict(persistent_history_bank_observation_pass=not errors,errors=errors,banks=banks,
                resource_leases=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args();controls=[];previous=None
    for index in range(4):
        try:
            observed=tuple(json.loads((args.directory/f'{name}_{index}.json').read_text()) for name in ('second','upscaler','temporal'))
            controls.append(analyze(*observed,previous=previous));previous=observed
        except (OSError,ValueError) as error:controls.append(dict(persistent_history_bank_observation_pass=False,errors=[str(error)]))
    result=dict(all_controls_pass=all(c['persistent_history_bank_observation_pass'] for c in controls),controls=controls,
                native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False)
    (args.directory/'persistent_history_bank_audit.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n');print(json.dumps(result,indent=2,allow_nan=False));return 0 if result['all_controls_pass'] else 1


if __name__=='__main__':raise SystemExit(main())
