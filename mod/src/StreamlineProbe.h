#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>
#include <deque>
#include <functional>
#include <sl_core_api.h>

// Passive by default. Opt-in diagnostic reset copies known V1/V2 main constants only;
// frame tokens, viewport handles, original game objects and API results stay unchanged.
// Kept alive for the process lifetime because render threads can still call the detour at exit.
class StreamlineProbe
{
public:
    enum class State { waiting, installed, missing_export, hook_failed };
    struct Camera
    {
        sl::float4x4 projection, inverse_projection, clip_to_previous, previous_to_clip;
        sl::float3 position, up, right, forward;
        sl::float2 jitter, motion_vector_scale;
        float near_plane{}, far_plane{}, fov{}, aspect{};
        sl::Boolean depth_inverted{}, reset{};
        uint32_t frame{}, viewport{};
        size_t constants_version{};
        sl::Result result{};
        std::chrono::steady_clock::time_point received{};
    };
    struct Snapshot
    {
        uint64_t calls{}, invalid{}, rejected{};
        std::optional<Camera> latest;
    };

    static StreamlineProbe& get();
    void poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    State state() const { return m_state.load(); }
    sl::Result submit_observed(const sl::Constants& values,const sl::FrameToken& frame,const sl::ViewportHandle& viewport){return state()==State::installed?on_constants(values,frame,viewport):sl::Result::eErrorMissingInputParameter;}
    Snapshot snapshot() const;
    std::optional<Camera> frame_camera(uint32_t token) const;
    void clear_frame_cache();
    void set_force_temporal_reset(bool enabled) { m_force_reset.store(enabled); }
    using ConstantsComplete=std::function<void(sl::Result)>;
    using ConstantsObserver=std::function<ConstantsComplete(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&)>;
    void set_constants_observer(ConstantsObserver observer){std::lock_guard lock{m_observer_mutex};m_observer=std::move(observer);}
    void start_trace(unsigned count);
    std::vector<Camera> take_trace();

private:
    std::atomic<bool> m_force_reset{};
    std::mutex m_observer_mutex;ConstantsObserver m_observer;
    StreamlineProbe() = default;
    static sl::Result on_constants(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
    void observe(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&, sl::Result);

    std::atomic<State> m_state{State::waiting};
    std::atomic<PFun_slSetConstants*> m_original{};
    std::mutex m_install_mutex;
    std::chrono::steady_clock::time_point m_next_poll{};
    mutable std::mutex m_snapshot_mutex;
    Snapshot m_snapshot;
    unsigned m_trace_remaining{};
    std::vector<Camera> m_trace;
    struct FrameCamera { Camera camera; bool ambiguous{}; };
    std::deque<FrameCamera> m_frames;
};
