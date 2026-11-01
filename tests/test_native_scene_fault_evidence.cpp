#include "synthetic_code.h"
#include "NativeSceneFaultEvidence.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>void put(uintptr_t address,const T& value){std::memcpy(reinterpret_cast<void*>(address),&value,sizeof(T));}
// The 23 Oct crash (PLAYTEST_2026-10-23_1948): lock xadd at the helper with RCX = "mp_bunke"+0x70,
// [rsp] the holder's return, [rsp+0x28] the scene object, [rsp+0x30] the caller's return, RDI the table.
int main(){try{
    using Probe=NativeSceneFaultEvidence;const auto stamp=game_build::REFERENCE_EXE;
    const auto helper=SYNTH(Probe::HELPER_BYTES);const auto holder=SYNTH(Probe::HOLDER_BYTES);const auto caller=SYNTH(Probe::CALLER_BYTES);
    require(Probe::matches(stamp,helper,holder,caller),"exact reference helper, holder and caller accepted");
    for(size_t i=0;i<caller.size();++i){auto changed=caller;changed[i]^=1;require(!Probe::matches(stamp,helper,holder,changed),"any changed caller byte refuses");}
    for(size_t i=0;i<helper.size();++i){auto changed=helper;changed[i]^=1;require(!Probe::matches(stamp,changed,holder,caller),"any changed helper byte refuses");}
    require(!Probe::matches(0x12345678,helper,holder,caller),"unknown build refuses");

    const auto memory=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,0x4000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));require(memory!=0,"fixture memory");
    const uintptr_t module=0x140000000;const Probe::Rvas rvas{Probe::HELPER+Probe::FAULT_OFFSET,Probe::HOLDER_RETURN,Probe::CALLER_RETURN};
    const auto stack=memory+0x100,object=memory+0x1000,table=memory+0x2000,freed=memory+0x3000;constexpr int32_t index=5;
    const auto entry=table+(index+1)*Probe::ENTRY_STRIDE;
    put(stack,module+Probe::HOLDER_RETURN);put(stack+Probe::OBJECT_SLOT,object);put(stack+Probe::CALLER_SLOT,module+Probe::CALLER_RETURN);
    put(object+Probe::INDEX_OFFSET,index);put(entry,freed);std::memcpy(reinterpret_cast<void*>(freed),"mp_bunker",9);
    EXCEPTION_RECORD record{};CONTEXT context{};context.Rip=module+rvas.fault;context.Rsp=stack;context.Rdi=table;context.Rdx=0x656b6e75625f706d;context.Rcx=context.Rdx+0x70;context.Rax=1;
    record.ExceptionCode=EXCEPTION_ACCESS_VIOLATION;record.NumberParameters=2;record.ExceptionInformation[1]=UINTPTR_MAX;record.ExceptionAddress=reinterpret_cast<void*>(context.Rip);
    EXCEPTION_POINTERS exception{&record,&context};const auto saved_context=context;const auto saved_record=record;
    Probe::Evidence value;
    {Probe::Scope scene{0x1234,0x11,2,15608};require(Probe::collect(&exception,module,rvas,stamp,value),"the 23 Oct fault shape is captured");}
    require(!std::memcmp(&context,&saved_context,sizeof(context)) && !std::memcmp(&record,&saved_record,sizeof(record)),"exception context and record unchanged");
    require(value.size==sizeof(value) && value.timestamp==stamp && value.first_chance==1 && value.disposition==EXCEPTION_CONTINUE_SEARCH && value.parameters[0]==0 && value.parameters[1]==UINTPTR_MAX,"provenance and the general-protection parameters are recorded");
    require(value.chain==(Probe::holder_return|Probe::caller_return|Probe::index_read),"both return addresses and the index join the chain");
    require(value.object==object && value.table==table && value.index==index && value.entry==entry && value.entry_value==freed,"object, table, index and entry follow the caller's own arithmetic");
    require(value.registers[1]==context.Rcx && value.registers[2]==context.Rdx && value.registers[7]==table,"general registers in CONTEXT order");
    require(value.scene_renderer==0x1234 && value.scene_flags==0x11 && value.scene_slot==2 && value.scene_dispatch==15608 && value.scene_active==1,"the scene call on this thread is recorded");
    require(value.regions[0].address==stack && value.regions[0].size==512 && value.regions[1].address==object && value.regions[2].address==table && value.regions[3].address==entry-0x30 && value.regions[4].address==freed && !std::memcmp(value.regions[4].bytes.data(),"mp_bunker",9),"bounded copies of stack, object, table, entry and the entry's target");
    require(!value.regions[5].size,"an unmapped renderer is metadata only, never fabricated");
    Probe::Evidence outside;require(Probe::collect(&exception,module,rvas,stamp,outside) && !outside.scene_active && !outside.scene_renderer,"outside a scene the scene fields stay empty");

    // The helper is shared: a fault from another caller is still recorded, without the chain reads.
    put(stack+Probe::CALLER_SLOT,module+0x1000);Probe::Evidence other;require(Probe::collect(&exception,module,rvas,stamp,other),"another caller's fault is recorded");
    require(other.chain==Probe::holder_return && !other.object && !other.table && !other.entry && !other.regions[1].address,"without both return addresses nothing past the stack is followed");
    put(stack+Probe::CALLER_SLOT,module+Probe::CALLER_RETURN);
    for(unsigned bad=0;bad<4;++bad){auto r=record;auto c=context;EXCEPTION_POINTERS e{&r,&c};if(bad==0)r.ExceptionCode=EXCEPTION_INT_DIVIDE_BY_ZERO;if(bad==1)c.Rip++;if(bad==2)r.ExceptionAddress=nullptr;if(bad==3)e.ContextRecord=nullptr;
        Probe::Evidence untouched;untouched.magic=17;require(!Probe::collect(&e,module,rvas,stamp,untouched) && untouched.magic==17,"unrelated exceptions are not captured");}
    require(!Probe::collect(&exception,module,{},stamp,value) && !Probe::collect(nullptr,module,rvas,stamp,value),"unresolved build or no exception refuses");

    // arm() on a real image fixture: headers, then the three verified functions.
    const auto image=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,0x2800000,MEM_RESERVE,PAGE_NOACCESS));require(image!=0,"reserve image fixture");
    for(const auto rva:{uintptr_t(0),Probe::HELPER,Probe::HOLDER,Probe::CALLER})require(VirtualAlloc(reinterpret_cast<void*>(image+(rva&~uintptr_t(0xfff))),0x2000,MEM_COMMIT,PAGE_READWRITE),"commit fixture pages");
    IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x100;put(image,dos);IMAGE_NT_HEADERS64 nt{};nt.Signature=IMAGE_NT_SIGNATURE;nt.FileHeader.TimeDateStamp=stamp;nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt.OptionalHeader.SizeOfImage=0x2800000;put(image+0x100,nt);
    std::memcpy(reinterpret_cast<void*>(image+Probe::HELPER),helper.data(),helper.size());std::memcpy(reinterpret_cast<void*>(image+Probe::HOLDER),holder.data(),holder.size());std::memcpy(reinterpret_cast<void*>(image+Probe::CALLER),caller.data(),caller.size());
    const auto path=std::filesystem::current_path()/(L"scene_fault_fixture_"+std::to_wstring(GetCurrentProcessId())+L".bin");require(!std::filesystem::exists(path),"fresh evidence path");
    auto* byte=reinterpret_cast<uint8_t*>(image+Probe::HOLDER+3);*byte^=1;Probe refused;require(!refused.arm(reinterpret_cast<HMODULE>(image),path) && !refused.armed(),"a changed holder leaves the handler unregistered");*byte^=1;
    require(!refused.arm(reinterpret_cast<HMODULE>(image),path),"one arm attempt per instance");
    require(Probe::get().arm(reinterpret_cast<HMODULE>(image),path) && Probe::get().armed() && Probe::get().timestamp()==stamp,"exact image arms the handler");
    constexpr DWORD sentinel=0x2468ace0;
    auto unrelated=record;unrelated.ExceptionCode=EXCEPTION_BREAKPOINT;EXCEPTION_POINTERS skip{&unrelated,&context};
    SetLastError(sentinel);require(Probe::observe(&skip)==EXCEPTION_CONTINUE_SEARCH && GetLastError()==sentinel && !std::filesystem::exists(path),"unrelated exception writes nothing and keeps LastError");
    context.Rip=image+rvas.fault;record.ExceptionAddress=reinterpret_cast<void*>(context.Rip);put(stack,image+Probe::HOLDER_RETURN);put(stack+Probe::CALLER_SLOT,image+Probe::CALLER_RETURN);
    SetLastError(sentinel);require(Probe::observe(&exception)==EXCEPTION_CONTINUE_SEARCH && GetLastError()==sentinel,"the matching fault continues the search, LastError kept");
    require(std::filesystem::exists(path) && std::filesystem::file_size(path)==sizeof(Probe::Evidence),"one bounded POD written");
    std::ifstream input{path,std::ios::binary};Probe::Evidence written{};input.read(reinterpret_cast<char*>(&written),sizeof(written));input.close();
    require(written.magic==Probe::MAGIC && written.module==image && written.entry_value==freed && written.chain==(Probe::holder_return|Probe::caller_return|Probe::index_read),"written evidence is the captured chain");
    const auto time=std::filesystem::last_write_time(path);require(Probe::observe(&exception)==EXCEPTION_CONTINUE_SEARCH && std::filesystem::last_write_time(path)==time,"a repeated fault never rewrites the first evidence");
    require(DeleteFileW(path.c_str()),"remove fixture evidence");VirtualFree(reinterpret_cast<void*>(image),0,MEM_RELEASE);VirtualFree(reinterpret_cast<void*>(memory),0,MEM_RELEASE);
    std::cout<<"native scene fault evidence: verified functions, chain reads, scene call, once-only POD PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
