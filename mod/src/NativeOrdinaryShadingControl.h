#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <filesystem>
#include <fstream>
#include <mutex>

// Diagnostic, installed in every headset launch but inert there (shading.txt light_union* = mask 0):
// select native disabled branches for authenticated ordinary
// eyes. No resources or global game settings are edited. Freeze the choice for
// the whole dispatch so changing the file cannot give the two eyes different modes.
class NativeOrdinaryShadingControl {
public:
    using Scope=ordinary_scene_scope::Scope;
    using Native=uintptr_t(*)(uintptr_t);
    using Provider=std::function<std::optional<Scope>()>;
    struct Evidence {uint64_t dispatch{};unsigned slot{};bool complete{};std::array<int32_t,4> lights{},shadows{};};
    static Evidence read_state(uintptr_t base,const Scope& scope){
        Evidence e;e.dispatch=scope.scene.dispatch_sequence;e.slot=scope.scene.slot;
        if(!base || ordinary_scene_scope::inspect(scope).mask)return e;
        uintptr_t lights{},shadows{};
        if(!ordinary_scene_scope::read(base+game_build::rva(0x4faf810),lights) || !ordinary_scene_scope::read(base+game_build::rva(0x4faef88),shadows) || !lights || !shadows)return e;
        for(size_t i=0;i<4;++i)if(!ordinary_scene_scope::read(lights+0x90+i*0x44,e.lights[i]) || !ordinary_scene_scope::read(shadows+0xff60+i*0x3ed0,e.shadows[i]))return e;
        e.complete=!ordinary_scene_scope::inspect(scope).mask;return e;
    }
    std::array<Evidence,2> evidence()const{std::lock_guard lock{m_mutex};return m_evidence;}
    static NativeOrdinaryShadingControl& get(){static auto* p=new NativeOrdinaryShadingControl;return *p;}
    static std::optional<unsigned> parse(const std::string& value){
        if(value=="stereo_cubemaps" || value=="native_reflections")return 0;
        if(value=="none" || value=="light_visibility" || value=="light_union" || value=="light_union_shadow_refresh" || value=="light_union_shadow_refresh_native_volumetrics" || value=="light_union_shadow_refresh_native_shadow_lights" || value=="light_union_shadow_refresh_shared_spot_shadows")return 0;if(value=="ssr")return 1;if(value=="ssao")return 2;
        if(value=="screen_shadows" || value=="light_union_screen_shadows")return 4;
        if(value=="all" || value=="light_union_all")return 7;
        if(value=="light_union_ssr")return 1;if(value=="light_union_ssao")return 2;
        // Shadow refresh plus effect isolation: localizes what differs between eyes once the
        // cascades agree (scripts/capture_eye_parity.py variants).
        if(value=="light_union_shadow_refresh_ssr")return 1;if(value=="light_union_shadow_refresh_ssao")return 2;
        if(value=="light_union_shadow_refresh_screen_shadows")return 4;if(value=="light_union_shadow_refresh_all")return 7;return {};
    }
    bool install(const std::array<void*,3>& targets,Provider provider,std::function<uintptr_t()> owner,std::function<std::optional<unsigned>()> control){
        if(m_original[0] || !provider || !owner || !control || std::ranges::any_of(targets,[](auto p){return !p;}))return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        const std::array<void*,3> hooks{reinterpret_cast<void*>(&on_call<0>),reinterpret_cast<void*>(&on_call<1>),reinterpret_cast<void*>(&on_call<2>)};
        size_t made{};for(;made<3;++made)if(MH_CreateHook(targets[made],hooks[made],reinterpret_cast<void**>(&m_original[made]))!=MH_OK)break;
        if(made!=3){while(made)MH_RemoveHook(targets[--made]);m_original={};return false;}
        m_provider=std::move(provider);m_owner=std::move(owner);m_control=std::move(control);
        size_t enabled{};for(;enabled<3;++enabled)if(MH_EnableHook(targets[enabled])!=MH_OK)break;
        if(enabled==3)return true;
        for(auto target:targets){MH_DisableHook(target);MH_RemoveHook(target);}m_original={};return false;
    }
    bool install_in_game(uintptr_t base,const std::filesystem::path& path){
        if(!path.is_absolute())return false;
        constexpr std::array<uintptr_t,3> rvas{0x208c720,0x208c800,0x208c8e0};
        // The three functions open with the same 16 bytes.
        constexpr std::array<game_build::Code,3> expected{game_build::hashed(0x208c720,16,"97de461370e7cd95f6873b0503f6b75bcec64fd722f6d27fd183a89433e054de"),
            game_build::hashed(0x208c800,16,"97de461370e7cd95f6873b0503f6b75bcec64fd722f6d27fd183a89433e054de"),
            game_build::hashed(0x208c8e0,16,"97de461370e7cd95f6873b0503f6b75bcec64fd722f6d27fd183a89433e054de")};
        std::array<void*,3> targets{};
        for(size_t i=0;i<3;++i){std::array<uint8_t,16> bytes{};const auto at=game_build::rva(rvas[i]);if(!at || !ordinary_scene_scope::read(base+at,bytes) || !game_build::code(rvas[i],bytes,expected[i]))return false;targets[i]=reinterpret_cast<void*>(base+at);}
        m_base=base;
        return install(targets,[]()->std::optional<Scope>{const auto s=NativeRenderProbe::current_scene();const auto p=NativeRenderProbe::current_ordinary_pair();if(!s || !p)return {};return Scope{*s,*p};},
            [base]{uintptr_t owner{};ordinary_scene_scope::read(base+game_build::rva(0x4d62e40),owner);return owner;},
            [path,next=std::chrono::steady_clock::time_point{}]()mutable->std::optional<unsigned>{
                const auto now=std::chrono::steady_clock::now();if(now<next)return {};next=now+std::chrono::seconds(1);
                std::ifstream file{path};std::string line;if(!std::getline(file,line) || line.size()>32)return {};
                if(!line.empty() && line.back()=='\r')line.pop_back();return parse(line);
            });
    }
    std::array<uint64_t,4> stats()const{std::lock_guard lock{m_mutex};return {m_mask,m_calls[0],m_calls[1],m_calls[2]};}
private:
    template<size_t I>static uintptr_t on_call(uintptr_t owner){
        auto& self=get();const auto original=self.m_original[I](owner);
        try{
            if(!owner || owner!=self.m_owner())return original;
            const auto scope=self.m_provider();if(!scope || ordinary_scene_scope::inspect(*scope).mask)return original;
            std::lock_guard lock{self.m_mutex};const auto dispatch=scope->scene.dispatch_sequence;
            if(dispatch<self.m_dispatch)return original;
            if(dispatch>self.m_dispatch){
                self.m_dispatch=dispatch;if(const auto value=self.m_control();value && *value<=7)self.m_mask=*value;
            }
            if(self.m_base && dispatch%120==0 && self.m_evidence[scope->scene.slot].dispatch!=dispatch)
                self.m_evidence[scope->scene.slot]=read_state(self.m_base,*scope);
            if(self.m_mask&(1u<<I)){++self.m_calls[I];return original&~uintptr_t{255};}
        }catch(...){}
        return original;
    }
    std::array<Native,3> m_original{};Provider m_provider;std::function<uintptr_t()> m_owner;std::function<std::optional<unsigned>()> m_control;
    mutable std::mutex m_mutex;uint64_t m_dispatch{};unsigned m_mask{};std::array<uint64_t,3> m_calls{};
    uintptr_t m_base{};std::array<Evidence,2> m_evidence{};
};
