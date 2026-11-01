#include "StreamlineProbe.h"
#include "StreamlineModule.h"
#include "FrameSyncProbe.h"

#include <Windows.h>
#include <MinHook.h>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

namespace
{
    bool valid(float value) { return std::isfinite(value) && value != sl::INVALID_FLOAT; }
    bool valid(const sl::float3& v) { return valid(v.x) && valid(v.y) && valid(v.z); }
    bool valid(const sl::float4x4& matrix)
    {
        for (const auto& row : matrix.row) {
            if (!valid(row.x) || !valid(row.y) || !valid(row.z) || !valid(row.w)) { return false; }
        }
        return true;
    }
}

StreamlineProbe& StreamlineProbe::get()
{
    static auto* instance = new StreamlineProbe;
    return *instance;
}

void StreamlineProbe::poll(std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock{m_install_mutex};
    if (state() != State::waiting || now < m_next_poll) { return; }
    m_next_poll = now + std::chrono::seconds{1};

    // Never load Streamline ourselves, or wait for it on the Present thread.
    const auto module = streamline_module();
    if (!module) { return; }
    const auto target = GetProcAddress(module, "slSetConstants");
    if (!target) {
        m_state = State::missing_export;
        spdlog::error("StreamlineProbe: sl.interposer.dll has no slSetConstants export");
        return;
    }
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(target), &pinned)) {
        m_state = State::hook_failed;
        spdlog::error("StreamlineProbe: could not pin interposer module ({})", GetLastError());
        return;
    }
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        m_state = State::hook_failed;
        spdlog::error("StreamlineProbe: MinHook init: {}", MH_StatusToString(status));
        return;
    }
    void* original{};
    status = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&on_constants), &original);
    if (status == MH_OK) {
        // Publish the trampoline BEFORE enabling: another render thread can call immediately.
        m_original.store(reinterpret_cast<PFun_slSetConstants*>(original));
        status = MH_EnableHook(reinterpret_cast<void*>(target));
        if (status != MH_OK) { MH_RemoveHook(reinterpret_cast<void*>(target)); }
    }
    if (status != MH_OK) {
        m_state = State::hook_failed;
        spdlog::error("StreamlineProbe: slSetConstants hook: {}", MH_StatusToString(status));
        return;
    }
    m_state = State::installed;
    spdlog::info("StreamlineProbe: hooked slSetConstants at {:x} (passive camera capture)", reinterpret_cast<uintptr_t>(target));
}

sl::Result StreamlineProbe::on_constants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport)
{
    auto& probe = get();
    sl::Constants reset_values;
    const auto* submitted = &values;
    if (probe.m_force_reset.load() && static_cast<uint32_t>(viewport) == 0 && values.structType == sl::Constants::s_structType &&
        (values.structVersion == sl::kStructVersion1 || values.structVersion == sl::kStructVersion2)) {
        const auto prefix = reinterpret_cast<const uint8_t*>(&reset_values.minRelativeLinearDepthObjectSeparation)-reinterpret_cast<const uint8_t*>(&reset_values);
        std::memcpy(&reset_values,&values,values.structVersion == sl::kStructVersion1 ? prefix : sizeof(reset_values));
        reset_values.reset = sl::eTrue;
        submitted = &reset_values;
    }
    ConstantsComplete complete;
    try {ConstantsObserver observer;{std::lock_guard lock{probe.m_observer_mutex};observer=probe.m_observer;}if(observer)complete=observer(*submitted,frame,viewport);}catch(...){}
    const auto result = probe.m_original.load()(*submitted, frame, viewport);
    try {if(complete)complete(result);}catch(...){}
    // Diagnostic failures must never affect the application's result or unwind into the game.
    try {
        probe.observe(*submitted, frame, viewport, result);
        if (static_cast<uint32_t>(viewport) == 0) { FrameSyncProbe::get().record(FrameSyncProbe::Kind::constants, static_cast<uint32_t>(frame)); }
    } catch (...) {}
    return result;
}

void StreamlineProbe::observe(const sl::Constants& v, const sl::FrameToken& frame, const sl::ViewportHandle& viewport, sl::Result result)
{
    std::lock_guard lock{m_snapshot_mutex};
    ++m_snapshot.calls;
    const auto token = static_cast<uint32_t>(frame);
    const auto is_main = static_cast<uint32_t>(viewport) == 0;
    auto cached = std::ranges::find_if(m_frames,[&](const auto& entry) { return entry.camera.frame == token; });
    if (is_main && cached != m_frames.end() && result != sl::Result::eOk) { cached->ambiguous = true; }
    if (result != sl::Result::eOk) { ++m_snapshot.rejected; }
    // Read only the V1 prefix; never copy the struct's next pointer or V2-only trailing fields.
    if (v.structType != sl::Constants::s_structType || v.structVersion < sl::kStructVersion1 ||
        !valid(v.cameraViewToClip) || !valid(v.clipToCameraView) || !valid(v.clipToPrevClip) || !valid(v.prevClipToClip) ||
        !valid(v.cameraPos) || !valid(v.cameraUp) || !valid(v.cameraRight) || !valid(v.cameraFwd) ||
        !valid(v.jitterOffset.x) || !valid(v.jitterOffset.y) || !valid(v.mvecScale.x) || !valid(v.mvecScale.y) ||
        !valid(v.cameraNear) || !valid(v.cameraFar) || !valid(v.cameraFOV) || !valid(v.cameraAspectRatio)) {
        ++m_snapshot.invalid;
        if (is_main && cached != m_frames.end()) { cached->ambiguous = true; }
        return;
    }
    m_snapshot.latest = Camera{v.cameraViewToClip, v.clipToCameraView, v.clipToPrevClip, v.prevClipToClip,
        v.cameraPos, v.cameraUp, v.cameraRight, v.cameraFwd, v.jitterOffset, v.mvecScale,
        v.cameraNear, v.cameraFar, v.cameraFOV, v.cameraAspectRatio, v.depthInverted, v.reset,
        static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport), v.structVersion, result, std::chrono::steady_clock::now()};
    if (is_main && result == sl::Result::eOk) {
        const auto& camera = *m_snapshot.latest;
        if (cached == m_frames.end()) {
            if (m_frames.size() == 8) { m_frames.pop_front(); }
            m_frames.push_back({camera,false});
        } else {
            const auto different = [](float a,float b) { return std::abs(a-b) > 1e-4f; };
            const auto& previous = cached->camera;
            bool changed = different(previous.position.x,camera.position.x) || different(previous.position.y,camera.position.y) || different(previous.position.z,camera.position.z);
            for (unsigned row = 0; row < 4; ++row) for (unsigned col = 0; col < 4; ++col) {
                changed |= different(reinterpret_cast<const float*>(&previous.projection[row])[col],reinterpret_cast<const float*>(&camera.projection[row])[col]);
            }
            for (const auto pair : {std::pair{previous.right,camera.right},std::pair{previous.up,camera.up},std::pair{previous.forward,camera.forward}}) {
                changed |= different(pair.first.x,pair.second.x) || different(pair.first.y,pair.second.y) || different(pair.first.z,pair.second.z);
            }
            cached->ambiguous |= changed;
            cached->camera = camera;
        }
    }
    if (m_trace_remaining && static_cast<uint32_t>(viewport) == 0) {
        m_trace.push_back(*m_snapshot.latest);
        --m_trace_remaining;
    }
}

void StreamlineProbe::start_trace(unsigned count)
{
    std::lock_guard lock{m_snapshot_mutex};
    m_trace.clear();
    m_trace_remaining = std::min(count, 8192u);
}

std::vector<StreamlineProbe::Camera> StreamlineProbe::take_trace()
{
    std::lock_guard lock{m_snapshot_mutex};
    std::vector<Camera> result;
    result.swap(m_trace);
    return result;
}

StreamlineProbe::Snapshot StreamlineProbe::snapshot() const
{
    std::lock_guard lock{m_snapshot_mutex};
    return m_snapshot;
}
std::optional<StreamlineProbe::Camera> StreamlineProbe::frame_camera(uint32_t token) const {
    std::lock_guard lock{m_snapshot_mutex};
    for (const auto& entry : m_frames) { if (entry.camera.frame == token && !entry.ambiguous) { return entry.camera; } }
    return std::nullopt;
}
void StreamlineProbe::clear_frame_cache() { std::lock_guard lock{m_snapshot_mutex}; m_frames.clear(); }
