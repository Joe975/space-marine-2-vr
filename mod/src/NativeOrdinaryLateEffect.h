#pragma once
#include "NativeCallCensus.h"
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <intrin.h>
#include <functional>
#include <mutex>

// Four-job isolation experiment for the native post-DLSS pass that swaps the
// scene wrapper. Its own disabled branch is a no-op. Skipping preserves both
// caller-owned wrapper references; the caller still releases its unused target.
class NativeOrdinaryLateEffect {
public:
    using Scope=ordinary_scene_scope::Scope;
    using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
    using Early=void(*)(uintptr_t,uint8_t,uintptr_t,uintptr_t,uintptr_t,float);
    using Provider=std::function<std::optional<Scope>()>;
    std::function<void(const Scope&,uintptr_t)> early_input;
    struct Record {uint64_t dispatch{};std::array<uint32_t,2> skipped{},early_disabled{};};
    static NativeOrdinaryLateEffect& get(){static auto* value=new NativeOrdinaryLateEffect;return *value;}
    void continuous(bool value){m_continuous=value;}
    bool install(void* target,uintptr_t owner,Provider provider,uintptr_t caller=0){
        if(m_original || !target || !owner || !provider)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        void* original{};if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),&original)!=MH_OK)return false;
        m_owner=owner;m_provider=std::move(provider);m_caller=caller;m_original=reinterpret_cast<Native>(original);
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_original=nullptr;return false;
    }
    bool install_in_game(uintptr_t base){
        constexpr game_build::Code expected=game_build::hashed(0x1fabdd0,16,"9795d0c3868a2a082808916883ecf0b5af4367307346f31184b232e156ff35aa");
        constexpr game_build::Code call=game_build::hashed(0x180aa54,5,"22ff748ba1c8906d32e7aa941c088f5bb8f94dadd9ec23f73788a9762d41231c");std::array<uint8_t,16> bytes{};std::array<uint8_t,5> site{};
        if(!ordinary_scene_scope::read(base+game_build::rva(0x1fabdd0),bytes) || !game_build::code(0x1fabdd0,bytes,expected) || !ordinary_scene_scope::read(base+game_build::rva(0x180aa54),site) || !game_build::code(0x180aa54,site,call))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(0x1fabdd0)),base+game_build::rva(0x4faf840),[]()->std::optional<Scope>{
            const auto scene=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();if(!scene || !pair)return {};return Scope{*scene,*pair};
        },base+game_build::rva(0x180aa59));
    }
    bool install_early(void* target,uintptr_t caller=0){
        if(!m_original || m_early || !target)return false;void* original{};
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_early),&original)!=MH_OK)return false;
        m_early=reinterpret_cast<Early>(original);m_early_caller=caller;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_early=nullptr;return false;
    }
    bool install_early_in_game(uintptr_t base){
        constexpr game_build::Code expected=game_build::hashed(0x1fabae0,15,"f7ff11bd0d477e15ae7d55f7ef3ebb558b12c870bc5a1d4e50b3b0036205eb3a");
        constexpr game_build::Code call=game_build::hashed(0x180a96f,5,"4a281b74535e670a32fde02a4c1fc754f07d110c94d5926765774a271fdbdbef");std::array<uint8_t,15> bytes{};std::array<uint8_t,5> site{};
        if(!ordinary_scene_scope::read(base+game_build::rva(0x1fabae0),bytes) || !game_build::code(0x1fabae0,bytes,expected) || !ordinary_scene_scope::read(base+game_build::rva(0x180a96f),site) || !game_build::code(0x180a96f,site,call))return false;
        return install_early(reinterpret_cast<void*>(base+game_build::rva(0x1fabae0)),base+game_build::rva(0x180a974));
    }
    void write(std::ostream& out,uint64_t dispatch){std::lock_guard lock{m_mutex};out<<"{\"installed\":"<<(m_original?"true":"false")<<",\"skipped\":[";const auto r=std::ranges::find(m_records,dispatch,&Record::dispatch);out<<(r==m_records.end()?0:r->skipped[0])<<','<<(r==m_records.end()?0:r->skipped[1])<<"],\"early_installed\":"<<(m_early?"true":"false")<<",\"early_disabled\":["<<(r==m_records.end()?0:r->early_disabled[0])<<','<<(r==m_records.end()?0:r->early_disabled[1])<<"]}";}
private:
    bool skip(uintptr_t owner,uintptr_t entry,uintptr_t caller,bool early=false){
        const auto expected=early?m_early_caller:m_caller;
        if(owner!=m_owner || (expected && caller!=expected))return false;const auto scope=m_provider();
        if(!scope || entry!=scope->scene.entry || ordinary_scene_scope::inspect(*scope).mask)return false;
        std::lock_guard lock{m_mutex};auto r=std::ranges::find(m_records,scope->scene.dispatch_sequence,&Record::dispatch);
        if(r==m_records.end()){if(!m_records.empty() && scope->scene.dispatch_sequence<=m_records.back().dispatch)return false;if(m_records.size()>=4){if(!m_continuous)return false;m_records.erase(m_records.begin());}m_records.push_back({scope->scene.dispatch_sequence});r=std::prev(m_records.end());}
        auto& count=early?r->early_disabled[scope->scene.slot]:r->skipped[scope->scene.slot];if(count)return false;++count;return true;
    }
    static void on_call(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h,uintptr_t i,uintptr_t j){
        NativeCallCensus::get().count(NativeCallCensus::Hook::late_effect);
        auto& self=get();try{if(self.skip(a,e,reinterpret_cast<uintptr_t>(_ReturnAddress())))return;}catch(...){}
        self.m_original(a,b,c,d,e,f,g,h,i,j);
    }
    static void on_early(uintptr_t a,uint8_t b,uintptr_t c,uintptr_t d,uintptr_t e,float f){
        NativeCallCensus::get().count(NativeCallCensus::Hook::early_effect);
        auto& self=get();try{if(self.skip(a,e,reinterpret_cast<uintptr_t>(_ReturnAddress()),true)){b=0;if(self.early_input)if(const auto scope=self.m_provider())self.early_input(*scope,c);}}catch(...){}
        // Preserve native pool acquisition/release and all original arguments
        // except its explicit enabled flag. The native disabled branch is used.
        self.m_early(a,b,c,d,e,f);
    }
    Native m_original{};Early m_early{};uintptr_t m_owner{},m_caller{},m_early_caller{};Provider m_provider;std::mutex m_mutex;std::vector<Record> m_records;bool m_continuous{};
};
