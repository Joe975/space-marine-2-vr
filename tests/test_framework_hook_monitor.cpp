// The framework hook monitor's stall branch, compiled from the generated Framework.cpp
// (cmake/FrameworkHookMonitor.cmake extracts it to framework_monitor_stall.inc): presents stop, the
// monitor ticks every 500 ms as in Framework's monitor thread, and the test counts rehooks.
// 27 Oct 09:46 (docs/FREEZE_REHOOK_2026-10-27.md): a 22 s game stall with the present detour intact
// got two rehooks, and the null swap chain the second one left stopped owned stereo.
#include "FrameworkHookMonitor.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static std::vector<std::string> logged;
namespace spdlog {template<class... A>void info(const char* text,A&&...){logged.emplace_back(text);}}

namespace fixture {
// The stall branch reads std::chrono::steady_clock::now(); inside this namespace that name is the
// fixture clock, so the test steps time instead of sleeping.
namespace std::chrono {
using namespace ::std::chrono;
struct steady_clock {
    using time_point=::std::chrono::steady_clock::time_point;
    static inline time_point current{};
    static time_point now(){return current;}
};
}
struct Hook {bool hooked{true};bool is_hooked()const{return hooked;}};
struct Monitor {
    std::chrono::steady_clock::time_point m_last_present_time{},m_last_chance_time{},m_last_message_time{};
    bool m_has_last_chance{true},m_is_d3d11{};
    unsigned d3d12_rehooks{},d3d11_rehooks{};
    void hook_d3d12(){++d3d12_rehooks;}
    void hook_d3d11(){++d3d11_rehooks;}
    void tick(Hook* d3d12){
        const auto now=std::chrono::steady_clock::now();
#include "framework_monitor_stall.inc"
    }
};
// Presents stop at t=0; the monitor runs every 500 ms for the given time.
Monitor stall(Hook* hook,std::chrono::milliseconds length,bool d3d11=false){
    Monitor monitor;monitor.m_is_d3d11=d3d11;std::chrono::steady_clock::current={};
    monitor.m_last_present_time=monitor.m_last_message_time=std::chrono::steady_clock::now();
    for(auto t=std::chrono::milliseconds{0};t<=length;t+=std::chrono::milliseconds{500}){
        std::chrono::steady_clock::current=std::chrono::steady_clock::time_point{}+t;monitor.tick(hook);
    }
    return monitor;
}
}

static unsigned count(const char* text){unsigned n{};for(const auto& line:logged)if(line==text)++n;return n;}

int main(){try{
    using namespace std::chrono_literals;
    void* vtable[16]{};void* detour=reinterpret_cast<void*>(0x1234);vtable[8]=detour;
    fixture::Hook hooked;
    check(!sm2_framework_monitor::present_hook_intact(true),"nothing recorded: not intact");

    // The 09:46 case: D3D12 hooked, the present slot still holds the detour, a 22 s stall.
    sm2_framework_monitor::remember(&vtable[8],detour);
    check(sm2_framework_monitor::present_hook_intact(true) && !sm2_framework_monitor::present_hook_intact(false),"intact only while hooked and the slot holds the detour");
    logged.clear();auto monitor=fixture::stall(&hooked,22s);
    check(monitor.d3d12_rehooks==0 && monitor.d3d11_rehooks==0,"a stall with the present detour intact never rehooks (09:46: two rehooks)");
    check(count("D3D12 present hook is still intact, ignoring...")>=3 && count("Sending rehook request for D3D")==0,"the monitor says why it skipped each rehook");

    // Something rewrote the slot (an overlay, or the original present put back): rehook as before.
    vtable[8]=reinterpret_cast<void*>(0x5678);
    logged.clear();monitor=fixture::stall(&hooked,22s);
    check(monitor.d3d12_rehooks>=1 && count("Sending rehook request for D3D")==monitor.d3d12_rehooks,"a lost present detour still gets the framework's rehook");
    const auto lost=monitor.d3d12_rehooks;
    monitor=fixture::stall(&hooked,6s);check(monitor.d3d12_rehooks==0,"the first rehook still waits for the last chance (5 s, then 1 s)");
    monitor=fixture::stall(&hooked,7s);check(monitor.d3d12_rehooks==1,"and then comes at the same time as before");
    vtable[8]=detour;

    // No hook object yet (startup), a hook that unhooked, or D3D11: the framework's own behaviour.
    monitor=fixture::stall(nullptr,22s);check(monitor.d3d12_rehooks==lost,"no D3D12 hook object: rehooks as before");
    fixture::Hook unhooked{false};monitor=fixture::stall(&unhooked,22s);check(monitor.d3d12_rehooks==lost,"an unhooked D3D12 hook: rehooks as before");
    monitor=fixture::stall(&hooked,22s,true);check(monitor.d3d11_rehooks==lost && monitor.d3d12_rehooks==0,"D3D11: rehooks as before");

    std::cout<<"Framework hook monitor CPU fixture PASS (rehooks in a 22 s stall: intact 0, lost "<<lost<<")\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
