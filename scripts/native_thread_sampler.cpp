// Diagnostic only. Suspension perturbs execution: never use these samples as an FPS
// benchmark. No injection, writes, or synchronization bypasses.
//
// Default (snapshot) mode keeps the target suspended only while it copies the thread's
// registers and the top of its stack (up to 64 KiB), then resumes it and unwinds the copy.
// Code and unwind tables are read from the live process, which does not change them. The
// older --live-unwind mode walks the live stack while suspended (about 2 ms per sample, up to
// 130 ms on 19 Oct). suspended_ms in the output is the target's actual suspension.
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct Handle {
    HANDLE value{};
    ~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
struct Suspension {
    HANDLE thread;bool held{};
    explicit Suspension(HANDLE t):thread(t){held=SuspendThread(t)!=DWORD(-1);}
    ~Suspension(){if(held)ResumeThread(thread);}
};
struct Sample {double qpc{},suspended_ms{};DWORD error{};std::vector<DWORD64> pcs;};
double qpc_ms(){LARGE_INTEGER n{},f{};QueryPerformanceCounter(&n);QueryPerformanceFrequency(&f);return double(n.QuadPart)*1000./double(f.QuadPart);}

constexpr size_t STACK_COPY=64*1024;
// The stack copy StackWalk64 reads instead of the live (resumed, changing) stack.
struct StackCopy {DWORD64 base{};std::vector<uint8_t> bytes;};
thread_local const StackCopy* t_copy{};
BOOL CALLBACK read_memory(HANDLE process,DWORD64 address,PVOID buffer,DWORD size,LPDWORD read){
    if(t_copy && address>=t_copy->base && address+size<=t_copy->base+t_copy->bytes.size()){
        std::memcpy(buffer,t_copy->bytes.data()+(address-t_copy->base),size);if(read)*read=size;return TRUE;}
    SIZE_T n{};const auto ok=ReadProcessMemory(process,reinterpret_cast<LPCVOID>(address),buffer,size,&n);
    if(read)*read=DWORD(n);return ok && n==size;
}
// Page by page from rsp until the stack's end (an unreadable page) or STACK_COPY bytes.
void copy_stack(HANDLE process,DWORD64 rsp,StackCopy& copy){
    copy.base=rsp;copy.bytes.resize(STACK_COPY);size_t have=0;
    while(have<STACK_COPY){
        const auto address=rsp+have;const auto page_end=(address|0xfff)+1;
        const auto chunk=size_t(std::min<DWORD64>(page_end-address,STACK_COPY-have));
        SIZE_T n{};if(!ReadProcessMemory(process,reinterpret_cast<LPCVOID>(address),copy.bytes.data()+have,chunk,&n) || n!=chunk)break;
        have+=chunk;
    }
    copy.bytes.resize(have);
}
void unwind(HANDLE process,HANDLE thread,CONTEXT context,Sample& out){
    STACKFRAME64 frame{};
    frame.AddrPC={context.Rip,0,AddrModeFlat};
    frame.AddrStack={context.Rsp,0,AddrModeFlat};
    frame.AddrFrame={context.Rbp,0,AddrModeFlat};
    out.pcs.push_back(context.Rip);
    for(unsigned i=1;i<48;++i){
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,thread,&frame,&context,t_copy?read_memory:nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
        if(!frame.AddrPC.Offset || (frame.AddrPC.Offset==out.pcs.back() && i>1))break;
        if(frame.AddrPC.Offset!=out.pcs.back())out.pcs.push_back(frame.AddrPC.Offset);
    }
}
Sample sample(HANDLE process,HANDLE thread,bool live=false){
    Sample out;out.pcs.reserve(48); // Allocate before suspending the target.
    StackCopy copy;copy.bytes.reserve(STACK_COPY);
    CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
    out.qpc=qpc_ms();
    {
        Suspension suspended{thread};
        if(!suspended.held){out.error=GetLastError();return out;}
        if(!GetThreadContext(thread,&context)){out.error=GetLastError();return out;}
        if(live){unwind(process,thread,context,out);out.suspended_ms=qpc_ms()-out.qpc;return out;}
        copy_stack(process,context.Rsp,copy);
    }
    out.suspended_ms=qpc_ms()-out.qpc;
    t_copy=&copy;unwind(process,thread,context,out);t_copy=nullptr;
    return out;
}
__declspec(noinline) DWORD WINAPI wait_fixture(void* event){
    const auto result=WaitForSingleObject(static_cast<HANDLE>(event),INFINITE);
    volatile auto preserve=result;return preserve;
}
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
std::string csv_text(const std::string& value){std::string out="\"";for(char c:value){out+=c;if(c=='\"')out+='\"';}return out+'\"';}
bool finds_fixture(const Sample& s){
    for(auto pc:s.pcs)if(pc>=reinterpret_cast<DWORD64>(&wait_fixture) && pc<reinterpret_cast<DWORD64>(&wait_fixture)+128)return true;
    return false;
}
int main(int argc,char** argv){try{
    SymSetOptions(SYMOPT_DEFERRED_LOADS|SYMOPT_UNDNAME|SYMOPT_NO_PROMPTS|SYMOPT_FAIL_CRITICAL_ERRORS);
    if(argc==2 && std::string(argv[1])=="--self-test"){
        Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
        DWORD tid{};Handle worker{CreateThread(nullptr,0,wait_fixture,event.value,0,&tid)};
        require(event.value && worker.value,"fixture handles");
        require(SymInitialize(GetCurrentProcess(),"",TRUE)!=FALSE,"symbol engine");
        bool found{};Sample captured;
        for(unsigned i=0;i<100 && !found;++i){Sleep(1);captured=sample(GetCurrentProcess(),worker.value,true);found=finds_fixture(captured);}
        if(!found){std::cerr<<"fixture=0x"<<std::hex<<reinterpret_cast<DWORD64>(&wait_fixture)<<" error="<<captured.error<<" stack:";for(auto pc:captured.pcs)std::cerr<<" 0x"<<pc;std::cerr<<std::dec<<'\n';}
        require(found && !captured.error && captured.pcs.size()>2,"live unwind reaches the real blocking fixture");
        // Snapshot mode: the same stack, unwound from the copy after the thread resumed.
        std::vector<double> suspended;
        for(unsigned i=0;i<200 && suspended.size()<20;++i){
            Sleep(1);captured=sample(GetCurrentProcess(),worker.value);
            if(finds_fixture(captured) && !captured.error && captured.pcs.size()>2)suspended.push_back(captured.suspended_ms);
        }
        require(suspended.size()==20,"snapshot unwind reaches the real blocking fixture");
        std::sort(suspended.begin(),suspended.end());
        std::cout<<"snapshot suspension median "<<suspended[10]<<" ms, max "<<suspended.back()<<" ms\n";
        require(suspended[10]<1.0,"snapshot mode suspends the target for well under a millisecond");
        SetEvent(event.value);
        require(WaitForSingleObject(worker.value,2000)==WAIT_OBJECT_0,"worker resumes and completes after sampling");
        SymCleanup(GetCurrentProcess());
        Handle invalid{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
        require(sample(GetCurrentProcess(),invalid.value).error!=0,"invalid thread fails without claiming samples");
        std::cout<<"native thread sampler: blocked stack (live and snapshot), resume and invalid handle pass\n";return 0;
    }
    require(argc==6 || (argc==7 && std::string(argv[6])=="--live-unwind"),"usage: native_thread_sampler PID TID COUNT INTERVAL_MS ABSOLUTE_OUTPUT.csv [--live-unwind]");
    const bool live=argc==7;
    const auto pid=std::stoul(argv[1]),tid=std::stoul(argv[2]),count=std::stoul(argv[3]),interval=std::stoul(argv[4]);
    require(pid && tid && count>0 && count<=20000 && interval>=1 && interval<=1000,"bounded sampling arguments");
    const std::filesystem::path output{argv[5]};require(output.is_absolute() && !std::filesystem::exists(output),"fresh absolute output required");
    Handle process{OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid)};
    Handle thread{OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION|SYNCHRONIZE,FALSE,tid)};
    require(process.value && thread.value,"process/thread access");
    require(GetProcessIdOfThread(thread.value)==pid,"thread must belong to target PID");
    require(SymInitialize(process.value,"",TRUE)!=FALSE,"symbol engine");
    std::vector<Sample> samples;samples.reserve(count);
    // Stop early (and still write what was sampled) once the thread or the game has exited.
    for(unsigned i=0;i<count;++i){
        if(WaitForSingleObject(thread.value,0)==WAIT_OBJECT_0 || WaitForSingleObject(process.value,0)==WAIT_OBJECT_0)break;
        samples.push_back(sample(process.value,thread.value,live));Sleep(interval);
    }
    std::ofstream stream{output};require(stream.good(),"open output");
    // path: the module's full path; the mod's proxy and the system DXGI are both named dxgi.dll.
    stream<<"sample,qpc_ms,suspended_ms,error,frame,pc,module,rva,symbol,path\n";stream.precision(14);
    for(size_t i=0;i<samples.size();++i){const auto& s=samples[i];
        if(s.pcs.empty())stream<<i<<','<<s.qpc<<','<<s.suspended_ms<<','<<s.error<<",,,,,,\n";
        for(size_t j=0;j<s.pcs.size();++j){const auto pc=s.pcs[j],base=SymGetModuleBase64(process.value,pc);
            // dbghelp's own module list survives the target's exit (an early stop); the live query does not.
            std::array<wchar_t,32768> name{};IMAGEHLP_MODULEW64 module{};module.SizeOfStruct=sizeof(module);
            if(base && SymGetModuleInfoW64(process.value,pc,&module))wcsncpy_s(name.data(),name.size(),module.ImageName,_TRUNCATE);
            else GetModuleFileNameExW(process.value,reinterpret_cast<HMODULE>(base),name.data(),DWORD(name.size()));
            alignas(SYMBOL_INFO) std::array<char,sizeof(SYMBOL_INFO)+1024> storage{};
            auto* info=reinterpret_cast<SYMBOL_INFO*>(storage.data());info->SizeOfStruct=sizeof(SYMBOL_INFO);info->MaxNameLen=1024;DWORD64 displacement{};
            const auto symbol=SymFromAddr(process.value,pc,&displacement,info)?std::string{info->Name}:std::string{};
            stream<<i<<','<<s.qpc<<','<<s.suspended_ms<<','<<s.error<<','<<j<<",0x"<<std::hex<<pc<<','<<csv_text(std::filesystem::path{name.data()}.filename().string())<<",0x"<<(base?pc-base:0)<<','<<csv_text(symbol)<<','<<csv_text(std::filesystem::path{name.data()}.string())<<std::dec<<'\n';
        }
    }
    SymCleanup(process.value);require(stream.good(),"write output");return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
