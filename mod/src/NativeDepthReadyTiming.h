#pragma once
#include "NativeSceneWaitTiming.h"
#include "GameBuild.h"
#include "NativeQueryDependencyProbe.h"

// Passive observation of the authenticated ZMIPS_READY_0 CPU fence helper.
// This is elapsed wait time, not active CPU. Never changes or defers the wait.
class NativeDepthReadyTiming {
public:
    using Native=void(*)(uintptr_t);
    using Provider=NativeSceneWaitTiming::Provider;
    using Observer=std::function<void(const char*,double,double,double)>;
    static NativeDepthReadyTiming& get(){static auto* value=new NativeDepthReadyTiming;return *value;}
    static constexpr uintptr_t HELPER_RVA=0x180c150; // Reference build; ported through game_build.
    inline static constexpr game_build::Code HELPER_BYTES=game_build::hashed(0x180c150,16,"6397911e5fd92a8d9e720a46129f236beaa833e262e7164c72b3e9bc129cbbb3");
    bool install(void* target,Provider provider,Observer observer){
        if(m_original || !target || !provider || !observer)return false;
        const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_wait),reinterpret_cast<void**>(&m_original))!=MH_OK)return false;
        m_provider=std::move(provider);m_observer=std::move(observer);
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_original=nullptr;return false;
    }
    bool install_in_game(uintptr_t base){
        std::array<uint8_t,16> actual{};
        if(!ordinary_scene_scope::read(base+game_build::rva(HELPER_RVA),actual) || !game_build::code(HELPER_RVA,actual,HELPER_BYTES))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(HELPER_RVA)),[]{return NativeRenderProbe::current_scene();},
            [](const char* name,double start,double duration,double id){PerformanceTrace::get().record(name,start,duration,id);});
    }
private:
    static void on_wait(uintptr_t object){
        auto& self=get();const auto incoming=GetLastError();std::optional<NativeRenderProbe::Scene> scene;
        try{scene=self.m_provider();}catch(...){}
        const bool scoped=scene && scene->valid() && scene->slot<2 && !(scene->view_flags_before&0x50000044);
        const auto start=scoped?PerformanceTrace::now_ms():0;
        SetLastError(incoming);self.m_original(object);const auto outgoing=GetLastError();
        if(SceneQueryDependencyTiming::active())NativeQueryDependencyProbe::get().after_fence();
        if(scoped)try{self.m_observer(scene->slot?"scene_right_depth_ready_cpu":"scene_left_depth_ready_cpu",start,PerformanceTrace::now_ms()-start,double(scene->dispatch_sequence));}catch(...){}
        SetLastError(outgoing);
    }
    Native m_original{};Provider m_provider;Observer m_observer;
};
