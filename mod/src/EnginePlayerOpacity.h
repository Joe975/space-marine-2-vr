#pragma once
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include "DistanceFadeCache.h"
#include <string_view>

// Local-player scalar opacity and selectable player/all-object distance fading.
// Cached GPU data is rebuilt only through current native mesh calls.
class EnginePlayerOpacity {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr uintptr_t SETTER_RVA=0x1343f10, GETTER_RVA=0x1d95680, DRAW_RVA=0x1d8b0f0;
    struct Snapshot { uint64_t calls{},owned{},overrides{},released{},invalid{},reads{},owned_reads{},read_overrides{},native_faded_reads{},changed_reads{}; uintptr_t actor{},instance{}; float native{},effective{},render_native{},render_effective{},render_min_native{1}; bool enabled{},allowed{},getter_installed{}; };
    static EnginePlayerOpacity& get();
    static bool matches_build(uint32_t,std::span<const uint8_t>);
    static bool matches_getter(uint32_t,std::span<const uint8_t>);
    static bool matches_draw(uint32_t,std::span<const uint8_t>);
    static constexpr uintptr_t PARAMS_RVA=0x20bd700;
    static bool matches_params(uint32_t,std::span<const uint8_t>);
    static constexpr uintptr_t REFRESH_RVA=0x1d7fa70;
    static bool matches_refresh(uint32_t,std::span<const uint8_t>);
    // opacity=false: only the distance-fade pair (params, refresh). The setter, getter and draw
    // hooks only act with SM2VR_PLAYER_OPAQUE=1, yet cost a counter per getter call (7,800/s) and
    // about six ReadProcessMemory calls per player draw (release tidy review, 26 Oct).
    bool install_in_game(HMODULE,bool opacity=true);
    // The setter may be null when params is given (distance fade only).
    bool install(void*,std::function<uintptr_t(Clock::time_point)>,void* getter=nullptr,void* draw=nullptr,void* params=nullptr,void* refresh=nullptr);
    bool opacity_installed() const { return m_target!=nullptr; }
    static constexpr size_t DISTANCE_CACHE_LIMIT=65536;
    struct ParamsSnapshot {uint64_t owned{},changed{},failures{},modes[4]{},builds{},invalid{},capacity_skips{},tracked{},swept{},evicted{};uint32_t flags{};float scale{},offset{},padding{};bool installed{},enabled{},all_objects{};};
    ParamsSnapshot params_snapshot() const;
    void keep_distance_opaque(bool enabled);
    bool distance_control(std::string_view);
    struct DrawSnapshot {uint64_t owned{},bypassed{},failures{};bool installed{};};
    DrawSnapshot draw_snapshot() const;
    void enable(bool);
    void allow(bool);
    bool installed() const { return m_installed; }
    void uninstall(); // Quiescent teardown; subsequent native calls always receive original values.
    Snapshot snapshot() const;
private:
    using Setter=void(*)(void*,float); // Verified XMM1 -> MOVSS native ABI.
    static void hook(void*,float);
    using Getter=float(*)(void*);
    static float getter_hook(void*);
    float select(void*,float);
    float select_instance(void*,float);
    using Draw=void(*)(void*,uint64_t,uint32_t,uint32_t,void*,uint8_t*,uint8_t*,uint8_t*,uint8_t*,uint8_t*,uint8_t*,uint8_t*);
    static void draw_hook(void*,uint64_t,uint32_t,uint32_t,void*,uint8_t*,uint8_t*,uint8_t*,uint8_t*,uint8_t*,uint8_t*,uint8_t*);
    bool owns_draw(uint64_t) const;
    using Params=void(*)(uint32_t*,uint64_t,void*,void*,void*);
    static void params_hook(uint32_t*,uint64_t,void*,void*,void*);
    Params m_params{};ParamsSnapshot m_params_state;
    void* m_params_target{};
    using Refresh=void(*)(void*,uint64_t);
    static void refresh_hook(void*,uint64_t);
    Refresh m_refresh{};void* m_refresh_target{};
    // Evaluated native caches: modified, or rejected parameters (modified=false).
    // Rejected caches stay native until their next natural rebuild, avoiding a
    // rebuild on every draw. No saved pointer is dereferenced for restoration.
    // Full: stale entries go, and the player's own caches always get in (DistanceFadeCache.h).
    DistanceFadeCache m_distance_cache{DISTANCE_CACHE_LIMIT};
    mutable std::shared_mutex m_mutex; // Shared only for the per-draw touch in refresh_hook.
    Snapshot m_state;
    std::function<uintptr_t(Clock::time_point)> m_actor;
    Setter m_original{};
    Getter m_getter{};
    Draw m_draw{};DrawSnapshot m_draw_state;
    void* m_target{},*m_getter_target{},*m_draw_target{};
    uintptr_t m_game_base{},m_draw_caller{0x1e78931};
    std::atomic<bool> m_installed{};
    std::atomic<uintptr_t> m_candidate{}; // Read-only fast rejection; owned calls still revalidate.
    std::atomic<uint64_t> m_reads{};
};
