#pragma once
#include "GameBuild.h"
#include <Windows.h>
#include <array>
#include <functional>
#include <span>
#include <atomic>
#include <optional>
#include "VrCameraPose.h"

// Explicit desktop experiment: select scene tonemapping for the owned PIP call.
class NativePipTonemap {
public:
    static constexpr uintptr_t RVA=0x1e90410;
    inline static constexpr game_build::Code BYTES=game_build::hashed(0x1e90410,16,"d01c01fd8935a72f21602837bef319409eb8a281658a18b55eabbb5e514cbe8e");
    static constexpr uintptr_t SCENE_RVA=0x1e92740;
    static constexpr uintptr_t OUTER_RVA=0x1fac000;
    inline static constexpr game_build::Code OUTER_BYTES=game_build::hashed(0x1fac000,16,"442a19659fe9096de3a84e01a789d0005ab6710232eaa0a6a807ab732a813780");
    inline static constexpr game_build::Code SCENE_BYTES=game_build::hashed(0x1e92740,16,"7c62cad323fc153a26dd1b48dc2dcc1f716cfa7a1d10b71a674930e56576819c");
    static NativePipTonemap& get() {static auto* value=new NativePipTonemap;return *value;}
    static bool matches(uint32_t,std::span<const uint8_t>);
    static bool owned(uintptr_t renderer,int32_t record,std::span<const vr_camera::Matrix> poses);
    static std::optional<int32_t> primary_index(uintptr_t list);
    using OuterGrading=std::array<uint8_t,21>;
    static std::optional<OuterGrading> outer_grading(uintptr_t pipeline,int32_t index,float scale,float minimum);
    bool install_in_game(HMODULE,std::function<bool()>,bool main_grading=false,bool scene_effects=false);
    bool install(void*,std::function<bool()>,void* scene=nullptr,std::function<std::optional<int32_t>()> primary={},void* outer=nullptr,std::function<bool(uintptr_t)> entry_owned={},std::function<std::optional<OuterGrading>(uintptr_t,int32_t)> outer_grading={});
    uint64_t applied() const {return m_applied;}
    uint64_t grading_applied() const {return m_grading_applied;}
    uint64_t effects_applied() const {return m_effects_applied;}
    uint64_t outer_grading_applied() const {return m_outer_grading_applied;}
private:
    using Core=void(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t,int32_t,const void*,uintptr_t);
    static void on_core(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t,int32_t,const void*,uintptr_t);
    using Outer=void(*)(uintptr_t,uintptr_t,float,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,const void*);
    static void on_outer(uintptr_t,uintptr_t,float,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,const void*);
    Outer m_outer{};std::atomic<uint64_t> m_effects_applied{};
    std::function<bool(uintptr_t)> m_entry_owned;
    inline static thread_local bool s_effects{};
    inline static thread_local std::optional<OuterGrading> s_outer_grading;
    std::function<std::optional<OuterGrading>(uintptr_t,int32_t)> m_outer_grading;
    std::atomic<uint64_t> m_outer_grading_applied{};
    using Scene=void(*)(uintptr_t,uintptr_t,uintptr_t,const void*,int32_t);
    static void on_scene(uintptr_t,uintptr_t,uintptr_t,const void*,int32_t);
    struct Grading {int32_t pip{},main{};std::optional<OuterGrading> outer;};
    inline static thread_local std::optional<Grading> s_grading;
    Scene m_scene{};std::function<std::optional<int32_t>()> m_primary;std::atomic<uint64_t> m_grading_applied{};
    Core m_original{};std::function<bool()> m_owned;std::atomic<uint64_t> m_applied{};
};
