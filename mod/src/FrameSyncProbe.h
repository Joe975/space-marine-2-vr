#pragma once

#include "FrameSyncClock.h"
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>
#include <sl.h>
#include <sl_pcl.h>

class FrameSyncProbe {
public:
    // Profile trace event per standard PCL marker (0..5), value = raw token.
    static constexpr std::array<const char*, 6> PCL_EVENTS{"pcl_simulation_start", "pcl_simulation_end",
        "pcl_render_submit_start", "pcl_render_submit_end", "pcl_present_start", "pcl_present_end"};
    enum class State { waiting, installed, missing_export, hook_failed };
    enum class Kind { marker, camera, constants, present, reset };
    struct Event {
        uint64_t sequence{};
        Kind kind{};
        uint32_t marker{}, token{}, thread{};
        sl::Result result{sl::Result::eOk};
        FrameSyncClock::Snapshot clock;
        std::chrono::steady_clock::time_point received{};
        bool camera_bound{};
    };
    struct Snapshot {
        uint64_t markers{}, rejected{};
        std::optional<Event> latest;
        FrameSyncClock::Snapshot clock;
    };
    static FrameSyncProbe& get();
    // Called after the D3D device exists. Never load or enable a Streamline feature ourselves.
    void poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    State state() const { return m_state.load(); }
    void record(Kind kind, uint32_t token = 0) noexcept;
    void start_trace(unsigned count);
    std::vector<Event> take_trace();
    Snapshot snapshot() const;
    std::optional<FrameSyncClock::Frame> current_camera_frame() const;
    std::optional<FrameSyncClock::Frame> render_camera_frame() const;
    bool recent_out_of_band(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;
    // A main camera job, before its frame is read: drives the clock when the game sends no markers (FrameSyncOwnClock).
    void camera_job() noexcept;
    bool own_clock() const;
private:
    static sl::Result on_marker(sl::PCLMarker, const sl::FrameToken&);
    static void trace_marker(uint32_t marker, uint32_t token);
    void append(Kind, uint32_t token, uint32_t marker = 0, sl::Result result = sl::Result::eOk);
    std::atomic<State> m_state{State::waiting};
    std::atomic<PFun_slPCLSetMarker*> m_original{};
    std::mutex m_install_mutex;
    std::chrono::steady_clock::time_point m_next_poll{};
    mutable std::mutex m_mutex;
    FrameSyncClock m_clock{FrameSyncClock::Policy::sm2_cached_start};
    FrameSyncOwnClock m_own;uint32_t m_own_token{};
    uint64_t m_sequence{};
    Snapshot m_snapshot;
    unsigned m_remaining{};
    std::vector<Event> m_trace;
    uint64_t m_epoch{};
    std::optional<std::chrono::steady_clock::time_point> m_last_out_of_band;
    static thread_local std::optional<FrameSyncClock::Frame> s_camera_frame;
    static thread_local uint64_t s_epoch;
};
