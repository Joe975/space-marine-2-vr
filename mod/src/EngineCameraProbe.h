#pragma once

#include "GameBuild.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>
#include <optional>
#include "VrCameraPose.h"
#include "VrViewMode.h"
#include "VrProjection.h"
#include "FrameSyncClock.h"
#include <deque>
#include <functional>

// Passive observer of the renderer's per-frame camera list. Every view the engine renders (main,
// shadows, PIP, ...) is appended through one function, "camList add" (see docs/ENGINE_CAMERA.md).
// The default mode forwards all eleven arguments and the result untouched. Optional control tests
// shift only the main submission, restoring the source after add. It requires the known exe build.
class EngineCameraProbe
{
public:
    enum class State { idle, installed, wrong_build, hook_failed };

    // Retail.exe, Steam build 25799387 (PE timestamp 0x6ac3e716).
    static constexpr uint32_t  BUILD_TIMESTAMP = 0x6ac3e716;
    static constexpr uintptr_t ADD_CAMERA_RVA  = 0x1d76b50;
    // Return address of the add call that submits the main view (inside function 0x1803230).
    static constexpr uintptr_t MAIN_VIEW_RETURN_RVA = 0x18033bf;
    static constexpr uintptr_t ADDITIONAL_VIEW_RETURN_RVA = 0x18049d5;
    static constexpr game_build::Code ADD_CAMERA_PROLOGUE=game_build::hashed(0x1d76b50,24,"6aedf633012d36897a083f0ad00c3396995eeb43dd1e37f5e63f64183fe4c83d");
    // Bytes of the source camera the engine itself reads in this function (through +0x387).
    static constexpr size_t SOURCE_CAMERA_SIZE = 0x388;

    struct Sample
    {
        uintptr_t caller_rva{}; // return address relative to the game module, 0 if outside it
        uintptr_t list{}, source{};
        int64_t   index{};          // value returned by the engine: slot in the camera list
        float     world[16]{};      // +0x000 camera-to-world, row major, position in row 3
        float     second[16]{};     // +0x040 world-to-view (inverse of the above)
        float     near_plane{}, far_plane{}; // +0x080, +0x084
        float     viewport[4]{};    // +0x12c x, y, width, height
        int32_t   kind{};           // +0x2cc
        float     fov_h_deg{}, fov_v_deg{}; // +0x2d0, +0x2d4
        uint64_t  args[9]{};        // arguments 3..11 as raw 64-bit slots
        uint64_t  calls{};          // calls seen from this caller
        uint64_t submission{};
        float applied_shift{};
        float center[3]{};
        bool vr_pose_applied{};
        int vr_eye{-1};
        uint64_t vr_render_token{};
        bool vr_projection_requested{};
        bool vr_projection_applied{};
        bool vr_synthetic{};
        vr_camera::Matrix runtime_head{vr_camera::IDENTITY};
        vr_camera::Matrix runtime_eye_to_head{vr_camera::IDENTITY};
        uint64_t tracking_epoch{};
        std::chrono::steady_clock::time_point pose_captured{};
        std::optional<vr_camera::Matrix> projection;
        std::optional<FrameSyncClock::Frame> game_frame;
        unsigned frame_submissions{};
        std::chrono::steady_clock::time_point received{};
    };
    struct Snapshot
    {
        uint64_t            calls{};
        std::vector<Sample> by_caller; // latest sample per distinct call site, in first-seen order
    };

    static EngineCameraProbe& get();
    static bool               matches_build(uint32_t pe_timestamp, std::span<const uint8_t> function_start);

    // Checks the build and hooks the game's function. Safe to call repeatedly; acts once.
    State install_in_game(HMODULE game);
    // Hooks an arbitrary function with the same calling convention (used by the native test).
    State install(void* target, uintptr_t module_base, size_t module_size);
    State state() const { return m_state.load(); }
    Snapshot snapshot() const;
    std::optional<Sample> rendered_frame(uint64_t render_id) const;
    std::optional<Sample> additional_frame(uint64_t render_id) const;
    std::optional<Sample> matching_camera(const vr_camera::Matrix& world, const vr_camera::Matrix& projection) const;

    // Control test: moves the main view sideways by this many world units along its own right
    // axis, for the duration of the add call only (the source camera is restored afterwards).
    // 0 keeps the hook fully passive. `main_view_return_rva` is settable for the native test.
    void  set_main_view_shift(float units) { m_main_shift.store(units); }
    float main_view_shift() const { return m_main_shift.load(); }
    void  set_main_view_return_rva(uintptr_t rva) { m_main_view_return_rva.store(rva); }
    void set_additional_view_return_rva(uintptr_t rva) {m_additional_view_return_rva.store(rva);}
    static void shift_camera(float* camera, float units);
    void set_alternating_shift(float units);
    float alternating_shift() const { return m_alternating_shift.load(); }
    void start_trace(unsigned count);
    std::vector<Sample> take_trace();

    struct VrEye {
        vr_camera::Matrix relative_eye{vr_camera::IDENTITY};
        float units_per_metre{1.0f};
        // How many times the eyes' scale exceeds the Marine's (the first-person scale and the body
        // scale, docs/BODY_SCALE.md). Tracked offsets use units_per_metre; lengths on the Marine
        // and in the game (body following's limits, grips, placement) use marine_units_per_metre().
        float fit_factor{1.0f};
        float marine_units_per_metre() const { return units_per_metre/fit_factor; }
        int eye{}; // 0 left, 1 right; token must identify the corresponding rendered image later
        uint64_t render_token{};
        std::chrono::steady_clock::time_point captured{};
        std::optional<vr_projection::Tangents> fov;
        bool synthetic{};
        vr_camera::Matrix runtime_head{vr_camera::IDENTITY};
        vr_camera::Matrix runtime_eye_to_head{vr_camera::IDENTITY};
        uint64_t tracking_epoch{};
        uint64_t reference_epoch{}; // Recenter/loss/calibration, distinct from snap image epochs.
        vr_camera::Matrix relative_head{vr_camera::IDENTITY};
        VrViewSettings view_settings;
        // Viewer mode: the free camera's level anchor (units_per_metre carries its scale).
        // Both eyes of one job carry the same value; camera_anchor uses it instead of the body.
        std::optional<vr_camera::Matrix> viewer_anchor;
        bool valid_at(std::chrono::steady_clock::time_point now) const {
            const auto age = now - captured;
            return eye >= 0 && eye <= 1 && age >= std::chrono::steady_clock::duration::zero() && age <= std::chrono::milliseconds(250) &&
                vr_camera::rigid(relative_eye) && view_settings.valid() && std::isfinite(units_per_metre) && units_per_metre >= 0.01f && units_per_metre <= 100.0f && (!fov || fov->valid());
        }
    };
    // Runtime adapter boundary. No provider is registered yet, so normal runs remain passive.
    // Invalid/stale/out-of-order samples fail closed. Clear on tracking loss or runtime shutdown.
    bool publish_vr_eye(const VrEye& eye);
    void clear_vr_eye();
    using EyeProvider = std::function<std::optional<VrEye>(const FrameSyncClock::Frame&)>;
    void set_eye_provider(EyeProvider provider);
    using AnchorProvider = std::function<vr_camera::Matrix(const vr_camera::Matrix&,const VrEye&)>;
    void set_anchor_provider(AnchorProvider provider);
    // Diagnostic observer, after the main source is restored and before the caller's
    // additional-view loop. The source and frame pointers are valid only for this call.
    using MainObserver = std::function<void(void*, const void*, const FrameSyncClock::Frame*)>;
    void set_main_observer(MainObserver observer);
    struct UiEye {
        uintptr_t source{};
        FrameSyncClock::Frame frame;
        VrEye eye;
        vr_camera::Matrix world;
        vr_camera::Matrix view;
        float near_z{};
        float far_z{};
        std::array<float,3> native_lens{};
        std::array<float,4> native_rectangle{};
    };
    // Published before native camera-list add: UI jobs run while that function is in flight.
    std::optional<UiEye> prepared_ui_eye(uint64_t render_id) const;
    std::optional<UiEye> prepare_ui_eye(uintptr_t source,const FrameSyncClock::Frame&);

private:
    using AddCameraFn = int64_t(void*, void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    static constexpr size_t MAX_CALLERS = 32;

    EngineCameraProbe() = default;
    static int64_t on_add_camera(void*, void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    void           observe(uintptr_t return_address, void* list, const void* source, int64_t index, const uint64_t (&args)[9], uint64_t submission, float shift, const float* center, const VrEye* vr_eye,
        const FrameSyncClock::Frame* game_frame, const std::optional<vr_camera::Matrix>& projection, bool projection_applied);

    std::atomic<State>        m_state{ State::idle };
    std::atomic<AddCameraFn*> m_original{};
    std::atomic<float>        m_main_shift{ 0.0f };
    std::atomic<float>        m_alternating_shift{ 0.0f };
    std::atomic<uint64_t>     m_main_submission{ 0 };
    std::atomic<uintptr_t>    m_main_view_return_rva{ MAIN_VIEW_RETURN_RVA };
    std::atomic<uintptr_t> m_additional_view_return_rva{ADDITIONAL_VIEW_RETURN_RVA};
    std::mutex                m_install_mutex;
    uintptr_t                 m_module_base{};
    size_t                    m_module_size{};
    mutable std::mutex        m_snapshot_mutex;
    Snapshot                  m_snapshot;
    unsigned                  m_trace_remaining{};
    std::vector<Sample>       m_trace;
    std::mutex m_vr_mutex;
    std::optional<VrEye> m_vr_eye;
    EyeProvider m_eye_provider;
    AnchorProvider m_anchor_provider;
    MainObserver m_main_observer;
    std::deque<Sample> m_render_samples;
    std::deque<Sample> m_additional_samples;
    std::deque<UiEye> m_ui_eyes;
    std::mutex m_preparation_mutex;
    std::atomic<uintptr_t> m_main_source{},m_native_inflight{};
};
