#include "NativeFaultEvidence.h"
#include "GameBuild.h"
#include <algorithm>
#include <cstring>

namespace {
struct SavedLastError { DWORD value{GetLastError()}; ~SavedLastError() noexcept { SetLastError(value); } };
template<class T>bool read(uintptr_t address,T& value) noexcept {SIZE_T bytes{};return address && address<=UINTPTR_MAX-sizeof(T) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(T),&bytes) && bytes==sizeof(T);}
}
NativeFaultEvidence& NativeFaultEvidence::get(){static auto* value=new NativeFaultEvidence;return *value;}
bool NativeFaultEvidence::matches(uint32_t stamp,const std::array<uint8_t,16>& function,const std::array<uint8_t,7>& fault,Image image){const auto which=image==Image::dll?game_build::Image::dll:game_build::Image::exe;
    return game_build::code(stamp,image==Image::dll?FUNCTION_RVA:EXE_FUNCTION_RVA,function,image==Image::dll?FUNCTION_BYTES:EXE_FUNCTION_BYTES,which) &&
        game_build::code(stamp,image==Image::dll?RVA:EXE_RVA,fault,image==Image::dll?FAULT_BYTES:EXE_FAULT_BYTES,which);}
NativeFaultEvidence::Region NativeFaultEvidence::region(uintptr_t address) noexcept {
    Region value;value.address=address;MEMORY_BASIC_INFORMATION info{};if(!address || !VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info)))return value;
    value.allocation=reinterpret_cast<uintptr_t>(info.AllocationBase);value.base=reinterpret_cast<uintptr_t>(info.BaseAddress);value.region_size=info.RegionSize;value.state=info.State;value.protect=info.Protect;value.type=info.Type;
    if(info.State!=MEM_COMMIT || (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) || !info.Protect || value.base>address || info.RegionSize>UINTPTR_MAX-value.base || address>=value.base+info.RegionSize)return value;
    const auto count=std::min<size_t>(value.bytes.size(),value.base+info.RegionSize-address);SIZE_T copied{};
    if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),value.bytes.data(),count,&copied) && copied==count)value.size=uint32_t(count);
    else value.bytes.fill(0);return value;
}
bool NativeFaultEvidence::collect(const EXCEPTION_POINTERS* exception,uintptr_t module,Evidence& out,Image image) noexcept {
    const auto rva=image==Image::dll?(s_dll_rva?s_dll_rva:RVA):(s_exe_rva?s_exe_rva:EXE_RVA);
    if(!exception || !exception->ExceptionRecord || !exception->ContextRecord || !module || module>UINTPTR_MAX-rva)return false;
    const auto& fault=*exception->ExceptionRecord;const auto& context=*exception->ContextRecord;
    if(fault.ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || fault.NumberParameters!=2 || fault.ExceptionInformation[0]!=0 || context.Rip!=module+rva || reinterpret_cast<uintptr_t>(fault.ExceptionAddress)!=context.Rip || context.R8>UINTPTR_MAX-0xc0 || fault.ExceptionInformation[1]!=context.R8+0xc0)return false;
    out=Evidence{};out.timestamp=image==Image::dll?(s_dll_stamp?s_dll_stamp:TIMESTAMP):(s_exe_stamp?s_exe_stamp:EXE_TIMESTAMP);out.size=sizeof(out);out.pid=GetCurrentProcessId();out.thread=GetCurrentThreadId();out.code=fault.ExceptionCode;out.module=module;out.rip=context.Rip;out.fault_address=fault.ExceptionInformation[1];out.operation=fault.ExceptionInformation[0];out.rbx=context.Rbx;out.rcx=context.Rcx;out.r8=context.R8;out.rsp=context.Rsp;
    FILETIME time{};GetSystemTimeAsFileTime(&time);out.filetime=(uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
    out.regions[0]=region(context.Rbx);out.regions[1]=region(context.Rsp);out.regions[2]=region(context.R8);
    uintptr_t vtable{},packed{};const auto& object=out.regions[0];if(object.size>=8)std::memcpy(&vtable,object.bytes.data(),8);if(object.size>=0xe0)std::memcpy(&packed,object.bytes.data()+0xd8,8);
    out.regions[3]=region(vtable);out.regions[4]=region(packed&0xffffffffffffull);return true;
}
bool NativeFaultEvidence::write_new(const wchar_t* path,const Evidence& value) noexcept {
    if(!path || !*path || value.magic!=MAGIC || value.size!=sizeof(Evidence))return false;
    const auto file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written{};const bool ok=WriteFile(file,&value,sizeof(value),&written,nullptr) && written==sizeof(value);if(ok)FlushFileBuffers(file);CloseHandle(file);return ok;
}
bool NativeFaultEvidence::arm(HMODULE module,const std::filesystem::path& path,HMODULE executable){
    if(m_handler || !path.is_absolute() || !std::filesystem::is_directory(path.parent_path()) || std::filesystem::exists(path))return false;
    const auto guarded=[](HMODULE handle,Image image){
        const auto base=reinterpret_cast<uintptr_t>(handle);
        IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};std::array<uint8_t,16> function{};std::array<uint8_t,7> fault{};
        if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE)return false;
        const auto which=image==Image::dll?game_build::Image::dll:game_build::Image::exe;const auto stamp=nt.FileHeader.TimeDateStamp;
        const auto rva=game_build::rva(stamp,image==Image::dll?RVA:EXE_RVA,which),function_rva=game_build::rva(stamp,image==Image::dll?FUNCTION_RVA:EXE_FUNCTION_RVA,which);
        if(!rva || !function_rva)return false;
        if(image==Image::dll){s_dll_rva=rva;s_dll_stamp=stamp;}else{s_exe_rva=rva;s_exe_stamp=stamp;}
        return read(base,dos) && dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>=0 && dos.e_lfanew<=0x100000 && base<=UINTPTR_MAX-size_t(dos.e_lfanew) && read(base+dos.e_lfanew,nt) && nt.Signature==IMAGE_NT_SIGNATURE && nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC && nt.OptionalHeader.SizeOfImage>=rva+fault.size() && base<=UINTPTR_MAX-nt.OptionalHeader.SizeOfImage && read(base+function_rva,function) && read(base+rva,fault) && matches(nt.FileHeader.TimeDateStamp,function,fault,image);
    };
    if(!guarded(module,Image::dll) || (executable && !guarded(executable,Image::executable)))return false;
    NativeFaultEvidence* expected{};m_module=reinterpret_cast<uintptr_t>(module);m_executable=reinterpret_cast<uintptr_t>(executable);m_path=path.wstring();if(!s_owner.compare_exchange_strong(expected,this))return false;
    m_handler=AddVectoredExceptionHandler(0,&observe);if(!m_handler){s_owner.store(nullptr);return false;}return true;
}
NativeFaultEvidence::ArmResult NativeFaultEvidence::poll(HMODULE module,const std::filesystem::path& path,HMODULE executable){
    if(m_arm_attempted)return m_handler?ArmResult::armed:ArmResult::refused;
    if(!module)return ArmResult::waiting;
    m_arm_attempted=true;return arm(module,path,executable)?ArmResult::armed:ArmResult::refused;
}
LONG WINAPI NativeFaultEvidence::observe(EXCEPTION_POINTERS* exception) noexcept {
    const SavedLastError last_error;
    auto* self=s_owner.load();if(!self || self->m_attempted.load() || self->m_busy.test_and_set())return EXCEPTION_CONTINUE_SEARCH;
    Evidence value{};if((collect(exception,self->m_module,value) || collect(exception,self->m_executable,value,Image::executable)) && !self->m_attempted.exchange(true))write_new(self->m_path.c_str(),value);
    self->m_busy.clear();return EXCEPTION_CONTINUE_SEARCH;
}
