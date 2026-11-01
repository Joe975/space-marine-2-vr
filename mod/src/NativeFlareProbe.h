#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

// Read-only diagnostic for the hangar right-eye glow (docs/PLAYTEST_2026-10-23_0836.md). Lamp flares
// (GsFlareMng) keep one 0x60-byte state block per camera on each flare: +0 flags (4 = query wanted,
// 8 = query pending), +8 current visibility, +0xc target from the occlusion query, +0x28 query
// pixel total. The builder fades current toward target once a frame for every camera; the
// renderer reads query results and draws the camera's list by the view's index byte (view+0x33),
// and the flare z-query pass issues queries by the same index. Counts which index each eye's
// slot uses and what the two blocks hold, per log window. Never changes an argument.
//
// Benchmark switch (hide): after each builder call, both camera blocks' current and target
// visibility are set to 0, so the frame's flare list draws at zero visibility (effects benchmark,
// scripts/bench_effects.py). Queries still run. A control file (SM2VR_DIAG_FLARE_HIDE_CONTROL,
// polled once a second: "hide" or "native") switches it live; the probe installs with it.
class NativeFlareProbe {
public:
    using Build=void(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t,uint8_t);
    using Read=void(*)(uintptr_t,uint8_t,uintptr_t);
    using Issue=void(*)(uintptr_t,uint8_t,uintptr_t,uintptr_t,uintptr_t);
    static constexpr uintptr_t FLARE_BUILD=0x189da30,FLARE_READ=0x18a20f0,FLARE_ISSUE=0x18a1dc0;
    inline static constexpr game_build::Code FLARE_BUILD_BYTES=game_build::hashed(0x189da30,16,"98c9d3e1850edcd3ad9218832f2d40e5515506da73c6c817030be2ed11db312e");
    inline static constexpr game_build::Code FLARE_READ_BYTES=game_build::hashed(0x18a20f0,16,"136421c7ffdb9e36ff6840e9291434ad720a3235deaca73043fd9f1ff5f20c6f");
    inline static constexpr game_build::Code FLARE_ISSUE_BYTES=game_build::hashed(0x18a1dc0,16,"ae6e4b9fb2edd70220fb6ccb8e9f65de80dab43678be0d2b25d0634dcb7112e9");
    struct Block {uint64_t flares{},queried{},wanted{},pending{};double current{},target{};};
    struct Window {
        uint64_t builds{};int32_t cameras{};std::array<Block,2> blocks{};
        // [slot][index] calls; index 2 = any other value.
        std::array<std::array<uint64_t,3>,3> reads{},issues{};
    };
    static NativeFlareProbe& get(){static auto* value=new NativeFlareProbe;return *value;}
    bool installed()const{return m_build!=nullptr;}
    void configure_hide(bool hidden,std::filesystem::path control={}){
        std::lock_guard lock{m_control_mutex};m_hide.store(hidden);m_control=std::move(control);m_next=std::chrono::steady_clock::time_point{};
    }
    bool hidden()const{return m_hide.load();}
    // Zero both camera blocks' current and target visibility of one flare (exposed for tests).
    bool hide(uintptr_t flare){
        poll();if(!m_hide.load())return false;bool wrote=false;
        for(size_t i=0;i<2;++i){const auto block=flare+i*0x60;float current{},target{};
            if(!ordinary_scene_scope::read(block+8,current) || !ordinary_scene_scope::read(block+0xc,target))continue;
            if(current!=0.f || target!=0.f){const float zero[2]{};std::memcpy(reinterpret_cast<void*>(block+8),zero,sizeof zero);wrote=true;}}
        if(wrote)m_hidden.fetch_add(1,std::memory_order_relaxed);return wrote;
    }
    bool install(void* build,void* read,void* issue){
        if(m_build || !build || !read || !issue)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        std::array<void*,3> targets{build,read,issue},replacements{reinterpret_cast<void*>(&on_build),reinterpret_cast<void*>(&on_read),reinterpret_cast<void*>(&on_issue)},originals{};
        size_t created{};for(;created<3;++created)if(MH_CreateHook(targets[created],replacements[created],&originals[created])!=MH_OK)break;
        if(created!=3){while(created)MH_RemoveHook(targets[--created]);return false;}
        m_read=reinterpret_cast<Read>(originals[1]);m_issue=reinterpret_cast<Issue>(originals[2]);m_build=reinterpret_cast<Build>(originals[0]);
        size_t enabled{};for(;enabled<3;++enabled)if(MH_EnableHook(targets[enabled])!=MH_OK)break;
        if(enabled!=3){for(auto target:targets){MH_DisableHook(target);MH_RemoveHook(target);}m_build=nullptr;m_read=nullptr;m_issue=nullptr;return false;}
        return true;
    }
    bool install_in_game(uintptr_t base){
        const auto build=game_build::rva(FLARE_BUILD),read=game_build::rva(FLARE_READ),issue=game_build::rva(FLARE_ISSUE);std::array<uint8_t,16> b{},r{},i{};
        if(!build || !read || !issue || !ordinary_scene_scope::read(base+build,b) || !ordinary_scene_scope::read(base+read,r) || !ordinary_scene_scope::read(base+issue,i) ||
           !game_build::code(FLARE_BUILD,b,FLARE_BUILD_BYTES) || !game_build::code(FLARE_READ,r,FLARE_READ_BYTES) || !game_build::code(FLARE_ISSUE,i,FLARE_ISSUE_BYTES))return false;
        return install(reinterpret_cast<void*>(base+build),reinterpret_cast<void*>(base+read),reinterpret_cast<void*>(base+issue));
    }
    // Blocks of one flare after its builder call (exposed for tests).
    void sample(uintptr_t flare,int32_t cameras){
        std::lock_guard lock{m_mutex};++m_window.builds;m_window.cameras=std::max(m_window.cameras,cameras);
        for(size_t i=0;i<2;++i){const auto block=flare+i*0x60;uint8_t flags{};float current{},target{};uint32_t total{};
            if(!ordinary_scene_scope::read(block,flags) || !ordinary_scene_scope::read(block+8,current) || !ordinary_scene_scope::read(block+0xc,target) || !ordinary_scene_scope::read(block+0x28,total))continue;
            auto& b=m_window.blocks[i];++b.flares;b.queried+=total!=0;b.wanted+=(flags&4)!=0;b.pending+=(flags&8)!=0;b.current+=current;b.target+=target;}
    }
    void count(bool issue,uintptr_t view){
        const auto scene=NativeRenderProbe::current_scene();const size_t slot=scene && scene->slot<2?scene->slot:2;
        uint8_t index=0xff;ordinary_scene_scope::read(view+0x33,index);
        std::lock_guard lock{m_mutex};(issue?m_window.issues:m_window.reads)[slot][index<2?index:2]++;
    }
    // One log line for the window since the last call, then a fresh window.
    std::string line(){
        std::lock_guard lock{m_mutex};std::ostringstream out;const auto& w=m_window;
        out<<"builds="<<w.builds<<" cameras="<<w.cameras;
        for(size_t i=0;i<2;++i){const auto& b=w.blocks[i];const double n=b.flares?double(b.flares):1;
            out<<" block"<<i<<"={queried="<<b.queried<<" wanted="<<b.wanted<<" pending="<<b.pending<<" current="<<b.current/n<<" target="<<b.target/n<<'}';}
        for(const char* kind:{"reads","issues"}){const auto& table=kind[0]=='r'?w.reads:w.issues;out<<' '<<kind<<'=';
            for(size_t s=0;s<3;++s){out<<(s?"/":"")<<"s"<<(s<2?char('0'+s):'x')<<':'<<table[s][0]<<','<<table[s][1]<<','<<table[s][2];}}
        out<<" hide="<<(m_hide.load()?"on":"off")<<" hidden="<<m_hidden.exchange(0);
        m_window={};return out.str();
    }
private:
    void poll(){
        const auto now=std::chrono::steady_clock::now();if(now<m_next.load())return;
        std::unique_lock lock{m_control_mutex,std::try_to_lock};if(!lock || m_control.empty())return;m_next=now+std::chrono::seconds(1);
        std::ifstream in{m_control};std::string word;if(in>>word){if(word=="hide")m_hide.store(true);else if(word=="native")m_hide.store(false);}
    }
    static void on_build(uintptr_t flare,uintptr_t cameras,uintptr_t a,uintptr_t b,uint32_t c,uint8_t d){
        auto& self=get();self.m_build(flare,cameras,a,b,c,d);
        try{int32_t count{};ordinary_scene_scope::read(cameras+0x29b14,count);self.sample(flare,count);self.hide(flare);}catch(...){}
    }
    static void on_read(uintptr_t manager,uint8_t index,uintptr_t view){auto& self=get();try{self.count(false,view);}catch(...){}self.m_read(manager,index,view);}
    static void on_issue(uintptr_t manager,uint8_t index,uintptr_t command,uintptr_t view,uintptr_t target){auto& self=get();try{self.count(true,view);}catch(...){}self.m_issue(manager,index,command,view,target);}
    Build m_build{};Read m_read{};Issue m_issue{};std::mutex m_mutex;Window m_window;
    std::atomic_bool m_hide{};std::atomic<uint64_t> m_hidden{};std::mutex m_control_mutex;std::filesystem::path m_control;
    std::atomic<std::chrono::steady_clock::time_point> m_next{};
};
