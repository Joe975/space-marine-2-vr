"""Isolated, bounded execution of authenticated SM2 math instructions.

No game loader, entry point, imports, OS calls, GPU or runtime is invoked.
Inputs and the main-helper copy/transpose/jitter schedule are controlled models;
this is an offline math oracle, not actual campaign/Scene ownership evidence.
Optional dependency: requirements-native-math.txt. API reference:
https://www.unicorn-engine.org/docs/tutorial.html
"""
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct

EXE_SHA256='8e33f3dc6e0b4475c0f55ad180f35d45eccd64f43fccd45f81f9375bd3870718'
FUNCTIONS={
    0x1d75d30:(2514,'5cd5bb1dd9f25f63289de541a4e4f8edee293e6322fd8b5c7427b651772a24b3'),
    0x18159a0:(1275,'f8ba8ca70d6c38ed06a99db2a25889b52a8d7cc9e3e9953a74d8babfc7ecfa96'),
    0x1f8f740:(394,'220f50e7adc0b4f41d46e2cbbc13bdf9ee8f3f07e52e0a8228b731c9c03be0c5'),
    0x27a0b40:(54,'a1972173dc302bca1ede622cde05f9aee8ca9eb33f462ad10d0987a5941f5a36'),
    0x27a18a0:(1812,'8285709765799639af5e39bf67983173967b0f5654b54fefdf9c3b29c5d776c7'),
    0x27a1fc0:(5,'e25c59e69e49b6ea9ad7c68df329b5ac417b97cc58d2f5fa5fa2ccd7a02352da'),
    0x1ab9060:(325,'dbaa21d9f9f9b40723aa9c59d198d8f1cf75ef7426210a54a11377a152770c33'),
}
OFFSETS=(0x34,0x3dc,0x41c,0x45c,0x49c,0x4dc,0x51c,0x55c,0x59c,0x61c,0x65c,0x6b4,0x6f4,0x734,0x774,0x7b4,0x7f4,0x834,0x8f4,0x934,0x974,0x9b4,0x9f4)
ENTRY=0x50001000
IDENTITY=[float(i//4==i%4) for i in range(16)]
def f32(value):return struct.unpack('<f',struct.pack('<f',value))[0]
def words(values):return list(struct.unpack('<'+'I'*len(values),struct.pack('<'+'f'*len(values),*values)))
def floats(values):return list(struct.unpack('<'+'f'*len(values),struct.pack('<'+'I'*len(values),*values)))
def transpose(values):return [values[c*4+r] for r in range(4) for c in range(4)]
def projection(fov,near,far):
    left,right,bottom,top=map(f32,fov);near,far=f32(near),f32(far)
    if not all(map(math.isfinite,(left,right,bottom,top,near,far))) or not(left<0<right and bottom<0<top and 0<near<far):raise ValueError('invalid lens')
    try:
        dx,dy=f32(right-left),f32(top-bottom);out=[0.]*16
        out[0]=f32(2/dx);out[5]=f32(2/dy);out[8]=f32(-f32(right+left)/dx);out[9]=f32(-f32(top+bottom)/dy)
        out[10]=f32(near/f32(near-far));out[11]=1.;out[14]=f32(-out[10]*far)
        rectangle=[f32(near*v) for v in (left,bottom,dx,dy)]
        if dx<=1e-5 or dy<=1e-5 or not all(map(math.isfinite,out+rectangle+[f32(dy/dx)])) or min(out[0],out[5],out[14])<=0:raise ValueError('invalid lens arithmetic')
    except (OverflowError,ZeroDivisionError) as error:raise ValueError('invalid lens arithmetic') from error
    return out

class Sandbox:
    def __init__(self,exe):
        exe=Path(exe)
        if not exe.is_absolute():raise ValueError('absolute local executable path required')
        data=exe.read_bytes();digest=hashlib.sha256(data).hexdigest()
        if digest!=EXE_SHA256:raise ValueError('unsupported native executable SHA256')
        import pefile
        import unicorn as uc
        from unicorn import x86_const as x
        self.exe_sha256=digest
        self.uc,self.x=uc,x;self.pe=pefile.PE(data=data,fast_load=True);self.base=self.pe.OPTIONAL_HEADER.ImageBase
        if self.base!=0x140000000 or self.pe.FILE_HEADER.TimeDateStamp!=0x6ac3e716:raise ValueError('unsupported native PE identity')
        for rva,(size,digest) in FUNCTIONS.items():
            if hashlib.sha256(self.pe.get_data(rva,size)).hexdigest()!=digest:raise ValueError('changed native math helper')
        self.mu=uc.Uc(uc.UC_ARCH_X86,uc.UC_MODE_64);self.pages=set();self.visited=set();self.error=None;self.calls=0;self.instructions=0;self.cache_add_phase_checks=0
        self.mu.mem_map(0x50000000,0x100000,uc.UC_PROT_READ|uc.UC_PROT_WRITE)
        self.mu.mem_map(0x70000000,0x20000,uc.UC_PROT_READ|uc.UC_PROT_WRITE)
        self.mu.mem_map(0x60000000,0x1000,uc.UC_PROT_READ)
        self.mu.hook_add(uc.UC_HOOK_MEM_UNMAPPED,self._unmapped)
        self.mu.hook_add(uc.UC_HOOK_CODE,self._code)
        # Replace one pointer in EMULATED memory only, to controlled native globals.
        self._page(self.base+0x4f7c6c8)
        self.mu.mem_write(self.base+0x4f7c6c8,struct.pack('<Q',0x50030000))
    def _page(self,address):
        page=address&~0xfff
        if page in self.pages:return
        if not self.base<=page<self.base+self.pe.OPTIONAL_HEADER.SizeOfImage:raise ValueError('image page outside clone')
        self.mu.mem_map(page,0x1000,self.uc.UC_PROT_READ|self.uc.UC_PROT_EXEC)
        self.mu.mem_write(page,self.pe.get_data(page-self.base,0x1000).ljust(0x1000,b'\0'));self.pages.add(page)
    def _unmapped(self,mu,access,address,size,value,user):
        if not self.base<=address<address+size<=self.base+self.pe.OPTIONAL_HEADER.SizeOfImage:return False
        try:
            for page in range(address&~0xfff,(address+size+0xfff)&~0xfff,0x1000):self._page(page)
            return True
        except Exception as error:self.error=str(error);return False
    def _code(self,mu,address,size,user):
        self.instructions+=1;rva=address-self.base
        for start,(length,_) in FUNCTIONS.items():
            if start<=rva and rva+size<=start+length:self.visited.add(start);return
        self.error='unexpected native instruction '+hex(address);mu.emu_stop()
    def call(self,rva,*args):
        if rva not in FUNCTIONS or len(args)>4:raise ValueError('unapproved native math entry')
        self.error=None;rsp=0x7001fef8;self.mu.mem_write(rsp,struct.pack('<Q',0x60000000));self.mu.reg_write(self.x.UC_X86_REG_RSP,rsp)
        self.mu.reg_write(self.x.UC_X86_REG_MXCSR,0x1f80)
        for reg,value in zip((self.x.UC_X86_REG_RCX,self.x.UC_X86_REG_RDX,self.x.UC_X86_REG_R8,self.x.UC_X86_REG_R9),(*args,0,0,0,0)):self.mu.reg_write(reg,value)
        self._page(self.base+rva);self.calls+=1
        try:self.mu.emu_start(self.base+rva,0x60000000,timeout=1000000,count=20000)
        except self.uc.UcError as error:raise RuntimeError(f'{error} at {self.mu.reg_read(self.x.UC_X86_REG_RIP):x}') from error
        if self.error or self.mu.reg_read(self.x.UC_X86_REG_RIP)!=0x60000000:raise RuntimeError(self.error or 'native instruction/time limit')
    def matrix(self,address,value=None):
        if value is not None:
            if len(value)!=16 or not all(map(math.isfinite,value)):raise ValueError('finite matrix required')
            self.mu.mem_write(address,struct.pack('<16f',*value))
        return list(struct.unpack('<16f',self.mu.mem_read(address,64)))
    def multiply(self,a,b):
        self.matrix(0x50010000,a);self.matrix(0x50011000,b);self.call(0x1ab9060,0x50010000,0x50011000,0x50012000)
        return self.matrix(0x50012000)
    def inverse(self,a):
        self.matrix(0x50010000,a);self.call(0x27a1fc0,0x50010000,0x50012000);return self.matrix(0x50012000)
    def depth_free(self,a):
        self.matrix(0x50010000,a);self.call(0x1f8f740,0x50010000,0x50012000);return self.matrix(0x50012000)
    def packet(self,world,view,p,jitter,width,height,scale,depth,previous=None):
        self.mu.mem_write(ENTRY,bytes(0xa60));self.matrix(ENTRY+0x34,world)
        later_copy=[float(-17-i) for i in range(16)];self.matrix(ENTRY+0x49c,later_copy)
        self.matrix(0x50010000,view);self.matrix(0x50011000,p)
        self.call(0x1d75d30,ENTRY,0x50010000,0x50011000,0)
        if self.matrix(ENTRY+0x45c)!=list(map(f32,p)) or self.matrix(ENTRY+0x49c)!=later_copy:raise ValueError('native add/cache projection phase changed')
        self.cache_add_phase_checks+=1
        for dest,source in ((0x49c,0x45c),(0x51c,0x4dc),(0x59c,0x55c),(0x65c,0x61c)):
            self.matrix(ENTRY+dest,self.matrix(ENTRY+source))
        j=IDENTITY.copy();j[12]=f32(f32(2*jitter[0])/width);j[13]=f32(f32(-2*jitter[1])/height)
        if previous is None:
            previous={offset:self.matrix(ENTRY+offset) for offset in (0x34,0x3dc,0x49c,0x59c)}
            previous[0x55c]=self.multiply(previous[0x59c],j)
        for dest,source in ((0x834,0x49c),(0x8f4,0x3dc),(0x934,0x59c),(0x974,0x55c)):
            self.matrix(ENTRY+dest,previous[source])
        self.mu.mem_write(0x50030034,struct.pack('<2f',scale,depth));self.call(0x18159a0,0,ENTRY)
        self.matrix(ENTRY+0x7b4,self.matrix(ENTRY+0x734))
        current_free=self.depth_free(self.matrix(ENTRY+0x59c));prior_free=self.depth_free(self.matrix(ENTRY+0x934))
        self.matrix(ENTRY+0x9b4,transpose(self.multiply(self.inverse(current_free),prior_free)))
        self.matrix(ENTRY+0x9f4,transpose(self.multiply(self.inverse(prior_free),current_free)))
        for offset in (0x45c,0x55c,0x61c,0x6b4,0x734,0x7f4,0x934):self.matrix(ENTRY+offset,self.multiply(self.matrix(ENTRY+offset),j))
        for dest,source in ((0x4dc,0x45c),(0x6f4,0x6b4)):self.matrix(ENTRY+dest,self.inverse(self.matrix(ENTRY+source)))
        return {offset:self.matrix(ENTRY+offset) for offset in OFFSETS}

def calibrate(sandbox,root):
    captured=[json.loads((root/f'second_{i}.json').read_text())['scenes'][0]['packet_before'] for i in range(4)]
    results=[]
    for index in range(1,4):
        actual={int(k,16):floats(v) for k,v in captured[index]['matrix_words'].items()}
        previous={int(k,16):floats(v) for k,v in captured[index-1]['matrix_words'].items()}
        p,aux=actual[0x49c],actual[0x6b4]
        scale=f32(aux[0]);depth=f32((1-aux[10])/(1-p[10])) # Same inferred baseline coefficients as the existing native fixture.
        result=sandbox.packet(actual[0x34],actual[0x3dc],p,floats(captured[index]['jitter_words']),1920,1080,scale,depth,previous)
        errors={hex(offset):max(abs(a-b) for a,b in zip(result[offset],actual[offset])) for offset in OFFSETS}
        passed=all(error<=(1e-3 if int(offset,16) in (0x9b4,0x9f4) else 3e-4) for offset,error in errors.items())
        results.append(dict(index=index,pass_=passed,errors=errors))
    return results

def generate(sandbox,cases=None):
    if cases is None:cases=[]
    for scenario in range(8):
        for eye in range(2):
            previous=None;prior=-1
            for frame in range(2):
                fov=tuple(map(f32,((-1.2,.8,-.7,1.1),(-.8,1.2,-1.1,.7))[eye]))
                near,far=(.05,500.) if scenario&1 else (.1,1000.)
                width,height=(2016,2240) if scenario&2 else (1920,1080)
                angle=.15*scenario+.03*frame+.05*eye;c,s=f32(math.cos(angle)),f32(math.sin(angle))
                world=[c,0.,-s,0.,0.,1.,0.,0.,-s,0.,-c,0.,f32(10+.02*frame+.064*eye),f32(20+.03*frame),f32(30-.01*frame),1.]
                view=IDENTITY.copy()
                for r in range(3):
                    for col in range(3):view[r*4+col]=world[col*4+r]
                for col in range(3):view[12+col]=f32(-sum(world[12+k]*world[col*4+k] for k in range(3)))
                jitter=tuple(map(f32,((.25,-.125),(-.375,.25))[frame]));scale=f32(.8 if scenario&4 else 1.1);depth=f32(.25 if scenario&1 else .75)
                p=projection(fov,near,far);expected=sandbox.packet(world,view,p,jitter,width,height,scale,depth,previous)
                cases.append(dict(scenario=scenario,eye=eye,frame=frame,previous=prior,world=words(world),view=words(view),projection=words(p),fov=words(fov),near=words([near])[0],far=words([far])[0],jitter=words(jitter),width=width,height=height,scale=words([scale])[0],depth=words([depth])[0],expected={hex(k):words(v) for k,v in expected.items()}))
                previous=expected;prior=len(cases)-1
    return cases

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--exe',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if not args.exe.is_absolute() or not args.output.is_absolute():parser.error('absolute paths required')
    args.output.mkdir(parents=True,exist_ok=False)
    report=dict(model='offline_native_camera_math_oracle_v1',pass_=False,actual_native_code_emulated=False,synthetic_inputs=True,main_schedule_modeled=True,actual_game_run=False,native_scene_verified=False,native_gpu_histories_verified=False,gpu_pair_verified=False,runtime_submission=False,headset_ready=False,expected_exe_sha256=EXE_SHA256)
    sandbox=None
    try:
        sandbox=Sandbox(args.exe);report['exe_sha256']=sandbox.exe_sha256
        root=Path(__file__).resolve().parents[1]/'docs/validation/ordinary-native-temporal-packet-live-2026-10-17'
        report['calibration']=calibrate(sandbox,root)
        if not all(row['pass_'] for row in report['calibration']):raise ValueError('native helper schedule fails recorded symmetric baseline')
        report['cases']=[];generate(sandbox,report['cases']);report['pass_']=True
    except Exception as error:report['error']=str(error)
    if sandbox:
        report['actual_native_code_emulated']=sandbox.instructions>0
        report['functions']={hex(k):dict(bytes=v[0],sha256=v[1]) for k,v in FUNCTIONS.items()}
        report['visited']=[hex(v) for v in sorted(sandbox.visited)];report['calls']=sandbox.calls;report['instructions']=sandbox.instructions
        report['cache_add_phase_checks']=sandbox.cache_add_phase_checks
    (args.output/'oracle.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('offline native camera math oracle', 'PASS' if report['pass_'] else 'FAIL',len(report.get('cases',[])),'generated cases',report.get('error',''))
    return 0 if report['pass_'] else 1
if __name__=='__main__':raise SystemExit(main())
