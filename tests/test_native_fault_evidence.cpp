#include "synthetic_code.h"
#include "NativeFaultEvidence.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    using Probe=NativeFaultEvidence;auto function=SYNTH(Probe::FUNCTION_BYTES);auto fault=SYNTH(Probe::FAULT_BYTES);auto exe_fault=SYNTH(Probe::EXE_FAULT_BYTES);
    require(Probe::matches(Probe::TIMESTAMP,function,fault),"exact shipping game-DLL fault instructions accepted");
    for(size_t i=0;i<function.size();++i){auto changed=function;changed[i]^=1;require(!Probe::matches(Probe::TIMESTAMP,changed,fault),"changed native function refuses");}
    for(size_t i=0;i<fault.size();++i){auto changed=fault;changed[i]^=1;require(!Probe::matches(Probe::TIMESTAMP,function,changed),"changed fault instruction refuses");}
    require(!Probe::matches(0,function,fault),"wrong game-DLL build refuses");
    require(Probe::matches(Probe::EXE_TIMESTAMP,SYNTH(Probe::EXE_FUNCTION_BYTES),exe_fault,Probe::Image::executable),"exact installed executable duplicate getter accepted");
    require(!Probe::matches(Probe::TIMESTAMP,function,exe_fault,Probe::Image::executable) && !Probe::matches(Probe::EXE_TIMESTAMP,SYNTH(Probe::EXE_FUNCTION_BYTES),fault),"DLL and executable signatures cannot cross-match");
    for(size_t i=0;i<function.size();++i){auto changed=SYNTH(Probe::EXE_FUNCTION_BYTES);changed[i]^=1;require(!Probe::matches(Probe::EXE_TIMESTAMP,changed,exe_fault,Probe::Image::executable),"every changed executable byte refuses");}

    const auto memory=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,0x3000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));require(memory!=0,"allocate owned memory fixture");
    DWORD old{};require(VirtualProtect(reinterpret_cast<void*>(memory+0x1000),0x1000,PAGE_NOACCESS,&old),"protect middle fixture page");
    std::memset(reinterpret_cast<void*>(memory+0x2000),0x6a,0x1000);const auto object=memory+0x100,vtable=memory+0x2400,table=memory+0x2800;const uint64_t packed=table|(uint64_t(3)<<48);
    std::memcpy(reinterpret_cast<void*>(object),&vtable,8);std::memcpy(reinterpret_cast<void*>(object+0xd8),&packed,8);
    EXCEPTION_RECORD record{};CONTEXT context{};const uintptr_t module=0x180000000;context.Rip=module+Probe::RVA;context.Rbx=object;context.Rsp=memory+0x800;context.R8=0x4d00000000;context.Rcx=0;
    record.ExceptionCode=EXCEPTION_ACCESS_VIOLATION;record.NumberParameters=2;record.ExceptionAddress=reinterpret_cast<void*>(context.Rip);record.ExceptionInformation[1]=context.R8+0xc0;EXCEPTION_POINTERS exception{&record,&context};const auto saved_context=context;const auto saved_record=record;
    Probe::Evidence value;require(Probe::collect(&exception,module,value),"exact read fault produces bounded POD evidence");
    require(value.size==2908 && value.pid==GetCurrentProcessId() && value.thread==GetCurrentThreadId() && value.first_chance==1 && value.disposition==EXCEPTION_CONTINUE_SEARCH && value.rbx==object && value.fault_address==0x4d000000c0,"explicit first-chance provenance never suppresses an exception");
    require(!std::memcmp(&context,&saved_context,sizeof(context)) && !std::memcmp(&record,&saved_record,sizeof(record)),"exception context/record remain byte-for-byte unchanged");
    require(value.regions[0].size==512 && value.regions[3].address==vtable && value.regions[3].size==512 && value.regions[4].address==table && value.regions[4].bytes[0]==0x6a,"copy owned object/vtable and mask actual packed transform table without invoking it");
    require(!value.regions[2].size,"unmapped fault table remains unreadable evidence, never fabricated memory");
    auto exe_context=context;auto exe_record=record;exe_context.Rip=module+Probe::EXE_RVA;exe_record.ExceptionAddress=reinterpret_cast<void*>(exe_context.Rip);EXCEPTION_POINTERS exe_exception{&exe_record,&exe_context};Probe::Evidence exe_value;
    require(Probe::collect(&exe_exception,module,exe_value,Probe::Image::executable) && exe_value.timestamp==Probe::EXE_TIMESTAMP && exe_value.rip==module+Probe::EXE_RVA && exe_value.rbx==object && exe_value.regions[4].address==table,"actual executable getter collects the same bounded object and packed field");
    require(!Probe::collect(&exe_exception,module,exe_value) && !Probe::collect(&exception,module,exe_value,Probe::Image::executable),"fault RVAs remain image-specific");
    require(!std::memcmp(&exe_context,&context,offsetof(CONTEXT,Rip)) && exe_record.ExceptionCode==record.ExceptionCode,"executable collection leaves source registers unchanged");

    auto boundary=context;boundary.Rbx=memory+0xff0;EXCEPTION_POINTERS clipped{&record,&boundary};Probe::Evidence small;require(Probe::collect(&clipped,module,small) && small.regions[0].size==16 && !small.regions[3].size && !small.regions[4].size,"bounded read stops at protected page and cannot infer missing object fields");
    boundary.Rbx=memory+0x1000;require(Probe::collect(&clipped,module,small) && !small.regions[0].size && small.regions[0].protect==PAGE_NOACCESS,"no-access object records metadata without a direct dereference");
    for(unsigned bad=0;bad<8;++bad){auto r=record;auto c=context;EXCEPTION_POINTERS e{&r,&c};if(bad==0)r.ExceptionCode=0;if(bad==1)r.NumberParameters=1;if(bad==2)r.ExceptionInformation[0]=1;if(bad==3)c.Rip++;if(bad==4)r.ExceptionAddress=nullptr;if(bad==5)r.ExceptionInformation[1]++;if(bad==6)c.R8=UINTPTR_MAX;if(bad==7)e.ContextRecord=nullptr;
        Probe::Evidence untouched;untouched.magic=17;require(!Probe::collect(&e,module,untouched) && untouched.magic==17,"unrelated, malformed or write exceptions cannot trigger evidence capture");}
    require(!Probe::collect(nullptr,module,small) && !Probe::collect(&exception,UINTPTR_MAX,small),"null and overflow fault origins refuse");
    const auto path=std::filesystem::current_path()/(L"fault_evidence_fixture_"+std::to_wstring(GetCurrentProcessId())+L".bin");require(!std::filesystem::exists(path),"fresh evidence fixture path");
    require(Probe::write_new(path.c_str(),value),"exact POD evidence writes through real CREATE_NEW path");auto changed=value;changed.thread++;require(!Probe::write_new(path.c_str(),changed),"existing evidence is never overwritten");
    std::ifstream input{path,std::ios::binary};std::vector<char> bytes{std::istreambuf_iterator<char>{input},{}};input.close();require(bytes.size()==sizeof(value) && !std::memcmp(bytes.data(),&value,sizeof(value)),"written native layout is exact and previous evidence bytes survive");
    require(!Probe::get().arm(nullptr,path.parent_path()/L"never_fault.bin"),"missing native module cannot install first-chance observation");require(DeleteFileW(path.c_str()),"remove owned fixture file");
    const auto image=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,0xe00000,MEM_RESERVE,PAGE_NOACCESS));require(image!=0,"reserve guarded image fixture");
    require(VirtualAlloc(reinterpret_cast<void*>(image),0x1000,MEM_COMMIT,PAGE_READWRITE) && VirtualAlloc(reinterpret_cast<void*>(image+(Probe::RVA&~uintptr_t(0xfff))),0x1000,MEM_COMMIT,PAGE_READWRITE),"commit only exact guarded fixture pages");
    IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x100;std::memcpy(reinterpret_cast<void*>(image),&dos,sizeof(dos));IMAGE_NT_HEADERS64 nt{};nt.Signature=IMAGE_NT_SIGNATURE;nt.FileHeader.TimeDateStamp=Probe::TIMESTAMP;nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt.OptionalHeader.SizeOfImage=0xe00000;std::memcpy(reinterpret_cast<void*>(image+0x100),&nt,sizeof(nt));std::memcpy(reinterpret_cast<void*>(image+Probe::FUNCTION_RVA),function.data(),function.size());std::memcpy(reinterpret_cast<void*>(image+Probe::RVA),fault.data(),fault.size());
    const auto exe_image=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,0x1400000,MEM_RESERVE,PAGE_NOACCESS));require(exe_image!=0,"reserve executable image fixture");
    require(VirtualAlloc(reinterpret_cast<void*>(exe_image),0x1000,MEM_COMMIT,PAGE_READWRITE) && VirtualAlloc(reinterpret_cast<void*>(exe_image+(Probe::EXE_RVA&~uintptr_t(0xfff))),0x1000,MEM_COMMIT,PAGE_READWRITE),"commit exact executable fixture pages");
    nt.FileHeader.TimeDateStamp=Probe::EXE_TIMESTAMP;nt.OptionalHeader.SizeOfImage=0x1400000;std::memcpy(reinterpret_cast<void*>(exe_image),&dos,sizeof(dos));std::memcpy(reinterpret_cast<void*>(exe_image+0x100),&nt,sizeof(nt));std::memcpy(reinterpret_cast<void*>(exe_image+Probe::EXE_FUNCTION_RVA),SYNTH(Probe::EXE_FUNCTION_BYTES).data(),function.size());std::memcpy(reinterpret_cast<void*>(exe_image+Probe::EXE_RVA),exe_fault.data(),exe_fault.size());
    const auto live_path=path.parent_path()/(L"fault_callback_fixture_"+std::to_wstring(GetCurrentProcessId())+L".bin");
    require(Probe::get().poll(nullptr,live_path)==Probe::ArmResult::waiting && Probe::get().poll(nullptr,live_path)==Probe::ArmResult::waiting && !std::filesystem::exists(live_path),"late-loaded game module remains waiting without consuming a guard attempt");
    Probe failed;require(failed.poll(reinterpret_cast<HMODULE>(1),live_path)==Probe::ArmResult::refused && failed.poll(reinterpret_cast<HMODULE>(image),live_path)==Probe::ArmResult::refused,"real module guard refusal poisons the bounded arm attempt without retry");
    auto* exe_entry=reinterpret_cast<uint8_t*>(exe_image+Probe::EXE_FUNCTION_RVA);*exe_entry^=1;Probe bad_exe;
    require(!bad_exe.arm(reinterpret_cast<HMODULE>(image),live_path,reinterpret_cast<HMODULE>(exe_image)) && !std::filesystem::exists(live_path),"real executable image signature refusal leaves callback unarmed and no output");*exe_entry^=1;
    require(!std::filesystem::exists(live_path) && Probe::get().poll(reinterpret_cast<HMODULE>(image),live_path,reinterpret_cast<HMODULE>(exe_image))==Probe::ArmResult::armed && Probe::get().poll(reinterpret_cast<HMODULE>(image),live_path,reinterpret_cast<HMODULE>(exe_image))==Probe::ArmResult::armed,"late module arrival validates exact guard and installs callback once");
    constexpr DWORD error_sentinel=0x13579bdf;
    SetLastError(error_sentinel);const auto unrelated_result=Probe::observe(&exception);const auto unrelated_error=GetLastError();
    require(unrelated_result==EXCEPTION_CONTINUE_SEARCH && unrelated_error==error_sentinel && !std::filesystem::exists(live_path),"unrelated module exception preserves LastError and does not consume bounded capture");
    context.Rip=exe_image+Probe::EXE_RVA;record.ExceptionAddress=reinterpret_cast<void*>(context.Rip);
    // VirtualQuery must refuse this unreadable top-of-address-space region. The
    // record still describes the exact guarded instruction's R8+c0 read.
    context.R8=UINTPTR_MAX-0xc0;record.ExceptionInformation[1]=UINTPTR_MAX;
    const auto before=context;const auto record_before=record;
    SetLastError(error_sentinel);const auto captured_result=Probe::observe(&exception);const auto captured_error=GetLastError();
    require(captured_result==EXCEPTION_CONTINUE_SEARCH && captured_error==error_sentinel && !std::memcmp(&before,&context,sizeof(context)) && !std::memcmp(&record_before,&record,sizeof(record)),"actual matching callback preserves LastError, exception record and context with unreadable fault region");
    require(std::filesystem::exists(live_path) && std::filesystem::file_size(live_path)==sizeof(value),"actual executable callback writes its one bounded POD");
    std::ifstream captured{live_path,std::ios::binary};Probe::Evidence actual{};captured.read(reinterpret_cast<char*>(&actual),sizeof(actual));captured.close();require(actual.timestamp==Probe::EXE_TIMESTAMP && actual.module==exe_image && actual.rip==exe_image+Probe::EXE_RVA && actual.regions[2].address==context.R8 && actual.regions[2].size==0,"registered callback selects verified executable provenance and retains unreadable region without fabrication");
    const auto timestamp=std::filesystem::last_write_time(live_path);context.Rbx=1;SetLastError(error_sentinel);const auto repeated_result=Probe::observe(&exception);const auto repeated_error=GetLastError();require(repeated_result==EXCEPTION_CONTINUE_SEARCH && repeated_error==error_sentinel && std::filesystem::last_write_time(live_path)==timestamp,"matching repeated fault cannot rewrite first evidence or resume execution");require(DeleteFileW(live_path.c_str()),"remove owned callback evidence fixture");VirtualFree(reinterpret_cast<void*>(exe_image),0,MEM_RELEASE);VirtualFree(reinterpret_cast<void*>(image),0,MEM_RELEASE);VirtualFree(reinterpret_cast<void*>(memory),0,MEM_RELEASE);
    std::cout<<"native fault evidence: exact native guard, bounded actual POD, protected pages, unchanged exception and no overwrite PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
