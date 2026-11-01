"""Freeze authenticated offline native-helper outputs for dependency-free C++ tests."""
from pathlib import Path
import argparse
import importlib.util
import json
import math

spec=importlib.util.spec_from_file_location('native_math_oracle',Path(__file__).with_name('native_camera_math_oracle.py'))
oracle=importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)

def render_fixture(report):
    if report.get('model')!='offline_native_camera_math_oracle_v1' or report.get('pass_') is not True or report.get('exe_sha256')!=oracle.EXE_SHA256:
        raise ValueError('accepted authenticated offline native oracle required')
    for key,value in {'actual_native_code_emulated':True,'synthetic_inputs':True,'main_schedule_modeled':True,'actual_game_run':False,'native_scene_verified':False,'native_gpu_histories_verified':False,'gpu_pair_verified':False,'runtime_submission':False,'headset_ready':False}.items():
        if report.get(key) is not value:raise ValueError('wrong oracle evidence boundary '+key)
    functions={hex(k):dict(bytes=v[0],sha256=v[1]) for k,v in oracle.FUNCTIONS.items()}
    if report.get('functions')!=functions or set(report.get('visited',[]))!=set(functions):raise ValueError('missing/changed native helper proof')
    if report.get('cache_add_phase_checks')!=35:raise ValueError('native add-time projection phase must be verified in every control')
    calibration=report.get('calibration',[])
    if len(calibration)!=3:raise ValueError('three recorded left calibration controls required')
    for index,row in enumerate(calibration,1):
        if row.get('index')!=index or row.get('pass_') is not True or set(row.get('errors',{}))!={hex(k) for k in oracle.OFFSETS}:raise ValueError('incomplete native calibration')
        if any(not math.isfinite(error) or error<0 or error>(1e-3 if int(offset,16) in (0x9b4,0x9f4) else 3e-4) for offset,error in row['errors'].items()):raise ValueError('failed native calibration')
    cases=report.get('cases',[])
    if len(cases)!=32:raise ValueError('32 explicit synthetic lens/native-helper cases required')
    def raw(values,length):
        if not isinstance(values,list) or len(values)!=length or any(type(v)is not int or not 0<=v<2**32 for v in values):raise ValueError('invalid raw native words')
        if not all(map(math.isfinite,oracle.floats(values))):raise ValueError('nonfinite native matrix')
        return '{{'+','.join(str(v)+'U' for v in values)+'}}'
    lines=['// Generated offline native helpers; synthetic inputs and modeled main schedule.',
           '// NO actual game/Scene/GPU/runtime/headset claim. See matching oracle JSON.',
           '#pragma once','#include <array>','#include <cstdint>','namespace asymmetric_native_fixture {',
           'struct Case {std::array<uint32_t,16> world,view,projection;std::array<uint32_t,4> fov;std::array<uint32_t,2> clip,jitter;uint32_t width,height,scale,depth;int previous;std::array<std::array<uint32_t,16>,23> expected;};',
           'inline constexpr std::array<Case,32> CASES{{']
    for index,c in enumerate(cases):
        if (c.get('scenario'),c.get('eye'),c.get('frame'))!=(index//4,index//2%2,index%2) or c.get('previous')!=(index-1 if index%2 else -1):raise ValueError('cross-eye/scenario predecessor')
        if type(c.get('width'))is not int or type(c.get('height'))is not int or not 1<=c['width']<=32768 or not 1<=c['height']<=32768:raise ValueError('invalid native dimensions')
        if set(c.get('expected',{}))!={hex(k) for k in oracle.OFFSETS}:raise ValueError('missing native matrix output')
        for key,offset in [('world',0x34),('view',0x3dc),('projection',0x49c)]:
            if c[key]!=c['expected'][hex(offset)]:raise ValueError('native input/output identity changed')
        parts=[raw(c[k],16) for k in ('world','view','projection')]+[raw(c['fov'],4),raw([c['near'],c['far']],2),raw(c['jitter'],2)]
        if oracle.words(oracle.projection(oracle.floats(c['fov']),*oracle.floats([c['near'],c['far']])))!=c['projection']:raise ValueError('projection does not match explicit lens')
        raw([c['scale'],c['depth']],2)
        parts += [str(c[k])+'U' for k in ('width','height','scale','depth')]+[str(c['previous'])]
        parts += ['{{'+','.join(raw(c['expected'][hex(offset)],16) for offset in oracle.OFFSETS)+'}}']
        lines.append('Case{'+','.join(parts)+'},')
    return '\n'.join(lines+['}};','}'])+'\n'

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('source',type=Path);parser.add_argument('output',type=Path);args=parser.parse_args()
    if not args.source.is_absolute() or not args.output.is_absolute():parser.error('absolute paths required')
    text=render_fixture(json.loads(args.source.read_text(encoding='utf-8-sig')))
    args.output.write_text(text,encoding='utf-8',newline='\n');print(args.output)
if __name__=='__main__':main()
