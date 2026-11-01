import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1];SCRIPT=ROOT/'scripts/analyze_native_fault_pod.py'
spec=importlib.util.spec_from_file_location('fault_pod',SCRIPT);audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)


def synthetic_dump():
    # Minimal AMD64 minidump: exception (6), module list (4), misc info (15) and a trailing
    # reserved entry. Stands in for a real game crash dump, which is not kept in the repo.
    module=0x7ff600000000;rip=module+0xd6c19c
    registers=[0x1a0000000+i*0x10000 for i in range(16)]+[rip];fault=registers[8]+0xc0
    directory=32;exception=directory+4*12;context=exception+168;modules=context+1232;misc=modules+4+108
    raw=bytearray(misc+24);raw[0:4]=b'MDMP';struct.pack_into('<II',raw,8,4,directory)
    for index,(kind,size,offset) in enumerate(((6,168,exception),(4,4+108,modules),(15,24,misc),(0,0,0))):struct.pack_into('<III',raw,directory+12*index,kind,size,offset)
    struct.pack_into('<IIIIQQI',raw,exception,2908,0,0xc0000005,0,0,rip,2);struct.pack_into('<QQ',raw,exception+40,0,fault);struct.pack_into('<II',raw,exception+160,1232,context)
    struct.pack_into('<17Q',raw,context+120,*registers)
    struct.pack_into('<IQIII',raw,modules,1,module,0x6000000,0,0x6ac3eb69)
    struct.pack_into('<III',raw,misc,24,1,4242)
    return bytes(raw)


def fixture():
    dump=synthetic_dump();count,directory=struct.unpack_from('<II',dump,8);streams={}
    for index in range(count):
        kind,size,offset=struct.unpack_from('<III',dump,directory+index*12);streams[kind]=(size,offset)
    _,offset=streams[6];thread=struct.unpack_from('<I',dump,offset)[0];rip=struct.unpack_from('<Q',dump,offset+24)[0];operation,address=struct.unpack_from('<QQ',dump,offset+40);_,context=struct.unpack_from('<II',dump,offset+160);registers=struct.unpack_from('<17Q',dump,context+120)
    pid=123
    if 15 in streams:
        size,location=streams[15]
        if size>=12:
            _,flags,actual_pid=struct.unpack_from('<III',dump,location)
            if flags&1:pid=actual_pid
    module=rip-0xd6c19c;vtable=module+0x22222;table=0x23000000;packed=table|(4<<48);body=bytearray(512);struct.pack_into('<Q',body,0,vtable);struct.pack_into('<Q',body,0xd8,packed)
    region=lambda address,data: audit.REGION.pack(address,address&~0xfff,address&~0xfff,0x1000,0x1000,4,0x20000,len(data),data.ljust(512,b'\0'))
    header=audit.HEADER.pack(0x534d3246,1,2908,pid,thread,0x6ac3eb69,0xc0000005,1,0,1,module,rip,address,operation,registers[3],registers[1],registers[8],registers[4])
    data=header+region(registers[3],bytes(body))+region(registers[4],b'')+region(registers[8],b'')+region(vtable,b'\x6a'*512)+region(table,b'\x7b'*512)
    return data,dump,streams


class FaultPodTests(unittest.TestCase):
    def test_bounded_first_chance_bytes_join_real_preserved_exception_without_ownership_or_fix_claim(self):
        data,dump,_=fixture();value=audit.parse(data)
        self.assertTrue(value['first_chance_only']);self.assertFalse(value['crash_join_verified']);self.assertFalse(value['object_lifetime_verified']);self.assertFalse(value['cause_verified']);self.assertEqual(value['object_vtable_rva'],0x22222);self.assertEqual(value['packed_transform_index'],4)
        joined=audit.join_dump(value,dump);self.assertTrue(joined['crash_join_verified']);self.assertFalse(joined['cause_verified']);self.assertFalse(value['crash_join_verified'])

    def test_executable_duplicate_uses_exact_build_and_fault_rva(self):
        data,dump,streams=fixture();value=audit.parse(data);changed=bytearray(data)
        struct.pack_into('<I',changed,20,0x6ac3e716);struct.pack_into('<Q',changed,52,value['module']+0x134c30c)
        executable=audit.parse(changed);self.assertEqual(executable['image'],'executable')
        _,offset=streams[6];modified=bytearray(dump);struct.pack_into('<Q',modified,offset+24,executable['rip']);_,context=struct.unpack_from('<II',modified,offset+160);struct.pack_into('<Q',modified,context+248,executable['rip'])
        size,location=streams[4]
        for index in range(struct.unpack_from('<I',modified,location)[0]):
            entry=location+4+108*index;base,length=struct.unpack_from('<QI',modified,entry)
            if base==value['module']:struct.pack_into('<I',modified,entry+16,0x6ac3e716)
        self.assertTrue(audit.join_dump(executable,modified)['crash_join_verified'])
        with self.assertRaises(ValueError):audit.join_dump(executable,dump)
        struct.pack_into('<Q',changed,52,value['module']+0xd6c19c)
        with self.assertRaises(ValueError):audit.parse(changed)

    def test_malformed_or_unjoined_native_evidence_refuses(self):
        data,_,_=fixture()
        mutations=[data[:-1],data+b'\0']
        for offset,value in ((0,0),(4,2),(8,0),(20,0),(24,0),(28,0),(32,0xffffffff)):
            changed=bytearray(data);struct.pack_into('<I',changed,offset,value);mutations.append(changed)
        for offset,value in ((36,0),(52,17),(60,0),(68,1),(76,1)):
            changed=bytearray(data);struct.pack_into('<Q',changed,offset,value);mutations.append(changed)
        for offset,value in ((audit.HEADER.size,17),(audit.HEADER.size+32,0),(audit.HEADER.size+36,1),(audit.HEADER.size+44,513)):
            changed=bytearray(data);struct.pack_into('<I' if offset-audit.HEADER.size>=32 else '<Q',changed,offset,value);mutations.append(changed)
        changed=bytearray(data);changed[audit.HEADER.size+2*audit.REGION.size-1]=1;mutations.append(changed)
        for index,changed in enumerate(mutations):
            with self.subTest(index=index),self.assertRaises(ValueError):audit.parse(changed)

    def test_actual_dump_context_or_build_mutations_cannot_join(self):
        data,dump,streams=fixture();value=audit.parse(data);_,offset=streams[6]
        mutations=[dump[:10]]
        for location,fmt,number in ((offset,'<I',value['thread']+1),(offset+8,'<I',0),(offset+24,'<Q',value['rip']+1),(offset+48,'<Q',value['fault_address']+1)):
            changed=bytearray(dump);struct.pack_into(fmt,changed,location,number);mutations.append(changed)
        changed=copy.deepcopy(value);changed['timestamp']^=1
        with self.assertRaises(ValueError):audit.join_dump(changed,dump)
        count,directory=struct.unpack_from('<II',dump,8);duplicate=bytearray(dump);struct.pack_into('<III',duplicate,directory+12*(count-1),6,streams[6][0],streams[6][1]);mutations.append(duplicate)
        bad_reserved=bytearray(dump);struct.pack_into('<III',bad_reserved,directory+12*(count-1),0,1,0);mutations.append(bad_reserved)
        for changed in mutations:
            with self.assertRaises((ValueError,struct.error)):audit.join_dump(value,changed)

    def test_cli_preserves_existing_analysis_and_bad_input(self):
        data,dump,_=fixture()
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'fault.bin';mini=root/'crash.dmp';out=root/'analysis.json';source.write_bytes(data);mini.write_bytes(dump)
            command=[sys.executable,'-I',str(SCRIPT),str(source),'--dump',str(mini),'--output',str(out)]
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0);saved=out.read_bytes();self.assertTrue(json.loads(saved)['crash_join_verified'])
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,1);self.assertEqual(out.read_bytes(),saved)
            source.write_bytes(b'bad');self.assertEqual(subprocess.run(command,capture_output=True).returncode,1);self.assertEqual(out.read_bytes(),saved)


if __name__=='__main__':unittest.main()
