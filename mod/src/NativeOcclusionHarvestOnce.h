#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>

// Diagnostic candidate fix for the second-eye light flicker (docs/RIGHT_EYE_FLICKER_2026-10-28.md).
// Scene calls the occlusion-result harvest 0x209fac0 (from 0x1808207) once per view. The harvest
// wipes the whole query-result table (valid bitmap at manager +0x141400) and refills it only with
// the results that arrived since the last call (build/flicker_re/NOTES.md). In mono that is once a
// frame. In stereo the second Scene's call throws away what the first harvested, so the light job
// (once per frame) can find no result (-1) for a light only one eye sees, and that light drops out
// for a frame in a beat set by GPU readback timing. With "once" in the control file, slot 1's
// Scene skips the call: the pending query buffers are not reset, so the next frame's slot-0 call
// harvests both views' queries, as in mono. The caller ignores the return value.
class NativeOcclusionHarvestOnce {
public:
    using Native=uintptr_t(*)(uintptr_t);
    enum class Mode {native,once};
    static std::optional<Mode> parse(std::string_view line){
        if(!line.empty() && line.back()=='\r')line.remove_suffix(1);
        if(line=="native")return Mode::native;
        if(line=="once")return Mode::once;
        return std::nullopt;
    }
    // Skip only slot 1's Scene call, in "once" mode, for an authenticated ordinary pair.
    static bool skip(Mode mode,bool scene_site,std::optional<unsigned> authenticated_slot){
        return mode==Mode::once && scene_site && authenticated_slot && *authenticated_slot==1;
    }
    // A return address is the Scene site if the 5 bytes before it call `target` and it lies in Scene.
    static bool scene_call(uintptr_t ret,uintptr_t target,uintptr_t scene,const std::array<uint8_t,5>& call){
        if(!ret || !target || !scene || ret<scene || ret-scene>0x4000 || call[0]!=0xe8)return false;
        int32_t rel{};std::memcpy(&rel,call.data()+1,4);return ret+int64_t(rel)==target;
    }
    static NativeOcclusionHarvestOnce& get(){static auto* p=new NativeOcclusionHarvestOnce;return *p;}
    bool install_in_game(uintptr_t base,const std::filesystem::path& path){
        constexpr game_build::Code expected=game_build::hashed(0x209fac0,16,"e83d15388f5f71ff5c0995d50c753902e03e272b3240210e2708dff37d120657");
        std::array<uint8_t,16> actual{};
        if(m_native || !path.is_absolute() || !game_build::rva(0x209fac0) || !game_build::rva(0x1805940) ||
           !ordinary_scene_scope::read(base+game_build::rva(0x209fac0),actual) || !game_build::code(0x209fac0,actual,expected))return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        auto* target=reinterpret_cast<void*>(base+game_build::rva(0x209fac0));
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        m_target=base+game_build::rva(0x209fac0);m_scene=base+game_build::rva(0x1805940);m_path=path;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_native=nullptr;return false;
    }
    bool installed()const{return m_native!=nullptr;}
    // calls, Scene-site calls, slot 0, slot 1, skipped; and the mode
    std::array<uint64_t,5> stats()const{std::lock_guard lock{m_mutex};return m_counts;}
    Mode mode()const{std::lock_guard lock{m_mutex};return m_mode;}
private:
    static uintptr_t on_call(uintptr_t owner){
        auto& self=get();const auto ret=reinterpret_cast<uintptr_t>(_ReturnAddress());bool skipped=false;
        try{
            std::lock_guard lock{self.m_mutex};++self.m_counts[0];
            const auto now=std::chrono::steady_clock::now();
            if(now>=self.m_next){self.m_next=now+std::chrono::seconds(1);std::ifstream file{self.m_path};std::string line;if(std::getline(file,line))if(const auto m=parse(line))self.m_mode=*m;}
            if(ret!=self.m_site){std::array<uint8_t,5> call{};if(ordinary_scene_scope::read(ret-5,call) && scene_call(ret,self.m_target,self.m_scene,call))self.m_site=ret;}
            if(ret==self.m_site){
                ++self.m_counts[1];
                std::optional<unsigned> slot;
                const auto scene=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();
                if(scene && pair){const ordinary_scene_scope::Scope scope{*scene,*pair};if(!ordinary_scene_scope::inspect(scope).mask && NativeCameraEntries::get().current(*pair))slot=scope.scene.slot;}
                if(slot && *slot<2)++self.m_counts[2+*slot];
                if(skip(self.m_mode,true,slot)){++self.m_counts[4];skipped=true;}
            }
        }catch(...){skipped=false;}
        return skipped?0:self.m_native(owner);
    }
    Native m_native{};uintptr_t m_target{},m_scene{},m_site{};std::filesystem::path m_path;
    std::chrono::steady_clock::time_point m_next{};Mode m_mode{Mode::native};
    mutable std::mutex m_mutex;std::array<uint64_t,5> m_counts{};
};
