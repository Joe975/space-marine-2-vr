#pragma once
#include "GameBuild.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include "VrCameraPose.h"
#include "NativeCameraEntries.h"
#include "RenderTargetProbe.h"
#include "NativeCameraPacket.h"

// Opt-in CPU dispatcher observer. A completed dispatch is NOT a GPU fence.
class NativeRenderProbe {
public:
    static constexpr uint32_t TIMESTAMP=0x6ac3e716;
    static constexpr uintptr_t DISPATCH_RVA=0x1804a70,LIST_RVA=0x4f7c6c8;
    static constexpr uintptr_t SCENE_RVA=0x1805940;
    static constexpr size_t MAX_SCENES=64;
    inline static constexpr game_build::Code SCENE_PROLOGUE=game_build::hashed(0x1805940,21,"80100267480dbae03bc6ac513cec78abd43bb484d1dee4a18a5d95d62ef85850");
    inline static constexpr game_build::Code PROLOGUE=game_build::hashed(0x1804a70,18,"0885fb5cfb0ae59c6be1bcad1b58e8243240372d0ca721887f0574e8583a98c8");
    struct View {
        int32_t slot{},record{-1}; uint64_t flags{};
        vr_camera::Matrix world{},view{},projection{},inverse_projection{},combined{};
        std::optional<NativeCameraPacket> packet;
        std::array<float,4> rectangle{};
        std::array<float,8> corners{};
        std::array<float,24> planes{};
        float cull_radius{};
        std::array<float,12> cull_directions{},cull_sides{};
        std::array<float,3> cull_normal{},cull_origin{};
        float cull_distance{};
        float fov_h{},fov_v{},aspect{};
        bool pip() const {return (flags&0x40)!=0;}
        bool main() const {return (flags&0x50000044)==0;}
    };
    enum class SceneRefusal:uint32_t {none,unreadable,slot,list,entry,render_counter,revision,view,renderer};
    enum class OutputStatus:uint32_t {unavailable,unreadable,unobserved,changed,overflow,observed};
    struct Output {
        uintptr_t resource{};
        OutputStatus status{OutputStatus::unavailable};
        RenderTargetProbe::Observation descriptors;
    };
    using OutputObserver=std::function<RenderTargetProbe::Observation(uintptr_t)>;
    struct Scene {
        std::optional<NativeCameraPacket> packet_before,packet_after;
        struct Temporal {std::array<uintptr_t,11> previous{},next{};};
        std::optional<Temporal> temporal_before,temporal_after;
        struct PersistentBank {uintptr_t address{};std::array<uintptr_t,11> words{};bool operator==(const PersistentBank&)const=default;};
        // Borrowed POD of the dispatcher's per-slot storage, not a resource lease.
        std::optional<PersistentBank> persistent_before,persistent_after;
        uint64_t ordinal{},dispatch_sequence{};
        uint32_t flags{},slot{},render_counter_before{},render_counter_after{};
        uintptr_t renderer{},list{},entry{},active_entry_after{},output_before{},output_after{};
        uint64_t view_flags_before{},view_flags_after{};
        std::optional<uint64_t> camera_revision_before,camera_revision_after;
        SceneRefusal refusal{SceneRefusal::unreadable};
        Output resource_before,resource_after;
        bool valid() const {return refusal==SceneRefusal::none;}
    };
    struct Snapshot {
        uint64_t sequence{};uintptr_t list{},renderer{};uint32_t flags{};
        std::chrono::steady_clock::time_point begin{},end{};
        std::vector<View> before,after;
        bool scene_observer{};
        uint64_t scene_calls{},scene_refused{},scene_dropped{};
        std::vector<Scene> scenes;
        std::optional<uint64_t> camera_revision;
        std::optional<NativeCameraEntries::Pair> camera_entries;
        std::optional<NativeCameraEntries::Primary> primary;
        std::optional<NativeCameraEntries::OrdinaryPair> ordinary_pair;
    };
    static NativeRenderProbe& get();
    static uintptr_t current_renderer() {return s_current_renderer;}
    static std::optional<Scene> current_scene();
    static std::optional<NativeCameraEntries::OrdinaryPair> current_ordinary_pair() {
        if(!s_current_snapshot || !s_current_snapshot->ordinary_pair || !NativeCameraEntries::get().current(*s_current_snapshot->ordinary_pair))return std::nullopt;
        return s_current_snapshot->ordinary_pair;
    }
    static std::optional<NativeCameraEntries::Primary> current_primary() {
        return s_current_primary && NativeCameraEntries::get().current(*s_current_primary)?s_current_primary:std::nullopt;
    }
    static std::optional<NativeCameraEntries::Pair> current_camera_entries() {
        return s_current_entries && NativeCameraEntries::get().current(*s_current_entries)?s_current_entries:std::nullopt;
    }
    static bool matches(uint32_t timestamp,std::span<const uint8_t> bytes);
    static bool matches_scene(uint32_t timestamp,std::span<const uint8_t> bytes);
    bool install_in_game(HMODULE module,std::function<bool()> enabled,OutputObserver outputs={});
    bool install(void* target,std::function<uintptr_t()> list,std::function<bool()> enabled,OutputObserver outputs={});
    // NativePipLens owns the sole entry detour and calls observe_scene after preparation.
    using SceneRender=void(*)(void*,uint32_t,int32_t);
    bool attach_scene_hook(void* target);
    bool scene_installed() const {return m_scene_target.load()!=0;}
    static void observe_scene(void*,uint32_t,int32_t,SceneRender);
    std::optional<Snapshot> latest() const;
    // Runs after the native camera/SDK transaction has completed, outside its lock.
    bool attach_completion(std::function<void(const Snapshot&)> callback){std::lock_guard lock{m_install};if(m_complete || !callback)return false;m_complete=std::move(callback);return true;}
    static std::optional<std::vector<View>> read_views(uintptr_t list);
    // A PIP behind the owned pair gets the HDR index it has in mono, before the dispatch reads it
    // (ForeignViewHold::hdr_remap, docs/LOAD_CRASH_2026-10-26.md). True when it wrote one.
    static bool remap_pip_hdr(uintptr_t list);
    static Output read_output(uintptr_t wrapper,const OutputObserver&);
    static std::optional<Scene::PersistentBank> read_history_bank(uintptr_t renderer,uint32_t ordinary_slot);
private:
    using Dispatch=void(*)(void*,uint32_t);
    static void on_dispatch(void*,uint32_t);
    std::atomic<Dispatch> m_original{};std::function<uintptr_t()> m_list;std::function<bool()> m_enabled;
    std::atomic<uintptr_t> m_scene_target{};
    OutputObserver m_outputs;
    std::function<void(const Snapshot&)> m_complete;
    std::mutex m_install;mutable std::mutex m_mutex;
    std::optional<Snapshot> m_latest;std::atomic<uint64_t> m_sequence{};
    inline static thread_local uintptr_t s_current_renderer{};
    inline static thread_local std::optional<NativeCameraEntries::Pair> s_current_entries;
    inline static thread_local Snapshot* s_current_snapshot{};
    inline static thread_local std::optional<size_t> s_current_scene;
    inline static thread_local std::optional<NativeCameraEntries::Primary> s_current_primary;
};
