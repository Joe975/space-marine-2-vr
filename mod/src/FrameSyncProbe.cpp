#include "FrameSyncProbe.h"
#include "StreamlineModule.h"
#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <spdlog/spdlog.h>
#include "PerformanceTrace.h"

thread_local std::optional<FrameSyncClock::Frame> FrameSyncProbe::s_camera_frame;
thread_local uint64_t FrameSyncProbe::s_epoch{};

FrameSyncProbe& FrameSyncProbe::get() {
    static auto* instance = new FrameSyncProbe;
    return *instance;
}

void FrameSyncProbe::poll(std::chrono::steady_clock::time_point now) {
    std::lock_guard lock{m_install_mutex};
    if (state() != State::waiting || now < m_next_poll) { return; }
    m_next_poll = now + std::chrono::seconds(1);
    const auto module = streamline_module();
    if (!module) { return; }
    const auto resolver = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(module, "slGetFeatureFunction"));
    if (!resolver) { m_state = State::missing_export; return; }
    void* target{};
    // Feature APIs are resolved by name, not exported directly from sl.interposer.dll.
    // An unloaded/not-yet-ready feature is retryable; leave the game's feature configuration alone.
    if (resolver(sl::kFeaturePCL, "slPCLSetMarker", target) != sl::Result::eOk || !target) { return; }
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(target), &pinned)) { m_state = State::hook_failed; return; }
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) { m_state = State::hook_failed; return; }
    void* original{};
    status = MH_CreateHook(target, reinterpret_cast<void*>(&on_marker), &original);
    if (status == MH_OK) {
        m_original.store(reinterpret_cast<PFun_slPCLSetMarker*>(original));
        status = MH_EnableHook(target);
        if (status != MH_OK) { MH_RemoveHook(target); }
    }
    if (status != MH_OK) { m_state = State::hook_failed; spdlog::error("FrameSync: marker hook: {}", MH_StatusToString(status)); return; }
    m_state = State::installed;
    spdlog::info("FrameSync: hooked slPCLSetMarker at {:x} (passive, candidate clock only)", reinterpret_cast<uintptr_t>(target));
}

sl::Result FrameSyncProbe::on_marker(sl::PCLMarker marker, const sl::FrameToken& token) {
    auto& probe = get();
    const auto result = probe.m_original.load()(marker, token);
    try { probe.append(Kind::marker, static_cast<uint32_t>(token), static_cast<uint32_t>(marker), result); } catch (...) {}
    // Profiling: the engine's own frame phases (simulation, render submit, present) with raw tokens.
    // SM2's simulation start carries the previous token (FrameSyncClock sm2_cached_start).
    try { trace_marker(static_cast<uint32_t>(marker), static_cast<uint32_t>(token)); } catch (...) {}
    return result;
}

void FrameSyncProbe::trace_marker(uint32_t marker, uint32_t token) {
    auto& trace = PerformanceTrace::get();
    if (!trace.enabled() || marker >= PCL_EVENTS.size()) { return; }
    // SM2 starts each simulation frame in a job on a rotating worker (16 on 21 Oct) but ends it on
    // one game thread: that thread is the simulation role.
    if (marker == 1) { trace.thread_role(PerformanceTrace::Role::simulation); }
    trace.record(PCL_EVENTS[marker], PerformanceTrace::now_ms(), 0, double(token));
}

void FrameSyncProbe::record(Kind kind, uint32_t token) noexcept {
    try { append(kind, token); } catch (...) {}
}

void FrameSyncProbe::append(Kind kind, uint32_t token, uint32_t marker, sl::Result result) {
    std::lock_guard lock{m_mutex};
    if (kind == Kind::marker) {
        if (marker > 5) { m_last_out_of_band = std::chrono::steady_clock::now(); }
        ++m_snapshot.markers;
        if (result != sl::Result::eOk) { ++m_snapshot.rejected; }
        m_clock.marker(marker, token, result == sl::Result::eOk);
    }
    if (kind == Kind::reset) { m_clock.reset(); ++m_epoch; m_last_out_of_band.reset(); }
    Event event{++m_sequence, kind, marker, token, GetCurrentThreadId(), result, m_clock.snapshot(), std::chrono::steady_clock::now()};
    if (kind == Kind::marker && marker == 0) {
        s_camera_frame.reset();
        if (result == sl::Result::eOk && event.clock.simulation && event.clock.simulation->token == uint32_t(token+1)) {
            s_camera_frame = event.clock.simulation;
            s_epoch = m_epoch;
        }
    }
    if (kind == Kind::camera) {
        const auto frame = m_clock.camera_frame();
        if (frame) { event.token = frame->token; event.camera_bound = true; }
    }
    m_snapshot.latest = event;
    m_snapshot.clock = event.clock;
    if (m_remaining) { m_trace.push_back(event); --m_remaining; }
}

void FrameSyncProbe::start_trace(unsigned count) {
    std::lock_guard lock{m_mutex};
    m_trace.clear();
    m_remaining = std::min(count, 16384u);
    if (m_remaining) { spdlog::info("FrameSyncCapture: first_sequence={} count={}",m_sequence+1,m_remaining); }
}
std::vector<FrameSyncProbe::Event> FrameSyncProbe::take_trace() {
    std::lock_guard lock{m_mutex};
    std::vector<Event> events;
    events.swap(m_trace);
    return events;
}
FrameSyncProbe::Snapshot FrameSyncProbe::snapshot() const {
    std::lock_guard lock{m_mutex};
    return m_snapshot;
}
std::optional<FrameSyncClock::Frame> FrameSyncProbe::current_camera_frame() const {
    std::lock_guard lock{m_mutex};
    if (!s_camera_frame || s_epoch != m_epoch) { return std::nullopt; }
    const auto frame = m_clock.lookup(s_camera_frame->token);
    return frame && frame->render_id == s_camera_frame->render_id ? frame : std::nullopt;
}
std::optional<FrameSyncClock::Frame> FrameSyncProbe::render_camera_frame() const {
    std::lock_guard lock{m_mutex};
    return m_clock.camera_frame();
}
void FrameSyncProbe::camera_job() noexcept {
    try {
        std::lock_guard lock{m_mutex};
        const auto step=m_own.job(m_snapshot.markers);
        if(step==FrameSyncOwnClock::Step::started)spdlog::warn("FrameSync: no PCL markers in {} camera jobs: numbering the jobs here",FrameSyncOwnClock::AFTER_JOBS);
        if(step==FrameSyncOwnClock::Step::stopped){spdlog::info("FrameSync: PCL markers arrived: back on them");m_clock.reset();++m_epoch;m_own_token=0;}
        if(step!=FrameSyncOwnClock::Step::advance && step!=FrameSyncOwnClock::Step::started)return;
        // One open render-submit interval, the job's, as the game's markers make it (sm2_cached_start: phase 0 carries the previous token).
        if(m_own_token){m_clock.marker(3,m_own_token);m_clock.marker(4,m_own_token);m_clock.marker(5,m_own_token);}
        m_clock.marker(0,m_own_token);m_clock.marker(2,++m_own_token);
        m_snapshot.clock=m_clock.snapshot();
    } catch (...) {}
}
bool FrameSyncProbe::own_clock() const {
    std::lock_guard lock{m_mutex};
    return m_own.active();
}
bool FrameSyncProbe::recent_out_of_band(std::chrono::steady_clock::time_point now) const {
    std::lock_guard lock{m_mutex};
    return m_last_out_of_band && now >= *m_last_out_of_band && now-*m_last_out_of_band <= std::chrono::seconds(1);
}
