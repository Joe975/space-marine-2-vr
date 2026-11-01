#pragma once
#include "GameBuild.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include "PipLensGeometry.h"
#include "PipPoseEvidence.h"
#include "NativeCameraEntries.h"

// Owned-PIP lens fix at the renderer entry, after the native reset (desktop only). Headset launches
// install it with an empty lens provider: its Scene entry detour is the one other ordinary modules use.
class NativePipLens {
public:
    static constexpr uint32_t TIMESTAMP=0x6ac3e716;
    static constexpr uintptr_t RENDER_RVA=0x1805940,CACHE_RVA=0x1d75d30,FRUSTUM_RVA=0x2224490,LIST_RVA=0x4f7c6c8;
    inline static constexpr game_build::Code RENDER_BYTES=game_build::hashed(0x1805940,13,"a6a568f53b0441f1dada5118c0cc67a8cfbe0b1d3fafdb84d20131c0b8e013b5");
    // Other helper prologues are verified against the shipping image before any hook.
    struct Target {
        int32_t record{-1};vr_camera::Matrix world{};float horizontal{},vertical{};uint32_t phase{};uint64_t updates{};
        std::vector<vr_camera::Matrix> recent_worlds;
        std::vector<PipPoseEvidence> history;bool current_eligible{true};double lock_wait_ms{};
        std::optional<NativeCameraEntries::Pair> camera_entries;
    };
    struct Rejection {
        uint64_t serial{},updates{};int reason{};uint32_t phase{},thread{};int32_t slot{},record{};
        size_t candidates{};float current_pose_error{},nearest_pose_error{};
        vr_camera::Matrix world{},target_world{};
        std::vector<PipPoseEvidence> history;bool current_eligible{};double lock_wait_ms{};
    };
    struct Skip {
        uint64_t serial{};int reason{};uint32_t thread{};int32_t slot{},record{};vr_camera::Matrix world{};
    };
    struct Sample {
        uint64_t applied{},rejected{},historical_matches{};int reason{};uint32_t phase{};int32_t slot{-1};uint64_t updates{};
        pip_lens::Geometry geometry;std::array<float,24> planes{};
        std::chrono::steady_clock::time_point received{};
        std::array<uint64_t,13> reason_counts{};
        std::vector<Rejection> rejection_events; // Last 64; cumulative counters survive ring eviction and successes.
        uint64_t skipped{},unexpected_errors{};
        std::array<uint64_t,4> skip_counts{};
        std::vector<Skip> skip_events; // PIP candidates only; not a claim of active ownership.
    };
    using Cache=void(*)(void*,const float*,const float*,const float*);
    using Frustum=void(*)(void*);
    static NativePipLens& get();
    static bool matches(uint32_t,std::span<const uint8_t>,std::span<const uint8_t>,std::span<const uint8_t>);
    bool install_in_game(HMODULE,std::function<std::optional<Target>()>);
    bool install(void*,Cache,Frustum,std::function<uintptr_t()>,std::function<std::optional<Target>()>);
    Sample snapshot() const;
private:
    using Render=void(*)(void*,uint32_t,int32_t);
    static void on_render(void*,uint32_t,int32_t);
    void prepare(int32_t);
    void publish(Sample,bool historical,std::optional<Rejection> = {});
    void skip(int reason,int32_t slot,int32_t record,const vr_camera::Matrix&);
    std::atomic<Render> m_original{};Cache m_cache{};Frustum m_frustum{};
    std::function<uintptr_t()> m_list;std::function<std::optional<Target>()> m_target;
    std::mutex m_install;mutable std::mutex m_mutex;Sample m_sample;
};
