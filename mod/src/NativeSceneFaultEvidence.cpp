#include "NativeSceneFaultEvidence.h"
#include <algorithm>
#include <cstring>

namespace {
struct SavedLastError { DWORD value{GetLastError()}; ~SavedLastError() noexcept { SetLastError(value); } };
template<class T>bool read(uintptr_t address,T& value) noexcept {SIZE_T bytes{};return address && address<=UINTPTR_MAX-sizeof(T) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(T),&bytes) && bytes==sizeof(T);}
}
NativeSceneFaultEvidence& NativeSceneFaultEvidence::get(){static auto* value=new NativeSceneFaultEvidence;return *value;}
bool NativeSceneFaultEvidence::matches(uint32_t stamp,std::span<const uint8_t> helper,std::span<const uint8_t> holder,std::span<const uint8_t> caller){
    return game_build::code(stamp,HELPER,helper,HELPER_BYTES) && game_build::code(stamp,HOLDER,holder,HOLDER_BYTES) && game_build::code(stamp,CALLER,caller,CALLER_BYTES);
}
NativeSceneFaultEvidence::Region NativeSceneFaultEvidence::region(uintptr_t address) noexcept {
    Region value;value.address=address;MEMORY_BASIC_INFORMATION info{};if(!address || !VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info)))return value;
    value.allocation=reinterpret_cast<uintptr_t>(info.AllocationBase);value.base=reinterpret_cast<uintptr_t>(info.BaseAddress);value.region_size=info.RegionSize;value.state=info.State;value.protect=info.Protect;value.type=info.Type;
    if(info.State!=MEM_COMMIT || (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) || !info.Protect || value.base>address || info.RegionSize>UINTPTR_MAX-value.base || address>=value.base+info.RegionSize)return value;
    const auto count=std::min<size_t>(value.bytes.size(),value.base+info.RegionSize-address);SIZE_T copied{};
    if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),value.bytes.data(),count,&copied) && copied==count)value.size=uint32_t(count);
    else value.bytes.fill(0);return value;
}
bool NativeSceneFaultEvidence::collect(const EXCEPTION_POINTERS* exception,uintptr_t module,const Rvas& rvas,uint32_t stamp,Evidence& out) noexcept {
    if(!exception || !exception->ExceptionRecord || !exception->ContextRecord || !module || !rvas.fault || module>UINTPTR_MAX-rvas.fault)return false;
    const auto& fault=*exception->ExceptionRecord;const auto& context=*exception->ContextRecord;
    // A non-canonical pointer (the 23 Oct ASCII) faults as a general protection: read, address ~0. Not filtered on.
    if(fault.ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || context.Rip!=module+rvas.fault || reinterpret_cast<uintptr_t>(fault.ExceptionAddress)!=context.Rip)return false;
    out=Evidence{};out.size=sizeof(out);out.pid=GetCurrentProcessId();out.thread=GetCurrentThreadId();out.timestamp=stamp;out.code=fault.ExceptionCode;out.module=module;out.rip=context.Rip;
    for(DWORD i=0;i<2 && i<fault.NumberParameters;++i)out.parameters[i]=fault.ExceptionInformation[i];
    out.registers={context.Rax,context.Rcx,context.Rdx,context.Rbx,context.Rsp,context.Rbp,context.Rsi,context.Rdi,context.R8,context.R9,context.R10,context.R11,context.R12,context.R13,context.R14,context.R15};
    out.scene_renderer=s_scene.renderer;out.scene_dispatch=s_scene.dispatch;out.scene_flags=s_scene.flags;out.scene_slot=s_scene.slot;out.scene_active=s_scene.active;
    FILETIME time{};GetSystemTimeAsFileTime(&time);out.filetime=(uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
    out.regions[0]=region(context.Rsp);
    uintptr_t holder_return{},caller_return{};
    if(read(context.Rsp,holder_return) && rvas.holder_return && holder_return==module+rvas.holder_return)out.chain|=Chain::holder_return;
    if(context.Rsp<=UINTPTR_MAX-CALLER_SLOT && read(context.Rsp+CALLER_SLOT,caller_return) && rvas.caller_return && caller_return==module+rvas.caller_return)out.chain|=Chain::caller_return;
    if((out.chain&(Chain::holder_return|Chain::caller_return))==(Chain::holder_return|Chain::caller_return)){
        out.table=context.Rdi;read(context.Rsp+OBJECT_SLOT,out.object);
        int32_t index{};if(out.object<=UINTPTR_MAX-INDEX_OFFSET && read(out.object+INDEX_OFFSET,index)){out.chain|=index_read;out.index=index;
            const auto offset=(int64_t(index)+1)*int64_t(ENTRY_STRIDE);
            if(offset>=0 && out.table<=UINTPTR_MAX-uint64_t(offset)){out.entry=out.table+uint64_t(offset);read(out.entry,out.entry_value);}}
        out.regions[1]=region(out.object);out.regions[2]=region(out.table);
        if(out.entry)out.regions[3]=region(out.entry>=0x30?out.entry-0x30:out.entry);
        out.regions[4]=region(out.entry_value);
    }
    out.regions[5]=region(out.scene_renderer);
    return true;
}
bool NativeSceneFaultEvidence::write_new(const wchar_t* path,const Evidence& value) noexcept {
    if(!path || !*path || value.magic!=MAGIC || value.size!=sizeof(Evidence))return false;
    const auto file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written{};const bool ok=WriteFile(file,&value,sizeof(value),&written,nullptr) && written==sizeof(value);if(ok)FlushFileBuffers(file);CloseHandle(file);return ok;
}
bool NativeSceneFaultEvidence::arm(HMODULE executable,const std::filesystem::path& path){
    if(m_arm_attempted)return false;m_arm_attempted=true;
    if(!executable || !path.is_absolute() || !std::filesystem::is_directory(path.parent_path()) || std::filesystem::exists(path))return false;
    const auto base=reinterpret_cast<uintptr_t>(executable);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return false;
    const auto stamp=nt.FileHeader.TimeDateStamp;const auto image=nt.OptionalHeader.SizeOfImage;
    const auto helper=game_build::rva(stamp,HELPER),holder=game_build::rva(stamp,HOLDER),caller=game_build::rva(stamp,CALLER);
    const Rvas rvas{helper?helper+FAULT_OFFSET:0,game_build::rva(stamp,HOLDER_RETURN),game_build::rva(stamp,CALLER_RETURN)};
    if(!helper || !holder || !caller || !rvas.holder_return || !rvas.caller_return)return false;
    std::array<uint8_t,HELPER_BYTES.size()> helper_bytes{};std::array<uint8_t,HOLDER_BYTES.size()> holder_bytes{};std::array<uint8_t,CALLER_BYTES.size()> caller_bytes{};
    if((std::max)({helper+helper_bytes.size(),holder+holder_bytes.size(),caller+caller_bytes.size()})>image || !read(base+helper,helper_bytes) || !read(base+holder,holder_bytes) || !read(base+caller,caller_bytes) ||
        !matches(stamp,helper_bytes,holder_bytes,caller_bytes))return false;
    NativeSceneFaultEvidence* expected{};m_module=base;m_rvas=rvas;m_stamp=stamp;m_path=path.wstring();if(!s_owner.compare_exchange_strong(expected,this))return false;
    m_handler=AddVectoredExceptionHandler(0,&observe);if(!m_handler){s_owner.store(nullptr);return false;}return true;
}
LONG WINAPI NativeSceneFaultEvidence::observe(EXCEPTION_POINTERS* exception) noexcept {
    const SavedLastError last_error;
    auto* self=s_owner.load();if(!self || self->m_attempted.load() || self->m_busy.test_and_set())return EXCEPTION_CONTINUE_SEARCH;
    Evidence value{};if(collect(exception,self->m_module,self->m_rvas,self->m_stamp,value) && !self->m_attempted.exchange(true))write_new(self->m_path.c_str(),value);
    self->m_busy.clear();return EXCEPTION_CONTINUE_SEARCH;
}
