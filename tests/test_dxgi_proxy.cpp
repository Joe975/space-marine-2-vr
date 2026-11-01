#include <windows.h>
#include <stdexcept>
static void(*log_delegate)(){};
static void log_proxy(const char*,unsigned){if(log_delegate)log_delegate();SetLastError(991);}
static void log_route(const char*,unsigned,uintptr_t,const char*,const char*);
#define SM2VR_DXGI_PROXY_LOG(name,depth) log_proxy(name,depth)
#define SM2VR_DXGI_PROXY_ROUTE(name,depth,caller,route) sm2_dxgi_proxy::capture_route(name,depth,caller,route,log_route)
#define USE_DXGI_HOOK
#include "DxgiProxy.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
namespace {
struct State {bool capture{},loaded{},raw_loop{},missing{},null_module{},throw_native{},nested{},disable_inner{},parallel{},renderdoc_origin{};unsigned hooked{},raw{},native{},proxy{};UINT flags{};const IID* iid{};void** out{};DWORD incoming{42};};
thread_local State state;
thread_local std::string last_route;
std::atomic<unsigned> entered{};
IID iid{0x12345678,9,10,{1,2,3,4,5,6,7,8}};
constexpr HRESULT result=HRESULT(0x87654321);
HRESULT invoke(unsigned i,UINT flags,REFIID id,void** out){
    switch(i){case 0:return PX_CreateDXGIFactory(id,out);case 1:return PX_CreateDXGIFactory1(id,out);case 2:return PX_CreateDXGIFactory2(flags,id,out);default:return PX_DXGIGetDebugInterface1(flags,id,out);}
}
HRESULT native(unsigned i,UINT flags,REFIID id,void** out){
    ++state.native;check(&id==state.iid && out==state.out && flags==state.flags,"full ABI arguments unchanged");if(GetLastError()!=state.incoming){std::cerr<<"symbol "<<i<<" native="<<state.native<<" hooked="<<state.hooked<<" raw="<<state.raw<<" proxy="<<state.proxy<<" error="<<GetLastError()<<" expected="<<state.incoming<<" nested="<<state.nested<<" logging="<<(log_delegate!=nullptr)<<"\n";throw std::runtime_error("native incoming error preserved through lookup/logging");}
    if(state.parallel){++entered;auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(entered.load()<2 && std::chrono::steady_clock::now()<until)std::this_thread::yield();check(entered==2,"parallel calls overlap");}
    if(state.nested && i==0){state.nested=false;check(invoke(1,0,id,out)==result,"nested other symbol succeeds");}
    SetLastError(73);if(state.throw_native)throw std::runtime_error("native throw");*out=reinterpret_cast<void*>(0x1234);return result;
}
HRESULT intercepted(unsigned i,UINT flags,REFIID id,void** out){
    ++state.proxy;if(state.proxy>8)throw std::runtime_error("unbounded recursion");
    if(state.capture && state.loaded){if(state.disable_inner){state.capture=false;state.disable_inner=false;}return invoke(i,flags,id,out);}return native(i,flags,id,out);
}
#define FUNCTIONS(n) HRESULT WINAPI h##n(REFIID id,void** out){return intercepted(n,0,id,out);} HRESULT WINAPI r##n(REFIID id,void** out){return native(n,0,id,out);}
FUNCTIONS(0) FUNCTIONS(1)
#define FUNCTIONS_FLAGS(n) HRESULT WINAPI h##n(UINT f,REFIID id,void** out){return intercepted(n,f,id,out);} HRESULT WINAPI r##n(UINT f,REFIID id,void** out){return native(n,f,id,out);}
FUNCTIONS_FLAGS(2) FUNCTIONS_FLAGS(3)
FARPROC pointer(const char* name,bool raw){for(unsigned i=0;i<4;++i)if(sm2_dxgi_proxy::names[i]==name){FARPROC a[]{reinterpret_cast<FARPROC>(h0),reinterpret_cast<FARPROC>(h1),reinterpret_cast<FARPROC>(h2),reinterpret_cast<FARPROC>(h3)};FARPROC b[]{reinterpret_cast<FARPROC>(r0),reinterpret_cast<FARPROC>(r1),reinterpret_cast<FARPROC>(r2),reinterpret_cast<FARPROC>(r3)};return raw?b[i]:a[i];}return nullptr;}
HMODULE module(){SetLastError(992);return state.null_module?nullptr:reinterpret_cast<HMODULE>(0x10000);}
FARPROC WINAPI hooked(HMODULE,const char* name){++state.hooked;SetLastError(993);return pointer(name,false);}
FARPROC raw(HMODULE,const char* name){++state.raw;SetLastError(994);return state.missing?nullptr:pointer(name,!state.raw_loop);}
bool active(){SetLastError(995);return state.capture && state.loaded;}
bool origin(uintptr_t address){HMODULE actual{};check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&actual) && actual==GetModuleHandleW(nullptr),"actual exported-entry caller address belongs to CPU fixture");SetLastError(996);return sm2_dxgi_proxy::exact_caller(address,reinterpret_cast<HMODULE>(1),[](DWORD flags,LPCWSTR pointer,HMODULE* out){check(flags==(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) && pointer,"exact module query uses address without retaining ref");*out=reinterpret_cast<HMODULE>(uintptr_t(state.renderdoc_origin?1:2));return TRUE;});}
void reset(void** out,UINT flags=0){state={};state.iid=&iid;state.out=out;state.flags=flags;SetLastError(42);}
void recursive_log(){const auto saved=state.incoming;state.incoming=GetLastError();const auto r=invoke(0,0,*state.iid,state.out);state.incoming=saved;check(r==result || r==E_FAIL,"recursive logger bounded");}
void exports(){
    sm2_dxgi_proxy::api()={module,hooked,raw,active,origin};
    for(unsigned i=0;i<4;++i){void* out{};const UINT flags=i>=2?0xfedcba98:0;reset(&out,flags);check(invoke(i,flags,iid,&out)==result && state.native==1 && state.hooked==1 && !state.raw && out==reinterpret_cast<void*>(0x1234) && GetLastError()==73,"default export forwards exactly once");
        reset(&out,flags);state.capture=true;check(invoke(i,flags,iid,&out)==result && state.native==1 && !state.hooked && !state.raw,"request without RenderDoc retains normal cached path");
        reset(&out,flags);state.capture=state.loaded=true;check(invoke(i,flags,iid,&out)==result && state.native==1 && state.hooked==1 && state.raw==1 && state.proxy==1 && GetLastError()==73 && last_route=="recursive_raw","capture recursion uses raw system export exactly once");
        reset(&out,flags);state.capture=state.loaded=state.renderdoc_origin=true;check(invoke(i,flags,iid,&out)==result && state.native==1 && state.hooked==0 && state.raw==1 && state.proxy==0 && GetLastError()==73 && last_route=="renderdoc_raw","RenderDoc saved-original origin avoids second hook/wrapper layer");
        reset(&out,flags);state.capture=state.loaded=state.raw_loop=true;check(invoke(i,flags,iid,&out)==E_FAIL && !state.native && state.proxy==2 && state.raw==1 && GetLastError()==42,"second recursion refuses without native call");
        reset(&out,flags);state.capture=state.loaded=state.missing=true;out=nullptr;check(invoke(i,flags,iid,&out)==E_FAIL && !state.native && !out && GetLastError()==42,"missing raw export refuses unchanged");
    }
    void* out{};reset(&out);state.capture=state.loaded=state.nested=true;check(invoke(0,0,iid,&out)==result && state.native==2 && state.raw==2,"cross symbol recursion uses independent depths");
    reset(&out);state.capture=state.loaded=state.throw_native=true;bool threw=false;try{invoke(0,0,iid,&out);}catch(...){threw=true;}check(threw && state.native==1 && GetLastError()==73,"native exception and outgoing error propagate");check(sm2_dxgi_proxy::depths==std::array<unsigned,4>{},"depth unwinds after native exception");
    reset(&out);state.capture=state.loaded=state.disable_inner=true;check(invoke(0,0,iid,&out)==result && state.raw==1,"active recursion stays guarded when request changes");
    reset(&out);state.capture=state.loaded=state.null_module=true;check(invoke(0,0,iid,&out)==E_FAIL && !state.native && GetLastError()==42,"missing module fails preserving input error");
    reset(&out);state.capture=state.loaded=true;log_delegate=recursive_log;check(invoke(0,0,iid,&out)==result && state.native==2 && GetLastError()==73,"recursive logging bounded before logging callback at cutoff");log_delegate=nullptr;
    std::exception_ptr errors[2];std::thread threads[2];for(unsigned i=0;i<2;++i)threads[i]=std::thread([&,i]{try{void* local{};reset(&local);state.capture=state.loaded=state.parallel=true;check(invoke(0,0,iid,&local)==result && state.native==1 && state.raw==1,"parallel TLS recursion isolated");}catch(...){errors[i]=std::current_exception();}});for(auto& t:threads)t.join();for(auto& e:errors)if(e)std::rethrow_exception(e);
}
struct Image {
    std::vector<unsigned char> bytes=std::vector<unsigned char>(0x3000);
    template<class T>T& at(size_t offset){return *reinterpret_cast<T*>(bytes.data()+offset);}
    Image(){auto& d=at<IMAGE_DOS_HEADER>(0);d.e_magic=IMAGE_DOS_SIGNATURE;d.e_lfanew=0x80;auto& n=at<IMAGE_NT_HEADERS64>(0x80);n.Signature=IMAGE_NT_SIGNATURE;n.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;n.FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);n.FileHeader.NumberOfSections=1;n.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;n.OptionalHeader.SizeOfHeaders=0x400;n.OptionalHeader.SizeOfImage=0x3000;n.OptionalHeader.NumberOfRvaAndSizes=16;n.OptionalHeader.DataDirectory[0]={0x400,0x400};auto& s=at<IMAGE_SECTION_HEADER>(0x80+sizeof(n));s.VirtualAddress=0x1000;s.Misc.VirtualSize=0x1000;s.Characteristics=IMAGE_SCN_MEM_EXECUTE;auto& e=at<IMAGE_EXPORT_DIRECTORY>(0x400);e.NumberOfFunctions=e.NumberOfNames=4;e.AddressOfFunctions=0x500;e.AddressOfNames=0x540;e.AddressOfNameOrdinals=0x580;for(unsigned i=0;i<4;++i){at<DWORD>(0x500+i*4)=0x1000+i*16;at<DWORD>(0x540+i*4)=0x600+i*0x40;at<WORD>(0x580+i*2)=WORD(i);std::memcpy(bytes.data()+0x600+i*0x40,sm2_dxgi_proxy::names[i].data(),sm2_dxgi_proxy::names[i].size()+1);}}
    FARPROC resolve(std::string_view name="CreateDXGIFactory",bool fail=false){const auto base=reinterpret_cast<uintptr_t>(bytes.data());return sm2_dxgi_proxy::raw_export(base,name,[&](uintptr_t p,void* out,size_t n){if(fail || p<base || n>bytes.size() || p-base>bytes.size()-n)return false;std::memcpy(out,reinterpret_cast<void*>(p),n);return true;});}
};
void parser(){
    Image good;for(unsigned i=0;i<4;++i)check(good.resolve(sm2_dxgi_proxy::names[i])==reinterpret_cast<FARPROC>(good.bytes.data()+0x1000+i*16),"bounded direct executable export resolves");
    check(!good.resolve("unsupported") && !good.resolve("CreateDXGIFactory",true),"unsupported and unreadable refused");
    auto reject=[](auto change,const char* label){Image image;change(image);check(!image.resolve(),label);};
    reject([](Image& i){i.at<IMAGE_DOS_HEADER>(0).e_magic=0;},"DOS guard");reject([](Image& i){i.at<IMAGE_DOS_HEADER>(0).e_lfanew=-1;},"negative NT offset");
    reject([](Image& i){i.at<IMAGE_NT_HEADERS64>(0x80).FileHeader.Machine=0;},"machine guard");reject([](Image& i){i.at<IMAGE_NT_HEADERS64>(0x80).OptionalHeader.SizeOfImage=0xffffffff;},"image bounds");
    reject([](Image& i){i.at<IMAGE_NT_HEADERS64>(0x80).FileHeader.NumberOfSections=97;},"section cap");reject([](Image& i){i.at<IMAGE_NT_HEADERS64>(0x80).OptionalHeader.DataDirectory[0]={0x2ff0,0x100};},"directory bounds");
    reject([](Image& i){i.at<IMAGE_EXPORT_DIRECTORY>(0x400).NumberOfNames=1025;},"name cap");reject([](Image& i){i.at<IMAGE_EXPORT_DIRECTORY>(0x400).AddressOfFunctions=0x2fff;},"array bounds");
    reject([](Image& i){i.at<WORD>(0x580)=4;},"ordinal bounds");reject([](Image& i){i.at<DWORD>(0x540)=0x2fff;},"name span bounds");
    reject([](Image& i){i.at<DWORD>(0x500)=0x700;auto& s=i.at<IMAGE_SECTION_HEADER>(0x80+sizeof(IMAGE_NT_HEADERS64));s.VirtualAddress=0x400;s.Misc.VirtualSize=0x1c00;},"forwarded export refused even inside executable section");reject([](Image& i){i.at<DWORD>(0x500)=0x900;},"nonexecuting export refused");
    reject([](Image& i){i.at<DWORD>(0x500)=0x3000;},"target image bounds");
    for(auto text:{"","0","11"})check(!sm2_dxgi_proxy::requested(text,true),"explicit gate only");check(!sm2_dxgi_proxy::requested("1",false) && sm2_dxgi_proxy::requested("1",true),"loaded module gate");
    auto yes=[](DWORD,LPCWSTR,HMODULE* out){*out=reinterpret_cast<HMODULE>(1);return TRUE;};auto no=[](DWORD,LPCWSTR,HMODULE*){return FALSE;};
    check(!sm2_dxgi_proxy::exact_caller(1,nullptr,yes) && !sm2_dxgi_proxy::exact_caller(0,reinterpret_cast<HMODULE>(1),yes) && !sm2_dxgi_proxy::exact_caller(1,reinterpret_cast<HMODULE>(1),no) && !sm2_dxgi_proxy::exact_caller(1,reinterpret_cast<HMODULE>(2),yes),"unloaded wrong-module failed-origin queries refused");
}
static void route_limits(){unsigned records{};auto logger=[&](const char*,unsigned,uintptr_t,const char*,const char* path){++records;check(path && path[0],"caller module metadata present");SetLastError(123);if(records==1)throw std::runtime_error("optional log");};for(unsigned i=0;i<70;++i){SetLastError(42);sm2_dxgi_proxy::capture_route("fixture",0,reinterpret_cast<uintptr_t>(&route_limits),"intercepted",logger);check(GetLastError()==42,"optional route metadata preserves caller error including throw");}check(records==64,"capture route logging is bounded");}
}
static void log_route(const char*,unsigned,uintptr_t,const char* route,const char*){last_route=route;SetLastError(997);}
int main(){try{parser();exports();route_limits();std::cout<<"DXGI proxy CPU fixtures PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}



