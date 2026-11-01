#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include "SceneQueryDependencyTiming.h"
#include <MinHook.h>

// Explicit opt-in. Reads bounded native query identifiers, never query results
// before the engine's own retrieval and never writes native memory.
class NativeQueryDependencyProbe {
public:
    using Native=uintptr_t(*)(uintptr_t);
    using Reader=std::function<SceneQueryDependencyTiming::Snapshot(uintptr_t)>;
    static NativeQueryDependencyProbe& get(){static auto* value=new NativeQueryDependencyProbe;return *value;}
    bool install(void* target,Reader reader){
        if(m_original || !target || !reader)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_collect),reinterpret_cast<void**>(&m_original))!=MH_OK)return false;
        m_reader=std::move(reader);
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_original=nullptr;return false;
    }
    bool install_in_game(uintptr_t base){
        constexpr game_build::Code expected=game_build::hashed(0x209fac0,16,"e83d15388f5f71ff5c0995d50c753902e03e272b3240210e2708dff37d120657");
        std::array<uint8_t,16> actual{};if(!ordinary_scene_scope::read(base+game_build::rva(0x209fac0),actual) || !game_build::code(0x209fac0,actual,expected))return false;
        m_base=base;
        if(!install(reinterpret_cast<void*>(base+game_build::rva(0x209fac0)),[this](uintptr_t owner){return snapshot(owner);}))return false;
        SceneQueryDependencyTiming::enabled=true;return true;
    }
    void after_fence(){
        if(!SceneQueryDependencyTiming::active())return;
        uintptr_t owner{};int count{-1};
        if(!m_base || !ordinary_scene_scope::read(m_base+game_build::rva(0x508caa8),owner) || !owner || !ordinary_scene_scope::read(owner+0x1f0,count) || count<0 || count>65536)count=-1;
        SceneQueryDependencyTiming::fence(PerformanceTrace::now_ms(),count);
    }
private:
    SceneQueryDependencyTiming::Snapshot snapshot(uintptr_t owner){
        using ordinary_scene_scope::read;
        SceneQueryDependencyTiming::Snapshot result;uintptr_t expected{},context{};
        if(!read(m_base+game_build::rva(0x4ff0c08),expected) || expected!=owner || !read(m_base+game_build::rva(0x4ff0bd0),result.manager) || !result.manager ||
           !read(m_base+game_build::rva(0x4ff0c10),context) || !context)return result;
        const auto capture=[&](uintptr_t ctx){
            uintptr_t queries{};int count{};
            if(!ctx || !read(ctx+0x18,queries) || !queries || !read(queries+0x60008,count) || count<0 || count>0x4000)return false;
            result.count+=unsigned(count);
            for(int i=0;i<count && result.captured<result.keys.size();++i){
                uint64_t key{};if(!read(queries+0x20008+size_t(i)*8,key))return false;
                result.keys[result.captured++]=key;
            }return true;
        };
        if(!capture(context))return result;
        int workers{};if(!read(owner+0xd0,workers) || workers<0 || workers>3)return result;
        for(int i=0;i<workers;++i){uintptr_t ctx{};if(!read(owner+0xb8+size_t(i)*8,ctx) || !capture(ctx))return result;}
        result.valid=true;return result;
    }
    static uintptr_t on_collect(uintptr_t owner){
        auto& self=get();const auto incoming=GetLastError();const bool active=SceneQueryDependencyTiming::active();
        if(active)try{const auto snapshot=self.m_reader(owner);SceneQueryDependencyTiming::collect_begin(snapshot,PerformanceTrace::now_ms());}catch(...){SceneQueryDependencyTiming::collect_begin({},PerformanceTrace::now_ms());}
        SetLastError(incoming);const auto result=self.m_original(owner);const auto outgoing=GetLastError();
        if(active)SceneQueryDependencyTiming::collect_end(PerformanceTrace::now_ms());
        SetLastError(outgoing);return result;
    }
    Native m_original{};Reader m_reader;uintptr_t m_base{};
};
