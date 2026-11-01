#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include "PerformanceTrace.h"
#include "ScenePhaseTiming.h"
#include "SceneExecutionTiming.h"
#include <MinHook.h>
#include <intrin.h>
#include <string_view>

// Opt-in observation only: every native synchronization call still executes
// exactly once with the original object and timeout, preserving all return bits.
class NativeSceneWaitTiming {
public:
    using Native=uint32_t(*)(uintptr_t,uint32_t);
    using Provider=std::function<std::optional<NativeRenderProbe::Scene>()>;
    using Observer=std::function<void(const char*,double,double)>;
    static NativeSceneWaitTiming& get(){static auto* p=new NativeSceneWaitTiming;return *p;}
    // Reference-build addresses (ported through game_build): the wait helper, and the return
    // addresses of its depth, shadow and third calls inside the scene render.
    static constexpr uintptr_t WAIT_RVA=0x26f3c40,DEPTH_CALLER=0x1806490,SHADOW_CALLER=0x1806bd3,THIRD_CALLER=0x1806fc6;
    inline static constexpr game_build::Code WAIT_BYTES=game_build::hashed(0x26f3c40,16,"a9459e4f64f8bad7cb47004917380196f8b44567ae85c656f4bcddc284aacdab");
    bool install(void* target,Provider provider,Observer observer,uintptr_t base=0){
        if(m_original || !target || !provider || !observer)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_wait),reinterpret_cast<void**>(&m_original))!=MH_OK)return false;
        m_provider=std::move(provider);m_observer=std::move(observer);m_base=base;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_original=nullptr;return false;
    }
    bool install_in_game(uintptr_t base){
        std::array<uint8_t,16> actual{};if(!ordinary_scene_scope::read(base+game_build::rva(WAIT_RVA),actual) || !game_build::code(WAIT_RVA,actual,WAIT_BYTES))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(WAIT_RVA)),[]{return NativeRenderProbe::current_scene();},
            [](const char* name,double start,double duration){
                thread_local OtherWaits other;
                other.add(name,start,duration,[](const char* n,double s,double d,double count){PerformanceTrace::get().record(n,s,d,count);});},base);
    }
    // "other" waits come in bursts of short calls (about 34 rows a frame, half of all trace rows on
    // 21 Oct). A burst of one name, each call starting within GAP_MS of the previous one's end, becomes
    // one row: start of the first call, summed duration, value = number of calls. Named waits pass through.
    struct OtherWaits {
        static constexpr double GAP_MS=5;
        const char* name{};double start{},sum{},last_end{};unsigned count{};
        template<class Emit>void add(const char* event,double at,double duration,const Emit& emit){
            if(!std::string_view{event}.ends_with("_other_wait_cpu")){emit(event,at,duration,0);return;}
            if(name && (std::string_view{event}!=name || at-last_end>GAP_MS)){emit(name,start,sum,count);name=nullptr;}
            if(!name){name=event;start=at;sum=0;count=0;}
            sum+=duration;++count;last_end=at+duration;
        }
    };
private:
    static uint32_t on_wait(uintptr_t object,uint32_t timeout){
        auto& self=get();const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());const char* name{};
        try{const auto s=self.m_provider();if(s && s->valid() && s->slot<2 && !(s->view_flags_before&0x50000044)){
            const bool right=s->slot==1;
            if(caller==self.m_base+game_build::rva(DEPTH_CALLER))name=right?"scene_right_depth_wait_cpu":"scene_left_depth_wait_cpu";
            else if(caller==self.m_base+game_build::rva(SHADOW_CALLER))name=right?"scene_right_shadow_wait_cpu":"scene_left_shadow_wait_cpu";
            else if(caller==self.m_base+game_build::rva(THIRD_CALLER))name=right?"scene_right_third_wait_cpu":"scene_left_third_wait_cpu";
            else name=right?"scene_right_other_wait_cpu":"scene_left_other_wait_cpu";
        }}catch(...){}
        const bool detail=name && PerformanceTrace::get().probes();
        const unsigned phase=caller==self.m_base+game_build::rva(DEPTH_CALLER)?0:caller==self.m_base+game_build::rva(SHADOW_CALLER)?1:caller==self.m_base+game_build::rva(THIRD_CALLER)?2:3;
        if(detail && phase<3)ScenePhaseTiming::wait(phase,true,PerformanceTrace::now_ms());
        const auto start=name?PerformanceTrace::now_ms():0;
        const auto result=self.m_original(object,timeout);
        const auto end=name?PerformanceTrace::now_ms():0;
        if(name)try{self.m_observer(name,start,end-start);}catch(...){}
        if(detail && phase<3)ScenePhaseTiming::wait(phase,false,PerformanceTrace::now_ms());
        if(detail && phase==2)SceneExecutionTiming::third_wait_end(PerformanceTrace::now_ms());
        return result;
    }
    Native m_original{};Provider m_provider;Observer m_observer;uintptr_t m_base{};
};
