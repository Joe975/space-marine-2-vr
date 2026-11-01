#include "HangWatchdog.h"
#include <DbgHelp.h>
#include <thread>

HangWatchdog& HangWatchdog::get(){static auto* value=new HangWatchdog;return *value;}
bool HangWatchdog::write_dump(const std::filesystem::path& path) noexcept {
    if(!path.is_absolute())return false;
    const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    const auto type=MINIDUMP_TYPE(MiniDumpNormal|MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules);
    const bool ok=MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),file,type,nullptr,nullptr,nullptr)!=FALSE;
    FlushFileBuffers(file);CloseHandle(file);
    if(!ok)DeleteFileW(path.c_str());
    return ok;
}
bool HangWatchdog::start(std::filesystem::path path,std::chrono::milliseconds threshold,std::chrono::milliseconds period,
    std::function<void(const std::filesystem::path&,bool,std::chrono::milliseconds)> on_dump){
    if(!path.is_absolute() || threshold.count()<=0 || period.count()<=0 || m_started.exchange(true))return false;
    std::thread([this,path=std::move(path),threshold,period,on_dump=std::move(on_dump)]{
        while(!m_dumped.load()){
            std::this_thread::sleep_for(period);
            const auto last=m_last.load(std::memory_order_relaxed);const auto now=Clock::now();
            if(!due(last,now,threshold,m_dumped.load()))continue;
            m_dumped.store(true);
            const auto stalled=std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()-Clock::duration{last});
            const bool ok=write_dump(path);
            if(on_dump)try{on_dump(path,ok,stalled);}catch(...){}
        }
    }).detach();
    return true;
}
