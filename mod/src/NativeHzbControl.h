#pragma once
#include "NativeHzbCulling.h"
#include "EnginePlayerVisibilityProbe.h"
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

// Occlusion culling for both eyes. With -HzbControl it follows hzb.txt, polled once a second on the
// render thread, so the switch only changes between visibility passes.
class NativeHzbControl {
public:
    using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,char);
    using LwiPass=void(*)(uintptr_t,uintptr_t,uint32_t);
    struct Stats {uint64_t calls{},second_eye{},checked{},inherited{},lwi_changes{},wide_gap{};native_hzb_culling::Mode mode{};bool lwi{};float edge{};};
    static NativeHzbControl& get(){static auto* p=new NativeHzbControl;return *p;}
    bool install_in_game(uintptr_t base,const std::filesystem::path& path){
        constexpr game_build::Code expected=game_build::hashed(0x1ee1130,16,"0d254e0685304e2754cbe5a8431b6d58d0761bb9c3e7b66b53457475d7622995");
        constexpr game_build::Code lwi_pass=game_build::hashed(0x1db9cb0,16,"734c8613243cf80d908527f5b6f068d3747c76e2889f20c52625913ee4e5bd7a");
        constexpr game_build::Code lwi=game_build::hashed(0x1db9535,23,"2572a9b41c6bcadcbb25082e59d1c6dd0d23924691c4197a0bef87d228dc339f");
        const auto target=game_build::rva(native_hzb_culling::VISIBILITY_RVA),flag=game_build::rva(native_hzb_culling::SWITCH_RVA);
        std::array<uint8_t,16> bytes{};
        if(m_native || (!path.empty() && !path.is_absolute()) || !target || !flag || !ordinary_scene_scope::read(base+target,bytes) ||
           !game_build::code(0x1ee1130,bytes,expected))return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        m_path=path;m_switch=reinterpret_cast<uint8_t*>(base+flag);
        if(MH_CreateHook(reinterpret_cast<void*>(base+target),reinterpret_cast<void*>(&on_call),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        // Optional: a refused site leaves rocks and foliage frustum-only in the right eye (lwi=false).
        // The patch needs the hook that ends the widened view, so it is all or nothing.
        const auto site=game_build::rva(native_hzb_culling::LWI_KEY_RVA),pass=game_build::rva(native_hzb_culling::LWI_PASS_RVA);native_hzb_culling::LwiKey native{};std::array<uint8_t,16> pass_bytes{};
        if(site && pass && ordinary_scene_scope::read(base+site,native) && game_build::code(0x1db9535,native,lwi) &&
           ordinary_scene_scope::read(base+pass,pass_bytes) && game_build::code(0x1db9cb0,pass_bytes,lwi_pass) &&
           MH_CreateHook(reinterpret_cast<void*>(base+pass),reinterpret_cast<void*>(&on_lwi_pass),reinterpret_cast<void**>(&m_lwi_pass))==MH_OK &&
           MH_EnableHook(reinterpret_cast<void*>(base+pass))==MH_OK){
            m_lwi_site=base+site;m_lwi_native=native;m_lwi_patch=native_hzb_culling::lwi_key_patch(native_hzb_culling::lwi_switch_disp(base+site,base+flag));
        }
        if(MH_EnableHook(reinterpret_cast<void*>(base+target))==MH_OK){EnginePlayerVisibilityProbe::set_mask_filter(&on_object);return true;}
        MH_RemoveHook(reinterpret_cast<void*>(base+target));m_native=nullptr;return false;
    }
    Stats stats()const{return {m_calls.load(),m_second_eye.load(),m_checked.load(),m_inherited.load(),m_lwi_changes.load(),m_wide_gap.load(),m_mode.load(),m_lwi_on,m_edge.load()};}
private:
    static void on_call(uintptr_t manager,uintptr_t views,uintptr_t cameras,char pass){
        using namespace native_hzb_culling;
        auto& self=get();const auto mode=self.poll();++self.m_calls;
        self.m_wide.settle();
        const uint8_t want=switch_value(mode);if(*self.m_switch<=1 && *self.m_switch!=want)*self.m_switch=want;
        uint32_t* second{};bool pair{};uint8_t* left{};float edge{};
        if(second_eye_frustum_only(mode) && cameras){
            left=reinterpret_cast<uint8_t*>(cameras+LIST_ENTRIES);auto* right=left+ENTRY_STRIDE;
            auto* left_flags=reinterpret_cast<uint32_t*>(left);auto* right_flags=reinterpret_cast<uint32_t*>(right);
            if(stereo_pair(*reinterpret_cast<int32_t*>(cameras+LIST_COUNT),*left_flags,left[ENTRY_SLOT],*right_flags,right[ENTRY_SLOT])){
                second=right_flags;pair=true;++self.m_second_eye;
                edge=wide_edge(reinterpret_cast<float*>(left+ENTRY_RELATIVE_VIEW_PROJECTION),reinterpret_cast<float*>(right+ENTRY_RELATIVE_VIEW_PROJECTION));
                self.m_edge.store(edge,std::memory_order_relaxed);
                const bool close=eyes_close(reinterpret_cast<float*>(left+ENTRY_POSITION),reinterpret_cast<float*>(right+ENTRY_POSITION)) && edge;
                self.m_share.store(close,std::memory_order_relaxed);if(!close)++self.m_wide_gap;
            }
        }
        self.set_lwi(pair && self.m_share.load(std::memory_order_relaxed) && lwi_inherits(mode));
        if(pair && self.m_lwi_on)self.m_wide.widen(left,edge);
        ScopedSecondEye hold{second};
        self.m_native(manager,views,cameras,pass);
    }
    // Native order: the visibility pass, then this pass, which reads the left eye's matrix for rocks and foliage.
    static void on_lwi_pass(uintptr_t views,uintptr_t cameras,uint32_t pass){
        auto& self=get();self.m_lwi_pass(views,cameras,pass);self.m_wide.settle();
    }
    // Runs on the game's submission workers, once per object per frame, so the common case is one load.
    static void on_object(void* views,void* instance,uint64_t* mask){
        using namespace native_hzb_culling;
        auto& self=get();
        if(!objects_inherit(self.m_mode.load(std::memory_order_relaxed)) || !self.m_share.load(std::memory_order_relaxed) || !inherit_candidate(*mask))return;
        const auto list=reinterpret_cast<uintptr_t>(views);
        auto* left=reinterpret_cast<uint8_t*>(list+LIST_ENTRIES);auto* right=left+ENTRY_STRIDE;
        if(!stereo_pair(*reinterpret_cast<int32_t*>(list+LIST_COUNT),*reinterpret_cast<uint32_t*>(left),left[ENTRY_SLOT],*reinterpret_cast<uint32_t*>(right),right[ENTRY_SLOT]))return;
        self.m_checked.fetch_add(1,std::memory_order_relaxed);
        const bool inside=wholly_inside(left+ENTRY_FRUSTUM,reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(instance)+OBJECT_BOUNDS));
        const auto after=inherit(*mask,inside);
        if(after!=*mask){*mask=after;self.m_inherited.fetch_add(1,std::memory_order_relaxed);}
    }
    // The LWI dispatch runs right after the visibility pass on this thread, so the bytes change between uses.
    void set_lwi(bool on){
        if(on==m_lwi_on || !m_lwi_site)return;
        const auto& bytes=on?m_lwi_patch:m_lwi_native;auto* at=reinterpret_cast<void*>(m_lwi_site);DWORD old{};
        if(!VirtualProtect(at,bytes.size(),PAGE_EXECUTE_READWRITE,&old))return;
        std::memcpy(at,bytes.data(),bytes.size());
        DWORD ignored{};VirtualProtect(at,bytes.size(),old,&ignored);FlushInstructionCache(GetCurrentProcess(),at,bytes.size());
        m_lwi_on=on;++m_lwi_changes;
    }
    native_hzb_culling::Mode poll(){
        const auto now=std::chrono::steady_clock::now();
        if(now>=m_next && !m_path.empty()){
            m_next=now+std::chrono::seconds(1);
            std::ifstream file{m_path};std::string line;
            if(std::getline(file,line))if(const auto mode=native_hzb_culling::parse_mode(line))m_mode.store(*mode);
        }
        return m_mode.load();
    }
    Native m_native{};std::filesystem::path m_path;uint8_t* m_switch{};std::chrono::steady_clock::time_point m_next{};
    std::atomic<native_hzb_culling::Mode> m_mode{native_hzb_culling::DEFAULT_MODE};std::atomic<bool> m_share{};
    std::atomic<uint64_t> m_calls{},m_second_eye{},m_checked{},m_inherited{},m_lwi_changes{},m_wide_gap{};
    uintptr_t m_lwi_site{};native_hzb_culling::LwiKey m_lwi_native{},m_lwi_patch{};bool m_lwi_on{};
    LwiPass m_lwi_pass{};native_hzb_culling::WideView m_wide;std::atomic<float> m_edge{};
};
