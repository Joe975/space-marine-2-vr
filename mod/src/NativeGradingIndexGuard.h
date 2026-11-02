#pragma once
#include "ForeignViewHold.h"
#include "GameBuild.h"
#include "NativeOrdinarySceneScope.h"
#include <MinHook.h>
#include <array>
#include <spdlog/spdlog.h>

// The game's colour grading tables hold two entries, main 0 and PIP 1, and the per-view scene (0x1805940)
// passes its render-list slot straight to FUN_141e8d1c0, which finishes that entry's pending table. A
// preview behind the owned pair is slot 2, so entry 2 lands on the texture array after the entries, and
// the call runs a grading dispatch on index 1's textures and then clears +0xe0, one of them. Nothing shows
// until the rotation reaches the empty texture, which photo mode's grading change does: the null at
// Retail.exe+0x1e8eddb. Refused here: the preview's pending table waits for the next frame, when the hold
// has put it in slot 1. docs/LOAD_CRASH_2026-10-26.md, photo mode.
class NativeGradingIndexGuard {
public:
    using Native=void(*)(uintptr_t,int32_t);
    static NativeGradingIndexGuard& get(){static auto* value=new NativeGradingIndexGuard;return *value;}
    bool install_in_game(uintptr_t base){
        constexpr game_build::Code expected=game_build::hashed(0x1e8d1c0,16,"99c6aeb8d7085cf571bc5cd6d3d94a8eb5f13e2be19b93a28654c7d1ac1c35f4");
        const auto target=game_build::rva(0x1e8d1c0);std::array<uint8_t,16> bytes{};
        if(m_native || !target || !ordinary_scene_scope::read(base+target,bytes) || !game_build::code(0x1e8d1c0,bytes,expected))return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(reinterpret_cast<void*>(base+target),reinterpret_cast<void*>(&on_finish),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        if(MH_EnableHook(reinterpret_cast<void*>(base+target))==MH_OK)return true;
        MH_RemoveHook(reinterpret_cast<void*>(base+target));m_native=nullptr;return false;
    }
private:
    static void on_finish(uintptr_t table,int32_t slot){
        auto& hold=ForeignViewHold::get();
        if(hold.admit_grading(slot)){get().m_native(table,slot);return;}
        if(hold.stats().grading_refused<=8)spdlog::warn("GradingIndexGuard: refused slot={} (past the game's two grading entries)",slot);
    }
    Native m_native{};
};
