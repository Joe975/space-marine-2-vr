#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace sm2_dxgi_proxy {
enum class Symbol:size_t {factory,factory1,factory2,debug1,count};
inline constexpr std::array<std::string_view,4> names{"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2","DXGIGetDebugInterface1"};
struct PreserveError {DWORD value{GetLastError()};~PreserveError(){SetLastError(value);}};
inline bool requested(std::string_view value,bool loaded){return value=="1" && loaded;}
inline bool enabled(){char flag[2]{};const auto n=GetEnvironmentVariableA("SM2VR_RENDERDOC_PROXY_BRIDGE",flag,2);return n==1 && requested({flag,n},GetModuleHandleW(L"renderdoc.dll")!=nullptr);}
template<class Query> bool exact_caller(uintptr_t address,HMODULE renderdoc,Query&& query){HMODULE caller{};return renderdoc && address && query(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&caller) && caller==renderdoc;}
inline bool renderdoc_caller(uintptr_t address){PreserveError error;return exact_caller(address,GetModuleHandleW(L"renderdoc.dll"),GetModuleHandleExW);}
inline std::wstring system_path(){wchar_t path[MAX_PATH]{};const auto n=GetSystemDirectoryW(path,MAX_PATH);return n && n<MAX_PATH?std::wstring{path,n}+L"\\dxgi.dll":std::wstring{};}
inline HMODULE system_module(){
    // No function-static initialisation lock spans LoadLibrary: its callbacks
    // can reenter a factory. Successful loads stay pinned for process lifetime.
    static std::atomic<HMODULE> cached{};auto module=cached.load();if(module)return module;
    const auto path=system_path();if(path.empty())return nullptr;module=LoadLibraryW(path.c_str());if(module)cached.store(module);return module;
}
inline bool system_identity(HMODULE module){
    const auto expected=system_path();wchar_t actual[MAX_PATH]{};const auto n=GetModuleFileNameW(module,actual,MAX_PATH);MEMORY_BASIC_INFORMATION info{};
    return module && !expected.empty() && n && n<MAX_PATH && _wcsicmp(actual,expected.c_str())==0 &&
        VirtualQuery(module,&info,sizeof(info))==sizeof(info) && info.Type==MEM_IMAGE && info.AllocationBase==module;
}

// Only a direct executable export in this loaded AMD64 image is eligible. No
// forwarded export, ordinal lookup, unbounded name, GetProcAddress or code write.
template<class Read> FARPROC raw_export(uintptr_t base,std::string_view name,Read&& reader){
    if(!base || base>UINTPTR_MAX-0x4000000 || name.empty() || name.size()>32)return nullptr;
    bool allowed=false;for(const auto item:names)allowed|=item==name;if(!allowed)return nullptr;
    size_t reads{},bytes{};
    const auto copy=[&](uintptr_t p,void* out,size_t n){if(reads>=4096 || !n || n>512 || n>65536-bytes || p<0x10000 || p>UINTPTR_MAX-n)return false;++reads;bytes+=n;return reader(p,out,n);};
    IMAGE_DOS_HEADER dos{};if(!copy(base,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<int(sizeof(dos)) || dos.e_lfanew>0x100000)return nullptr;
    IMAGE_NT_HEADERS64 nt{};if(!copy(base+dos.e_lfanew,&nt,sizeof(nt)) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64) || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections>96 ||
        nt.OptionalHeader.SizeOfImage<4096 || nt.OptionalHeader.SizeOfImage>0x4000000 || nt.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_EXPORT)return nullptr;
    const auto size=nt.OptionalHeader.SizeOfImage;
    const auto span=[&](uint64_t rva,uint64_t n){return rva && n && rva<size && n<=size-rva;};
    const uint64_t sections=uint64_t(dos.e_lfanew)+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
    if(!span(sections,uint64_t(nt.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER)) || sections+uint64_t(nt.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER)>nt.OptionalHeader.SizeOfHeaders || nt.OptionalHeader.SizeOfHeaders>size)return nullptr;
    const auto dir=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];IMAGE_EXPORT_DIRECTORY exp{};
    if(dir.Size<sizeof(exp) || !span(dir.VirtualAddress,dir.Size) || !copy(base+dir.VirtualAddress,&exp,sizeof(exp)) || !exp.NumberOfFunctions || exp.NumberOfFunctions>4096 ||
        !exp.NumberOfNames || exp.NumberOfNames>1024 || exp.NumberOfNames>exp.NumberOfFunctions || !span(exp.AddressOfFunctions,uint64_t(exp.NumberOfFunctions)*4) ||
        !span(exp.AddressOfNames,uint64_t(exp.NumberOfNames)*4) || !span(exp.AddressOfNameOrdinals,uint64_t(exp.NumberOfNames)*2))return nullptr;
    for(uint32_t i=0;i<exp.NumberOfNames;++i){uint32_t rva{};std::array<char,33> text{};
        if(!copy(base+exp.AddressOfNames+size_t(i)*4,&rva,4) || !span(rva,name.size()+1) || !copy(base+rva,text.data(),name.size()+1))return nullptr;
        if(std::memcmp(text.data(),name.data(),name.size()) || text[name.size()]!='\0')continue;
        uint16_t ordinal{};uint32_t target{};
        if(!copy(base+exp.AddressOfNameOrdinals+size_t(i)*2,&ordinal,2) || ordinal>=exp.NumberOfFunctions || !copy(base+exp.AddressOfFunctions+size_t(ordinal)*4,&target,4) ||
            !span(target,1) || (target>=dir.VirtualAddress && uint64_t(target)<uint64_t(dir.VirtualAddress)+dir.Size))return nullptr;
        for(unsigned j=0;j<nt.FileHeader.NumberOfSections;++j){IMAGE_SECTION_HEADER section{};
            if(!copy(base+sections+size_t(j)*sizeof(section),&section,sizeof(section)))return nullptr;
            if((section.Characteristics&IMAGE_SCN_MEM_EXECUTE) && span(section.VirtualAddress,section.Misc.VirtualSize) && target>=section.VirtualAddress && uint64_t(target)<uint64_t(section.VirtualAddress)+section.Misc.VirtualSize)return reinterpret_cast<FARPROC>(base+target);
        }
        return nullptr;
    }
    return nullptr;
}
inline FARPROC system_export(HMODULE module,const char* name){
    if(!system_identity(module))return nullptr;
    const auto target=raw_export(reinterpret_cast<uintptr_t>(module),name,[](uintptr_t p,void* out,size_t n){SIZE_T got{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,n,&got) && got==n;});
    MEMORY_BASIC_INFORMATION info{};if(!target || VirtualQuery(reinterpret_cast<void*>(target),&info,sizeof(info))!=sizeof(info) || info.State!=MEM_COMMIT || info.Type!=MEM_IMAGE || info.AllocationBase!=module || (info.Protect&PAGE_GUARD))return nullptr;
    const auto protection=info.Protect&0xff;return protection==PAGE_EXECUTE || protection==PAGE_EXECUTE_READ || protection==PAGE_EXECUTE_READWRITE || protection==PAGE_EXECUTE_WRITECOPY?target:nullptr;
}
struct Api {HMODULE(*module)(){system_module};FARPROC(WINAPI* hooked)(HMODULE,LPCSTR){GetProcAddress};FARPROC(*raw)(HMODULE,const char*){system_export};bool(*active)(){enabled};bool(*renderdoc_origin)(uintptr_t){renderdoc_caller};};
inline Api& api(){static Api value;return value;}
inline thread_local std::array<unsigned,4> depths{};
struct Depth {unsigned* value{};explicit Depth(unsigned* p):value{p}{if(value)++*value;}~Depth(){if(value)--*value;}};

#ifndef SM2VR_DXGI_PROXY_LOG
#define SM2VR_DXGI_PROXY_LOG(name,depth) ((void)0)
#endif
template<class Log> void capture_route(const char* name,unsigned depth,uintptr_t caller,const char* route,Log log){
    PreserveError error;static std::atomic<uint64_t> records{};if(records.fetch_add(1)>=64)return;
    try{HMODULE owner{};char path[MAX_PATH]{"<unknown>"};if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(caller),&owner))if(!GetModuleFileNameA(owner,path,MAX_PATH))std::memcpy(path,"<unavailable>",14);path[MAX_PATH-1]=0;log(name,depth,caller,route,path);}catch(...){/* Optional capture metadata never controls forwarding. */}
}
#ifndef SM2VR_DXGI_PROXY_ROUTE
#define SM2VR_DXGI_PROXY_ROUTE(name,depth,caller,route) ((void)0)
#endif
template<Symbol S,class Fn,class... Args> HRESULT forward(uintptr_t caller_address,Args&&... args){
    PreserveError error;constexpr size_t index=static_cast<size_t>(S);const auto name=names[index].data();
    // Once entered, the guard remains active even if the environment changes in
    // an inner call. Depth is independent for each symbol and operating thread.
    const bool bridge=depths[index]!=0 || api().active();const auto prior=depths[index];Depth depth{bridge?&depths[index]:nullptr};
    if(bridge && prior>=2)return E_FAIL;
    SM2VR_DXGI_PROXY_LOG(name,bridge?prior:0);
    auto module=api().module();if(!module)return E_FAIL;Fn target{};
    // RenderDoc's saved-original call returns to its hook before HandleWrap.
    // That exact module origin must bypass GPA to avoid another wrapper layer.
    if(bridge){const bool origin=!prior && api().renderdoc_origin(caller_address);SM2VR_DXGI_PROXY_ROUTE(name,prior,caller_address,prior?"recursive_raw":origin?"renderdoc_raw":"intercepted");target=reinterpret_cast<Fn>((prior || origin)?api().raw(module,name):api().hooked(module,name));}
    else {static const auto normal=reinterpret_cast<Fn>(api().hooked(module,name));target=normal;}
    if(!target)return E_FAIL;SetLastError(error.value);HRESULT returned{};
    try{returned=target(std::forward<Args>(args)...);}catch(...){error.value=GetLastError();throw;}
    error.value=GetLastError();return returned;
}
}
