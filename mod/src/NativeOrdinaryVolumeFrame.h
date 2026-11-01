#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <intrin.h>
#include <mutex>

// Opt-in experiment: the native world-space volume setup advances two shared
// history banks and camera origins. Advance once for an authenticated stereo job,
// keeping the completed left setup for its immediately matching right view.
class NativeOrdinaryVolumeFrame {
public:
    using Scope=ordinary_scene_scope::Scope;
    using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,float);
    using Provider=std::function<std::optional<Scope>()>;
    static NativeOrdinaryVolumeFrame& get(){static auto* p=new NativeOrdinaryVolumeFrame;return *p;}
    bool install(void* target,Provider provider,std::function<uintptr_t()> owner,uintptr_t caller=0){
        if(m_original || !target || !provider || !owner)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        void* original{};if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),&original)!=MH_OK)return false;
        m_original=reinterpret_cast<Native>(original);m_provider=std::move(provider);m_owner=std::move(owner);m_caller=caller;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_original=nullptr;return false;
    }
    bool install_in_game(uintptr_t base){
        constexpr game_build::Code expected=game_build::hashed(0x1fe3dd0,14,"34118ad31b351139110dbbaeea7553157f9c700dbd9983c057f76f7765582661");
        constexpr game_build::Code call=game_build::hashed(0x1805cab,5,"0f099e1e6b8fb0aeae59586f63b30939d9fd71169a628e39613cb8f50880ae27");
        std::array<uint8_t,14> bytes{};std::array<uint8_t,5> site{};
        if(!ordinary_scene_scope::read(base+game_build::rva(0x1fe3dd0),bytes) || !game_build::code(0x1fe3dd0,bytes,expected) || !ordinary_scene_scope::read(base+game_build::rva(0x1805cab),site) || !game_build::code(0x1805cab,site,call))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(0x1fe3dd0)),[]()->std::optional<Scope>{
            const auto s=NativeRenderProbe::current_scene();const auto p=NativeRenderProbe::current_ordinary_pair();if(!s || !p)return {};return Scope{*s,*p};
        },[base]{uintptr_t owner{};ordinary_scene_scope::read(base+game_build::rva(0x4faeec8),owner);return owner;},base+game_build::rva(0x1805cb0));
    }
    uint64_t shared()const{std::lock_guard lock{m_mutex};return m_shared;}
private:
    static void on_call(uintptr_t owner,uintptr_t command,uintptr_t entry,float delta){
        auto& self=get();std::optional<Scope> scope;bool skip{};
        try{
            if((!self.m_caller || self.m_caller==reinterpret_cast<uintptr_t>(_ReturnAddress())) && owner && owner==self.m_owner()){
                scope=self.m_provider();if(scope && (scope->scene.entry!=entry || ordinary_scene_scope::inspect(*scope).mask))scope.reset();
            }
            std::lock_guard lock{self.m_mutex};
            if(scope && scope->scene.slot==1 && self.m_left && self.m_owner_seen==owner && self.m_command==command && self.m_delta==delta && std::isfinite(delta) && self.m_thread==GetCurrentThreadId()){
                auto normalized=*scope;normalized.scene.slot=0;normalized.scene.ordinal=1;normalized.scene.entry=scope->pair.left.entry;
                skip=ordinary_scene_scope::same(*self.m_left,normalized);
            }
            self.m_left.reset();if(skip)++self.m_shared;
        }catch(...){scope.reset();}
        if(skip)return;
        self.m_original(owner,command,entry,delta);
        if(scope && scope->scene.slot==0){
            std::lock_guard lock{self.m_mutex};
            if(scope->scene.dispatch_sequence>self.m_last_left){
                self.m_last_left=scope->scene.dispatch_sequence;self.m_left=*scope;
                self.m_owner_seen=owner;self.m_command=command;self.m_delta=delta;self.m_thread=GetCurrentThreadId();
            }
        }
    }
    Native m_original{};Provider m_provider;std::function<uintptr_t()> m_owner;uintptr_t m_caller{};
    mutable std::mutex m_mutex;std::optional<Scope> m_left;uint64_t m_last_left{},m_shared{};
    uintptr_t m_owner_seen{},m_command{};float m_delta{};DWORD m_thread{};
};
