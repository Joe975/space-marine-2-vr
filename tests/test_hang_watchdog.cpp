#include "HangWatchdog.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using namespace std::chrono_literals;
int main(){try{
    using W=HangWatchdog;using C=W::Clock;const auto t0=C::time_point{}+100s;const auto ticks=t0.time_since_epoch().count();
    require(!W::due(0,t0+1h,10s,false),"no frame seen yet (loading before the first present): never due");
    require(!W::due(ticks,t0+9999ms,10s,false) && W::due(ticks,t0+10s,10s,false),"due once no frame has come for 10 s");
    require(!W::due(ticks,t0+1h,10s,true),"once dumped, never again");
    require(W::THRESHOLD==10s,"10 s by default");

    const auto dir=std::filesystem::temp_directory_path()/("hang_watchdog_"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const auto direct=dir/"direct.dmp";
    require(W::write_dump(direct) && std::filesystem::file_size(direct)>0,"a real minidump of this process is written");
    {std::ifstream in{direct,std::ios::binary};char sig[4]{};in.read(sig,4);require(std::string(sig,4)=="MDMP","minidump signature");}
    const auto size=std::filesystem::file_size(direct);
    require(!W::write_dump(direct) && std::filesystem::file_size(direct)==size,"an existing dump is never overwritten");
    require(!W::write_dump("relative.dmp"),"relative paths are refused");

    // The thread: frames keep coming, no dump; frames stop, one dump; never a second.
    auto& watchdog=*new W; // detached thread: never destroyed
    const auto path=dir/"hang.dmp";std::atomic<int> calls{};std::atomic<bool> result{};std::atomic<long long> stalled_ms{};
    require(watchdog.start(path,300ms,20ms,[&](const auto& p,bool ok,auto stalled){++calls;result=ok && p==path;stalled_ms=stalled.count();}),"watchdog started");
    require(!watchdog.start(path,300ms,20ms),"started once");
    const auto until=C::now()+800ms;while(C::now()<until){watchdog.present();std::this_thread::sleep_for(10ms);}
    require(!calls && !std::filesystem::exists(path),"frames presenting: no dump");
    const auto stop=C::now()+2s;while(!calls && C::now()<stop)std::this_thread::sleep_for(20ms);
    require(calls==1 && result && watchdog.dumped() && std::filesystem::exists(path) && stalled_ms>=300,"frames stopped: one dump, with the stall time");
    watchdog.present();std::this_thread::sleep_for(500ms);require(calls==1,"no second dump after frames resume and stop again");
    auto& never=*new W;require(never.start(dir/"never.dmp",100ms,20ms),"second watchdog started");std::this_thread::sleep_for(400ms);require(!std::filesystem::exists(dir/"never.dmp"),"no frame ever presented: no dump");
    std::error_code ignored;std::filesystem::remove_all(dir,ignored);
    std::cout<<"hang watchdog: due rule, real minidump, once only, no dump while presenting PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
