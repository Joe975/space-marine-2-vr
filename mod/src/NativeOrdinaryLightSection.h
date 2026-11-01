#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>

// The right eye lights its tiles with its own lights (docs/RIGHT_EYE_FLICKER_2026-10-28.md, 29 Oct).
//
// Each Scene builds its tile light bitmask and zbins from its slot's light lists (0x1f986d0 reads
// the slot), so bit i means "this view's light i". The light data the shader indexes with those
// bits is uploaded by 0x1f8b2c0(renderer, destination, view, flag), called once per Scene from
// 0x1f99260 (call at 0x1f99d5f) with view = the camera entry's HDR core index [E+0x188]. That index
// is 0 for both owned eyes, so the right eye's tiles fetched the LEFT eye's light i: wrong lights
// in tile-shaped blocks wherever the two lists differ, churning as view 1's list changes
// (build/tile_re/NOTES.md). In slot 1's authenticated Scene, at that call site only, "slot" passes
// view 1: the section built from the same list as the bitmask. Section 1 is scaled by HDR core 1's
// exposure, which the adaptation mirror keeps equal to core 0's (on by default).
class NativeOrdinaryLightSection {
public:
    using Native=void(*)(void*,void*,int,char);
    enum class Mode {native,slot};
    static std::optional<Mode> parse(std::string_view line){
        while(!line.empty() && (line.back()=='\r' || line.back()=='\n' || line.back()==' '))line.remove_suffix(1);
        if(line=="native")return Mode::native;
        if(line=="slot")return Mode::slot;
        return std::nullopt;
    }
    // The view index to upload: the authenticated Scene's slot when it is the right eye's and the
    // native index is the left eye's section; anything else is left alone.
    static int view(Mode mode,bool site,std::optional<unsigned> slot,int native){
        return mode==Mode::slot && site && slot && *slot==1 && native==0?1:native;
    }
    // A return address is the light-upload site if the 5 bytes before it call `target` and it lies
    // in the caller (0x1f99260, under 0x1000 bytes before the call).
    static bool site_call(uintptr_t ret,uintptr_t target,uintptr_t caller,const std::array<uint8_t,5>& call){
        if(!ret || !target || !caller || ret<caller || ret-caller>0x1000 || call[0]!=0xe8)return false;
        int32_t rel{};std::memcpy(&rel,call.data()+1,4);return ret+int64_t(rel)==target;
    }
    static NativeOrdinaryLightSection& get(){static auto* p=new NativeOrdinaryLightSection;return *p;}
    // path: the control file (light_section.txt); missing or unreadable keeps the current mode.
    bool install_in_game(uintptr_t base,const std::filesystem::path& path){
        constexpr game_build::Code expected=game_build::hashed(0x1f8b2c0,16,"9fb08dd66d03e3f5ef4b9636e09f9a6d7fd4edb9d524808c12261fb44cd9aa1f");
        std::array<uint8_t,16> actual{};
        if(m_native || !game_build::rva(0x1f8b2c0) || !game_build::rva(0x1f99260) ||
           !ordinary_scene_scope::read(base+game_build::rva(0x1f8b2c0),actual) || !game_build::code(0x1f8b2c0,actual,expected))return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        auto* target=reinterpret_cast<void*>(base+game_build::rva(0x1f8b2c0));
        {std::lock_guard lock{m_mutex};m_target=base+game_build::rva(0x1f8b2c0);m_caller=base+game_build::rva(0x1f99260);m_path=path;}
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_native=nullptr;return false;
    }
    bool installed()const{return m_native!=nullptr;}
    // calls, site calls, slot 0, slot 1, swapped
    std::array<uint64_t,5> stats()const{std::lock_guard lock{m_mutex};return m_counts;}
    Mode mode()const{std::lock_guard lock{m_mutex};return m_mode;}
private:
    static void on_call(void* renderer,void* destination,int native,char flag){
        auto& self=get();const auto ret=reinterpret_cast<uintptr_t>(_ReturnAddress());int chosen=native;
        try{
            std::lock_guard lock{self.m_mutex};++self.m_counts[0];
            const auto now=std::chrono::steady_clock::now();
            if(!self.m_path.empty() && now>=self.m_next){self.m_next=now+std::chrono::seconds(1);std::ifstream file{self.m_path};std::string line;if(std::getline(file,line))if(const auto m=parse(line))self.m_mode=*m;}
            if(ret!=self.m_site){std::array<uint8_t,5> call{};if(ordinary_scene_scope::read(ret-5,call) && site_call(ret,self.m_target,self.m_caller,call))self.m_site=ret;}
            const bool site=ret==self.m_site;
            std::optional<unsigned> slot;
            if(site){
                ++self.m_counts[1];
                const auto scene=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();
                if(scene && pair){const ordinary_scene_scope::Scope scope{*scene,*pair};if(!ordinary_scene_scope::inspect(scope).mask && NativeCameraEntries::get().current(*pair))slot=scope.scene.slot;}
                if(slot && *slot<2)++self.m_counts[2+*slot];
            }
            chosen=view(self.m_mode,site,slot,native);
            if(chosen!=native)++self.m_counts[4];
        }catch(...){chosen=native;}
        self.m_native(renderer,destination,chosen,flag);
    }
    Native m_native{};uintptr_t m_target{},m_caller{},m_site{};std::filesystem::path m_path;
    std::chrono::steady_clock::time_point m_next{};Mode m_mode{Mode::slot};
    mutable std::mutex m_mutex;std::array<uint64_t,5> m_counts{};
};
