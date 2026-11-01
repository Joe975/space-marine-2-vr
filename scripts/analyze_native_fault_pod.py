"""Decode bounded first-chance fault POD; never infer a fix or object ownership."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

# Reference build (2026-10-05) and its verified port of 2026-10-21 (mod/src/GameBuildTable.inc).
SIGNATURES={0x6ac3eb69:('dll',0xd6c19c),0x6ac3e716:('executable',0x134c30c),0x6ad110a6:('dll',0xd6c3fc),0x6ad10fcb:('executable',0x134c0dc)}
HEADER=struct.Struct('<9I9Q');REGION=struct.Struct('<4Q4I512s')
LABELS=('rbx_object','stack','r8_table','object_vtable','object_handle_table')


def parse(data):
    if len(data)!=HEADER.size+5*REGION.size:raise ValueError('exact bounded native fault evidence length required')
    words=HEADER.unpack_from(data);names=('magic','version','size','pid','thread','timestamp','code','first_chance','disposition','filetime','module','rip','fault_address','operation','rbx','rcx','r8','rsp');value=dict(zip(names,words))
    if value['timestamp'] not in SIGNATURES or [value[k] for k in ('magic','version','size','code','first_chance','disposition')]!=[0x534d3246,1,len(data),0xc0000005,1,0]:raise ValueError('native fault evidence model/build/first-chance/disposition differs')
    image,rva=SIGNATURES[value['timestamp']]
    if not all(value[k] for k in ('pid','thread','filetime','module')) or value['module']+rva>=2**64 or value['rip']!=value['module']+rva or value['operation'] or value['r8']+0xc0>=2**64 or value['fault_address']!=value['r8']+0xc0:raise ValueError('native exception origin or actual read address differs')
    regions=[]
    for index,label in enumerate(LABELS):
        address,allocation,base,region_size,state,protect,kind,size,raw=REGION.unpack_from(data,HEADER.size+index*REGION.size)
        if size>512 or any(raw[size:]):raise ValueError('native region byte extent differs from bounded actual copy')
        if size and (state!=0x1000 or not protect or protect&0x101 or not address or not base<=address<base+region_size or address+size>base+region_size or base+region_size>=2**64):raise ValueError('native region bytes lack readable committed page bounds')
        regions.append(dict(label=label,address=address,allocation=allocation,base=base,region_size=region_size,state=state,protect=protect,type=kind,size=size,bytes=raw[:size].hex()))
    object_bytes=bytes.fromhex(regions[0]['bytes']);vtable=int.from_bytes(object_bytes[:8],'little') if len(object_bytes)>=8 else 0;packed=int.from_bytes(object_bytes[0xd8:0xe0],'little') if len(object_bytes)>=0xe0 else 0
    expected=(value['rbx'],value['rsp'],value['r8'],vtable,packed&0xffffffffffff)
    if tuple(r['address'] for r in regions)!=expected:raise ValueError('actual register/object fields do not join observed region addresses')
    return dict(model='ordinary_native_fault_pod_v1',image=image,**value,regions=regions,object_vtable_rva=vtable-value['module'] if value['module']<=vtable<value['module']+0x7000000 else None,packed_transform=packed,packed_transform_index=(packed>>48),first_chance_only=True,crash_join_verified=False,cause_verified=False,object_lifetime_verified=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False,sha256=hashlib.sha256(data).hexdigest())


def join_dump(value,raw):
    def span(offset,count):
        if offset<0 or count<0 or offset+count>len(raw):raise ValueError('truncated minidump extent')
        return raw[offset:offset+count]
    def unpack(fmt,offset):return struct.unpack(fmt,span(offset,struct.calcsize(fmt)))
    if span(0,4)!=b'MDMP':raise ValueError('minidump signature differs')
    count,directory=unpack('<II',8)
    if not 1<=count<=4096:raise ValueError('unbounded minidump directory')
    streams={}
    for index in range(count):
        kind,size,offset=unpack('<III',directory+12*index);span(offset,size)
        if kind==0:
            if size or offset:raise ValueError('nonempty reserved minidump directory entry')
            continue
        if kind in streams:raise ValueError('ambiguous duplicate minidump stream')
        streams[kind]=(size,offset)
    if 6 not in streams or streams[6][0]<168:raise ValueError('minidump exception stream missing')
    _,offset=streams[6];thread=unpack('<I',offset)[0];code,_,_,rip,parameters=unpack('<IIQQI',offset+8);operation,address=unpack('<QQ',offset+40);context_size,context=unpack('<II',offset+160)
    if context_size<256:raise ValueError('minidump AMD64 exception context incomplete')
    span(context,context_size)
    registers=dict(zip(('rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi','r8','r9','r10','r11','r12','r13','r14','r15','rip'),unpack('<17Q',context+120)))
    if (thread,code,rip,parameters,operation,address)!=(value['thread'],value['code'],value['rip'],2,value['operation'],value['fault_address']) or any(registers[k]!=value[k] for k in ('rip','rbx','rcx','r8','rsp')):raise ValueError('actual minidump exception/registers do not join first-chance evidence')
    if 4 not in streams:raise ValueError('actual game module provenance missing from dump')
    size,offset=streams[4];modules=unpack('<I',offset)[0]
    if 4+108*modules>size:raise ValueError('truncated minidump module table')
    matches=[]
    for index in range(modules):
        base,length,_,stamp=unpack('<QIII',offset+4+108*index)
        if base+length>=2**64:raise ValueError('minidump module extent overflows')
        if base<=rip<base+length:matches.append((base,stamp))
    if matches!=[(value['module'],value['timestamp'])]:raise ValueError('actual faulting module base/build differs from first-chance evidence')
    if 15 in streams:
        size,offset=streams[15]
        if size>=12:
            _,flags,pid=unpack('<III',offset)
            if flags&1 and pid!=value['pid']:raise ValueError('actual minidump process differs')
    return dict(crash_join_verified=True,dump_sha256=hashlib.sha256(raw).hexdigest(),cause_verified=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('evidence',type=Path);parser.add_argument('--dump',type=Path);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    try:
        value=parse(args.evidence.read_bytes())
        if args.dump:value.update(join_dump(value,args.dump.read_bytes()))
    except (OSError,ValueError,struct.error) as error:print(str(error));return 1
    if args.output.exists():print('Refusing to overwrite existing fault analysis');return 1
    args.output.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8');print(json.dumps(value,indent=2));return 0


if __name__=='__main__':raise SystemExit(main())
