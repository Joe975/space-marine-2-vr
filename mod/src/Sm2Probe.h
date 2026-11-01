#pragma once
#include "OwnedStereoReadiness.h"
#include "OrdinaryStereoRecovery.h"
#include "SceneMode.h"
#include "NativeCallCensus.h"
#include <Mod.hpp>
#include "EyeTextureCapture.h"
#include "DesktopWindow.h"
#include "VrSession.h"
#include "EyeDumpRequest.h"
#include "RenderTargetProbe.h"
#include "GpuCompletionProbe.h"
#include "NativeOrdinaryProbe.h"
#include "GpuCompositorOutput.h"
#include "GpuPresentMirror.h"
#include "NativeOrdinaryCamera.h"
#include "GpuOrdinaryPairRetention.h"
#include "FileStamp.h"
#include "OrdinaryRuntimeImages.h"
#include "OrdinaryEyeSnapshot.h"
#include "PresentHandoff.h"
#include "StereoHoldControl.h"
#include "NativeAsyncLightingControl.h"
#include "FarLightCacheControl.h"
#include "LightCapSortControl.h"
#include "UiCoverageProbe.h"
#include "VrThumbsUp.h"
#include <mutex>

// Injection/Present/overlay probe plus passive Streamline camera diagnostics.
class Sm2Probe : public Mod
{
public:
    static std::shared_ptr<Sm2Probe>& get()
    {
        static auto instance{ std::make_shared<Sm2Probe>() };
        return instance;
    }

    [[nodiscard]] std::string_view get_name() const override { return "Sm2Probe"; }

    std::optional<std::string> on_initialize() override;
    std::optional<std::string> on_initialize_d3d_thread() override;
    void                       on_present() override;
    void                       on_post_frame() override;
    void                       on_post_present() override;
    void                       on_draw_ui() override;
    void                       on_device_reset() override;
    bool                       on_message(HWND,UINT,WPARAM,LPARAM) override;

private:
    void log_swap_chain(const char* reason);
    void rearm_ordinary_headset();
    void cold_restart_ordinary_headset();
    void report_ordinary_dlss();
    void guard_readiness_stall();
    ReadinessStall m_readiness_stall; // Present thread: player-state readiness stalls in plain play.
    void log_scene(const scene_mode::Scene&);
    void log_call_census();
    std::string m_call_census_flags{"|"};uint64_t m_call_census_lines{};
    scene_mode::Watch m_scene_watch; // Present thread: SceneMode lines on mode changes and first-seen masks.
    void apply_desktop_window();
    void apply_scripted_camera_shift();
    void log_temporal_trace();
    void log_frame_trace();
    void capture_eye_image();
    void capture_pip_experiment();
    void capture_ordinary_observation();
    bool m_ordinary_second_requested{};
    bool m_ordinary_headset{};
    // SM2VR_TRACE=1: the 5 s status also lists every tracked object (arm bones, tracker words,
    // weapon targets, extra Streamline matrices, marker cameras, hand poses). Counters always log.
    bool m_status_detail{};
    uint64_t m_logged_world_refusals{}; // ArmWorldRefused: the arm probe's refusal count last logged.
    bool m_screen_ui_requested{}; // eye snapshots (SM2VR_EYE_DUMP_DIR) or SM2VR_DIAG_SCREEN_UI=1
    OrdinaryRuntimeImages m_ordinary_runtime_images;
    std::array<EyeTextureCapture,2> m_menu_copies;uint64_t m_menu_render_id{},m_menu_submitted_id{},m_menu_frames{};
    std::chrono::steady_clock::time_point m_last_ordinary_submit{};
    SubmitGaps m_submit_gaps; // time between pairs submitted to the runtime (OrdinaryStereoRecovery.h)
    uint64_t m_ordinary_submitted{},m_ordinary_runtime_dropped{},m_ordinary_consumed{};
    unsigned m_ordinary_headset_dumps{};
    bool m_ordinary_headset_failure_dumped{};
    // Recoverable stereo suspension: logged episode, dropped pairs, flat-panel fallback state.
    uint64_t m_ordinary_suspension_logged{},m_ordinary_rebases_logged{},m_ordinary_renewals_logged{},m_ordinary_suspended_pairs{},m_flat_fallback_frames{},m_flat_waiting_episodes{};uint32_t m_flat_fallback{},m_ordinary_recovery_flags{};std::string m_ordinary_recovery_signature;std::chrono::steady_clock::time_point m_ordinary_recovery_logged{};
    bool m_ordinary_perf{};
    std::filesystem::path m_ordinary_perf_control;
    std::chrono::steady_clock::time_point m_ordinary_perf_poll{};
    bool m_ordinary_fault_pending{};
    unsigned m_stereo_warmup{241};
    std::vector<uint64_t> m_ordinary_second_dumped;
    void ordinary_boundary(const NativeOrdinaryProbe::Event&);
    void ordinary_pair_boundary(const NativeOrdinaryPairColour::Event*);
    void present_menu_only(ID3D12CommandQueue*);
    // Full-screen menus over a rendered world (barge Datavault, mission select): VrScreenMenu.h.
    void sample_ui_coverage(ID3D12CommandQueue*);
    UiCoverageProbe m_ui_coverage;VrScreenMenuDetector m_screen_menu;
    std::chrono::steady_clock::time_point m_next_ui_coverage{},m_next_ui_coverage_log{};uint64_t m_ui_coverage_failures{},m_ui_coverage_samples{};int m_ui_coverage_band{-1};
    void capture_ordinary_pair(bool after_dispatch=false);
    void submit_ordinary_ready(unsigned point=0);
    PresentHandoff m_handoff;
    StereoHoldControl m_stereo_hold;double m_stereo_hold_next{};bool m_queue_timeline_tried{};
    native_async_lighting::Control m_async_lighting;native_async_lighting::Switches m_async_switches;double m_async_lighting_next{};  // diagnostic
    far_light_cache::Control m_far_light;far_light_cache::Switch m_far_light_switch;double m_far_light_next{};  // diagnostic
    light_cap_sort::Control m_cap_sort;double m_cap_sort_next{};  // candidate fix
    // Decide once for lights (docs/DECIDE_ONCE_STEREO.md): live stereo_lights.txt, and the cap ranking the mode wants.
    std::filesystem::path m_stereo_lights_path;double m_stereo_lights_next{};light_cap_sort::Mode m_cap_want{light_cap_sort::Mode::native};bool m_cap_want_set{};
    uint64_t m_ordinary_bridge_attempted{};
    uint64_t m_native_hud_mono_skips{}; // HUD captures skipped because the back buffer held a mono frame.
    GpuOrdinaryPairRetention m_ordinary_pair_retention;
    bool m_ordinary_pair_requested{};std::atomic<bool> m_ordinary_pair_ready{};
    // Swap-chain reset: the producers are stopped and the retention invalidated at once; the
    // re-arm runs on a later present, once the drained GPU leases allow it (bounded attempts).
    bool m_ordinary_rearm_pending{},m_ordinary_rearm_clean{};unsigned m_ordinary_rearm_attempts{};uint64_t m_ordinary_rearms{};
    OrdinaryColdRestart m_cold_restart; // Last rung of stereo recovery (OrdinaryStereoRecovery.h); present thread.
    std::vector<uint64_t> m_ordinary_pair_dumped;
    std::optional<uint64_t> m_ordinary_pair_status;
    void capture_ordinary_inline();
    void mirror_ordinary_output();
    GpuPresentMirror m_ordinary_mirror;
    bool m_ordinary_mirror_requested{};
    GpuInlineCopy m_ordinary_inline;
    GpuSdkCapture m_ordinary_sdk_capture;
    bool m_ordinary_sdk_requested{};
    GpuCompositorOutput m_ordinary_private_output;
    bool m_ordinary_private_requested{};std::atomic<bool> m_ordinary_private_stopped{};
    std::mutex m_ordinary_private_mutex;
    std::map<uint64_t,GpuCompositorOutput::Receipt> m_ordinary_private_receipts;
    std::atomic<bool> m_ordinary_inline_ready{};
    bool m_ordinary_inline_requested{};
    uint64_t m_ordinary_inline_frames{},m_ordinary_inline_tag{};
    unsigned m_ordinary_inline_attempts{},m_ordinary_compositor_depth{};
    std::map<uint64_t,std::string> m_ordinary_inline_receipts;
    std::vector<uint64_t> m_ordinary_inline_dumped;
    std::filesystem::path m_ordinary_dump_directory;
    std::chrono::steady_clock::time_point m_next_sdk_request_poll{}; // sdk_request.txt re-arms -CapturePixels.
    std::atomic<bool> m_ordinary_diagnostics{};
    uint64_t m_ordinary_last_sequence{},m_ordinary_frames{};
    unsigned m_ordinary_dumped{};
    std::filesystem::path m_pip_dump_directory;
    bool m_pip_dump_attempted{};
    bool m_pip_final_lens{},m_pip_lens_sweep{};
    uint8_t m_pip_dumped_phases{};
    std::atomic<bool> m_pip_diagnostics{};
    RenderTargetProbe& m_render_targets{RenderTargetProbe::get()};
    GpuCompletionProbe m_pip_completion;
    bool m_pip_completion_enabled{};
    bool m_pip_scene_tonemap{};
    bool m_pip_pose_tags{};
    uint64_t m_pip_readback_retries{};
    uint64_t m_pip_logged_lens_rejections{};
    uint64_t m_pip_logged_lens_skips{},m_pip_logged_lens_errors{};
    void poll_calibration();
    void save_calibration();
    void poll_arm_settings();
    void poll_swing_settings();
    void poll_weapon_settings();
    void poll_camera_guard();
    void poll_takeover_control();
    void poll_melee_sweep_control();
    void poll_view_settings();
    void poll_menu_settings();
    VrMenuPanel m_menu_settings;
    std::atomic_bool m_menu_gate{}; // The menu stand-down (menu up, or within MenuHold::SETTLE after); read by owned readiness (render thread).
    MenuHold m_menu_hold;
    VrMenuAnchor m_menu_anchor; // Fixes the panel in the room while it is shown.
    std::filesystem::path m_menu_settings_path;
    std::chrono::steady_clock::time_point m_next_menu_settings_poll{};
    bool m_menu_was_active{};
    bool apply_view_settings(const VrViewSettings&,bool persist=false);
    vr_camera::Matrix camera_anchor(const vr_camera::Matrix&,const EngineCameraProbe::VrEye&);
    bool first_person_features() const; // first person and no viewer-mode phase
    void log_viewer(uint32_t native_flags);
    VrSession::ViewerStatus m_viewer_logged;
    std::optional<uint64_t> m_viewer_release_from; // arm-driver publication count when the viewer released the head
    uint64_t m_viewer_release_hands{}; // arm-driver posed hands at that point (the log shows the pose kept for the freeze)
    bool m_viewer_keep_pose{true}; // photo mode freezes the player's arm and finger pose (SM2VR_PHOTO_NATIVE_POSE=1: the game's own)
    std::mutex m_thumbs_up_mutex;
    vr_thumbs_up::Shaper m_thumbs_up; // finger targets: thumbs-up shape and the photo-entry left thumb (SM2VR_THUMBS_UP)
    uint64_t m_thumbs_up_requests_logged{};
    VrViewSettings m_view_settings;
    VrViewCamera m_view_camera;
    VrViewHotkeys m_view_hotkeys;
    std::atomic<bool> m_view_shortcuts_enabled{};
    std::filesystem::path m_view_settings_path;
    bool m_chase_launch{}; // -ChaseCam: stock camera files, view locked to chase
    VrViewFile m_view_file; // the camera-mode file as last read or written; saves keep its own mode
    std::chrono::steady_clock::time_point m_next_view_settings_poll{};
    EngineCameraProbe::AnchorProvider m_first_person_anchor;
    std::filesystem::path m_camera_guard_path;
    std::chrono::steady_clock::time_point m_next_camera_guard_poll{};
    std::filesystem::path m_takeover_control_path;
    std::chrono::steady_clock::time_point m_next_takeover_control_poll{};
    std::filesystem::path m_melee_sweep_control_path; // melee_sweep.txt: hand, replay or animation (EngineMeleeSweep)
    std::chrono::steady_clock::time_point m_next_melee_sweep_control_poll{};
    bool m_takeover_injected{};
    std::filesystem::path m_weapon_settings_path;
    std::chrono::steady_clock::time_point m_next_weapon_settings_poll{};
    // Settings and control files are reloaded only when their last-write time or size changes (FileStamp.h).
    FileStamp m_calibration_stamp,m_menu_settings_stamp,m_arm_settings_stamp,m_swing_settings_stamp,m_camera_guard_stamp,m_takeover_control_stamp,m_melee_sweep_control_stamp,m_weapon_settings_stamp,m_stereo_lights_stamp;std::array<FileStamp,6> m_hud_stamps;
    bool m_weapon_demo{};
    std::filesystem::path m_swing_settings_path;
    std::filesystem::path m_holster_settings_path;
    std::filesystem::path m_support_settings_path;
    std::filesystem::path m_hud_settings_path;
    bool m_native_hud_requested{};uint64_t m_native_hud_frames{};VrNativeHud m_native_hud_crop;VrNativeHudRefusals m_native_hud_refusals;
    std::filesystem::path m_native_hud_settings_path;
    VrNativePrompt m_native_prompt;std::filesystem::path m_native_prompt_settings_path; // Centre interaction prompt crop.
    std::optional<bool> m_logged_secondary_equipped; // The held weapon the controls last saw (VrHolster log line).
    VrNativeHudCorners m_native_hud_corners;std::filesystem::path m_native_hud_corners_path; // Right-hand HUD corners on a low panel.
    std::filesystem::path m_viewer_play_path;FileStamp m_viewer_play_stamp; // Right A in the viewer: index_viewer_play.calibration, read live.
    VrAimDot m_aim_dot;std::filesystem::path m_aim_dot_path;uint64_t m_aim_dot_pairs{},m_aim_dot_missing{}; // Aim dot at the aim ray's hit point.
    VrHudReticle m_hud_reticle;std::filesystem::path m_hud_reticle_path;uint64_t m_reticle_pairs{},m_reticle_steered{},m_reticle_aside_pairs{}; // The game's crosshair patch at the aim point; steered: by the controller gun; aside: the canvas stayed whole.
    VrHudReticle::Aside m_reticle_aside;float m_hud_aspect{16.f/9};
    std::optional<std::array<float,2>> m_reticle_centre; // Where the game's own aim ray last put its crosshair on the canvas.
    std::chrono::steady_clock::time_point m_next_hud_settings_poll{};
    // Left-hand gestures and the playtest trace (Sm2ProbeGestures.cpp).
    std::filesystem::path m_guard_settings_path,m_grenade_settings_path,m_grenade_aim_settings_path,m_guard_guide_settings_path;
    bool m_grenade_aim_candidate{};
    void setup_gestures_and_trace();
    void trace_camera(const vr_camera::Matrix& game,const vr_camera::Matrix& anchor,const EngineCameraProbe::VrEye& eye);
    // Gaze camera candidate (VrGazeCamera.h): render thread reports native camera vs gaze.
    void observe_gaze_camera(const vr_camera::Matrix& game,const vr_camera::Matrix& anchor,const EngineCameraProbe::VrEye& eye);
    std::filesystem::path m_gaze_camera_settings_path;
    void draw_gesture_overlay();
    void log_gestures();
    void trace_weapon();
    void trace_game();
    std::chrono::steady_clock::time_point m_next_swing_settings_poll{};
    std::filesystem::path m_arm_settings_path;
    std::chrono::steady_clock::time_point m_next_arm_settings_poll{};

    bool                                  m_scripted_shift_done{ false };
    std::chrono::steady_clock::time_point m_first_present_time{};

    uint64_t                              m_present_count{ 0 };
    // SM2VR_DESKTOP_WINDOW: smaller desktop client; the eyes keep the output size (docs/EYE_SIZE.md).
    std::optional<desktop_window::Size>   m_desktop_window{};
    bool                                  m_desktop_window_pending{ true };
    uint32_t                              m_desktop_window_attempts{ 0 };
    uint64_t                              m_ui_draw_count{ 0 };
    uint64_t                              m_last_logged_present{ 0 };
    std::chrono::steady_clock::time_point m_last_log_time{};
    float                                 m_fps{ 0.0f };
    EyeTextureCapture m_eye_capture;
    EyeDumpRequest m_eye_dump_request;
    std::filesystem::path m_eye_dump_directory;
    std::chrono::steady_clock::time_point m_next_eye_dump_poll{};
    bool m_gpu_capture_enabled{};
    unsigned m_frame_trace_count{};
    float m_frame_trace_interval{};
    std::chrono::steady_clock::time_point m_next_frame_trace{};
    uint64_t m_gpu_copied{}, m_gpu_missing{}, m_gpu_busy{}, m_gpu_errors{}, m_gpu_pairs{}, m_last_gpu_pair{};
    uint64_t m_gpu_pose_matches{}, m_gpu_previous_pose_matches{}, m_gpu_pose_missing{};
    std::unique_ptr<VrSession> m_vr_session;
    bool m_runtime_enabled{},m_openxr_selected{};
    bool m_vr_framegen_blocked{};
    uint64_t m_body_readings_logged{}; // VrSession body-scale readings already logged.
    bool m_vr_stereo{};
    VrCalibration m_vr_calibration;
    std::filesystem::path m_calibration_path;
    std::chrono::steady_clock::time_point m_next_calibration_poll{};
    bool m_calibration_read_failed{};
    uint64_t m_last_vr_submitted{};
    bool m_temporal_reset{};
    VrControllerInput m_controller_input;
    std::chrono::steady_clock::time_point m_next_controller_hook_poll{};
};
