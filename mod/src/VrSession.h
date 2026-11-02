#pragma once
#include "EngineCameraProbe.h"
#include "EyeTextureCapture.h"
#include "VrBodyScale.h"
#include "VrCalibration.h"
#include "VrControllerInput.h"
#include "VrFingerPose.h"
#include "VrSwingMelee.h"
#include "VrHolster.h"
#include "VrGuard.h"
#include "VrGuardGuide.h"
#include "VrLeftPush.h"
#include "VrGrenadeThrow.h"
#include "VrSupportGrip.h"
#include "VrSprintCamera.h"
#include "VrCombatRouting.h"
#include "UnknownGrace.h"
#include "VrWeaponAim.h"
#include "VrTrace.h"
#include "VrEyeSourceDiagnostic.h"
#include "VrLeftAButton.h"
#include "VrTouchXButton.h"
#include "VrTapHold.h"
#include "VrViewer.h"
#include "VrPhoto.h"
#include "VrRumble.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// The game adapter owns explicit camera/image identities. A backend only supplies tracking
// and submits an already validated pair; it must never choose an eye from Present parity.
class VrBackend {
public:
    struct Tracking {
        vr_camera::Matrix head{vr_camera::IDENTITY};
        std::array<vr_camera::Matrix,2> eyes{vr_camera::IDENTITY,vr_camera::IDENTITY};
        std::array<vr_projection::Tangents,2> fov;
        std::array<std::optional<vr_camera::Matrix>,2> controllers; // Raw left/right device origins in standing space.
        std::array<std::optional<vr_finger::Curl>,2> fingers;
        std::optional<bool> worn; // The headset is on the player's head; empty when the runtime cannot tell.
    };
    virtual ~VrBackend() = default;
    virtual bool available() = 0; // Read-only availability; must not start SteamVR.
    virtual bool prepare_graphics(ID3D12CommandQueue*) { return true; }
    virtual bool start() = 0;
    virtual void tick() {}
    virtual std::optional<Tracking> tracking() = 0;
    virtual bool submit(const EyeTextureCapture::Pair&,ID3D12CommandQueue*) = 0;
    enum class Transfer { accepted, skipped, failed };
    virtual Transfer transfer(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue) {
        return submit(pair,queue)?Transfer::accepted:Transfer::failed;
    }
    // The queue whose completion means the runtime transfer of a pair has finished reading it
    // (the caller's queue unless the backend copies on its own queue).
    virtual ID3D12CommandQueue* completion_queue(ID3D12CommandQueue* caller) { return caller; }
    // True when tick() and transfer() may run on one thread of their own while every other call
    // (tracking, controllers, rumble, status) runs on others: VrSession's submit thread.
    virtual bool separate_frame_thread() const { return false; }
    virtual void stop() = 0;
    virtual std::string status() const = 0;
    virtual std::optional<VrControllerSample> controllers() { return std::nullopt; }
    // Test driver may replace asleep controllers, but never dashboard/runtime focus.
    virtual bool test_input_available() const { return false; }
    // Explicit scripted-head tests may retain previously observed optics during
    // ordinary HMD tracking loss. Shutdown/origin changes must never qualify.
    virtual bool test_tracking_fallback_available() const { return false; }
    virtual std::optional<Tracking> test_tracking_seed() const { return {}; }
    virtual std::string input_status() const { return "controller input unavailable"; }
    virtual std::string finger_status() const { return "skeletal input unavailable"; }
    // Game rumble on the controllers (vr_rumble::Driver commands, left then right). False
    // when the runtime cannot play haptics now (no input focus, unsupported backend).
    virtual bool rumble(const vr_rumble::Commands&) { return false; }
};

class VrSession {
public:
    enum class RenderPath {afr,ordinary};
    struct Statistics {
        uint64_t submitted_pairs{}, failed_transfers{}, last_render_id{};
        double oldest_pose_age_ms{}, submit_cpu_ms{};
        // Session gate, image identity, missing issuance, stale issuance, metadata.
        std::array<uint64_t,5> ordinary_refusals{};
        std::array<uint64_t,2> ordinary_generations{};
        double ordinary_pose_age_ms{};
        VrEyeSourceDiagnostic::Mode eye_source{VrEyeSourceDiagnostic::Mode::Native};
        uint64_t eye_source_since_render_id{};
        uint64_t worn_references{}; // References taken again because the headset was put on.
        // Submit thread: running, and pairs a newer one replaced before the thread took them.
        bool submit_thread{}; uint64_t submit_replaced{};
    };
    struct ControllerPoses {
        std::array<std::optional<vr_camera::Matrix>,2> relative;
        std::chrono::steady_clock::time_point captured{};
        uint64_t reference_epoch{},tracking_epoch{};
        float units_per_metre{1};
        std::array<std::optional<vr_finger::Curl>,2> fingers;
        float fit_factor{1}; // As EngineCameraProbe::VrEye::fit_factor.
    };
    struct OrdinaryFrame {
        uint32_t game_token{};uint64_t render_id{};
        std::array<EngineCameraProbe::VrEye,2> eyes;
        std::optional<ControllerPoses> controllers;
        std::optional<VrControllerSample> input;
        bool valid_at(std::chrono::steady_clock::time_point)const;
        // GPU completion may outlive input freshness; this only permits image submission.
        bool valid_for_submission(std::chrono::steady_clock::time_point)const;
    private:
        bool valid_at(std::chrono::steady_clock::time_point,std::chrono::milliseconds max_age)const;
    };
    struct OrdinaryImages { EyeTextureCapture::Pair images; uint64_t dispatch{}; Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue; };
    explicit VrSession(std::unique_ptr<VrBackend> backend) : m_backend{std::move(backend)} {}
    ~VrSession();
    void enable(bool);
    void poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),ID3D12CommandQueue* queue = nullptr);
    // Player-initiated recenter (Left A hold, F9, the overlay button, native Back): also
    // measures the player's eye height for the body scale (docs/BODY_SCALE.md).
    void recenter();
    // Automatic reference reset (frame generation, putting the headset on, ...): never measures.
    // An automatic reference stands at the held eye height above the floor (the calibration's
    // reference before any recenter), wherever the headset happens to be.
    void reset_reference();
    struct BodyScaleStatus {
        std::optional<float> player_eye_height,reference_eye_height;
        std::optional<VrBodyScale::Reading> last; // Latest explicit-recenter reading.
        uint64_t readings{},rejected{};
        float factor{1}; // As applied now (1 outside first person).
        float units_per_metre{1}; // The eyes' scale as applied now, first-person scale and factor included.
    };
    BodyScaleStatus body_scale() const;
    void set_stereo(bool);
    bool set_render_path(RenderPath);
    RenderPath render_path()const;
    bool set_scale(float units_per_metre);
    bool set_calibration(const VrCalibration&);
    VrCalibration calibration() const;
    bool set_view_settings(const VrViewSettings&);
    VrViewSettings view_settings() const;
    // native_flags: published|requested native game-state flags (EngineCinematicState), for the
    // viewer's pause/photo-mode sequence. Left B: tap = native Start, one-second hold = viewer.
    // character_native: the game has published its own player pose since the viewer switched
    // first-person edits off (the viewer waits for it before pausing).
    // native_ground_speed: the followed native body's horizontal speed in world units/second
    // (EngineBodyFollowing::ground_speed), which confirms a native sprint for the sprint camera.
    // native_look: gaze camera steering (VrGazeCamera.h) for the native right stick; applied
    // only to first-person gameplay pads, never in the pause menu or viewer mode.
    void poll_controllers(VrControllerInput&,bool foreground,
        std::chrono::steady_clock::time_point now = {},float native_camera_heading_offset=0,std::optional<bool> secondary_equipped=std::nullopt,
        std::optional<uint32_t> native_flags=std::nullopt,bool character_native=true,std::optional<float> native_ground_speed=std::nullopt,
        std::optional<std::array<float,2>> native_look=std::nullopt);
    // Sprint-only forward eye offset (docs/SPRINT_CAMERA.md), as applied at `now`.
    VrSprintCamera::Snapshot sprint_camera(std::chrono::steady_clock::time_point now) const;
    // Viewer mode (docs/VIEWER_MODE.md). The ordinary provider reports each first-person anchor
    // it draws, so the viewer starts exactly where first person left off.
    // floor: the followed character's ground height (world units), so the viewer can stand on it.
    void note_first_person_anchor(const vr_camera::Matrix& anchor,const EngineCameraProbe::VrEye& eye,std::optional<float> floor={});
    // plays/refreezes: Right A plays and Right A freezes during them; play_end: why the last play
    // ended; play_seconds: how long it ran; play_speed: the character's last measured speed (m/s).
    struct ViewerStatus {VrViewerSequencer::State state{VrViewerSequencer::State::off};VrViewerRig::Snapshot rig;uint64_t requests{},refusals{},failures{},unreleased{};
        uint64_t plays{},refreezes{};vr_viewer::PlayEnd play_end{vr_viewer::PlayEnd::none};float play_seconds{},play_speed{};bool play_moved{};};
    ViewerStatus viewer_status() const;
    bool viewer_owns_camera() const; // the free camera draws the eyes (photo mode, or a play under it)
    bool viewer_engaged() const;     // any viewer phase, including the menu presses around it
    bool viewer_playing() const;     // a play: the game runs (or is let go or caught again) under the free camera
    // Right A in the viewer (docs/VIEWER_MODE.md "Watch it play out"): when a play ends.
    void set_viewer_play(const vr_viewer::Play&);
    vr_viewer::Play viewer_play() const;
    // The player character's latest native pose publication (EngineArmDriver::native_motion); a
    // play in animation mode ends once it has moved and then stayed still.
    void note_character_motion(const vr_viewer::Motion::Sample&);
    // True once per right stick click in camera mode (vr_viewer::takes_photos): save a picture.
    bool take_photo_request();
    // The shutter tick on the right hand. False when the runtime can't play it now.
    bool photo_pulse();
    std::string input_status() const;
    std::string finger_status() const;
    void enable_swing_melee(bool);
    // Called (under the session lock) when a physical swing commits its native attack request.
    void set_swing_observer(std::function<void(std::chrono::steady_clock::time_point)>);
    void set_eye_source_control(std::filesystem::path path){std::lock_guard lock{m_mutex};m_eye_source.set_control(std::move(path));}
    bool swing_melee_enabled() const;
    void allow_swing_melee(uintptr_t actor);
    bool set_swing_settings(const VrSwingMelee::Settings&);
    VrSwingMelee::Settings swing_settings() const;
    VrSwingMelee::Snapshot swing_statistics() const;
    // Optional touch veto for light swings. Pulled trigger selects a power swing.
    void require_trigger_lifted(bool);
    bool trigger_lift_required() const;
    void enable_holsters(bool);
    bool holsters_enabled() const;
    bool set_holster_settings(const VrHolster::Settings&);
    VrHolster::Settings holster_settings() const;
    VrHolster::Snapshot holster_statistics() const;
    // Left-hand gestures. Guard adds to the left grip's parry; grenade adds to the right
    // trackpad's equipment. The grenade owns the left grip at the belt and while held.
    void enable_guard(bool);
    bool guard_enabled() const;
    // Before 26 Oct a hand resting in the grenade belt latched the guard (VrGuard::poll); true restores that.
    void set_guard_belt_latch(bool);
    bool guard_belt_latch() const;
    bool set_guard_settings(const VrGuard::Settings&);
    VrGuard::Settings guard_settings() const;
    VrGuard::Snapshot guard_statistics() const;
    // Guard guide (VrGuardGuide.h): the ghost bar drawn into the eyes while the sword is out.
    void enable_guard_guide(bool);
    bool guard_guide_enabled() const;
    bool set_guard_guide_settings(const VrGuardGuide::Settings&);
    VrGuardGuide::Settings guard_guide_settings() const;
    std::optional<VrGuardGuide::Draw> guard_guide(std::chrono::steady_clock::time_point now) const;
    VrGuardGuide::Tracker::Status guard_guide_status() const;
    // Left-arm push (VrLeftPush): the native held strike, knockback, with the sword out.
    void enable_left_push(bool);
    bool left_push_enabled() const;
    VrLeftPush::Snapshot left_push_statistics() const;
    // Game rumble -> controller haptics (docs/HAPTICS.md), played from poll_controllers.
    // `source` is the engine's rumble mailbox; tests pass their own.
    bool enable_rumble(bool,vr_rumble::Settings={},vr_rumble::Mailbox* source=&vr_rumble::Mailbox::get());
    struct RumbleStatistics { bool enabled{}; uint64_t game_calls{},applies{},stops{},failures{}; float peak{}; std::array<uint64_t,4> amplitude_bins{}; uint64_t test_pulses{}; float frequency{}; };
    RumbleStatistics rumble_statistics() const;
    void enable_grenade_gesture(bool);
    bool grenade_gesture_enabled() const;
    bool set_grenade_settings(const VrGrenadeThrow::Settings&);
    VrGrenadeThrow::Settings grenade_settings() const;
    bool set_grenade_aim_settings(const VrGrenadeAim::Settings&);
    VrGrenadeAim::Settings grenade_aim_settings() const;
    VrGrenadeThrow::Snapshot grenade_statistics() const;
    bool set_support_settings(const VrSupportGrip::Settings&);
    VrSupportGrip::Settings support_settings() const;
    VrSupportGrip::Snapshot support_statistics() const;
    std::optional<VrWeaponAim::Pose> grenade_aim(const vr_camera::Matrix& anchor,uintptr_t actor,uint64_t epoch,std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    std::optional<ControllerPoses> controller_poses(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;
    std::optional<EngineCameraProbe::VrEye> eye(const FrameSyncClock::Frame&,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // UI/scene consumers share the same issued pose for an exact frame/eye, never parity.
    std::optional<EngineCameraProbe::VrEye> shared_eye(const FrameSyncClock::Frame&,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // One exact native camera-job identity, both eyes and hands from ONE backend
    // tracking read. Frame.eye is deliberately irrelevant to ordinary stereo.
    // Separate cache/type: never manufacture adjacent AFR IDs or tokens.
    // Head-locked menu frames while the ordinary producer is idle (native pause/settings):
    // current tracking per eye, and a submission that leaves the ordinary sequence untouched.
    std::optional<std::array<EyeTextureCapture::Metadata,2>> menu_eyes(std::chrono::steady_clock::time_point now);
    bool submit_menu(const EyeTextureCapture::Pair&,ID3D12CommandQueue*,std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    std::optional<OrdinaryFrame> ordinary_frame(const FrameSyncClock::Frame&,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    bool submit(const EyeTextureCapture::Pair&,ID3D12CommandQueue*,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    bool submit_ordinary(const OrdinaryImages&,ID3D12CommandQueue*,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::string status() const;
    uint64_t epoch() const;
    Statistics statistics() const;
    // Runtime frame work on a thread of its own (docs/OPENXR_SUBMIT_THREAD.md): the backend's tick()
    // and transfer() run there, so a blocking xrWaitFrame/xrBeginFrame holds that thread instead of
    // the caller (the game's render/Present thread). Submissions hand the pair over and return true;
    // a newer pair replaces one the thread has not taken yet. Only for backends that allow it
    // (separate_frame_thread); takes effect when the backend next starts. `wrap` runs each worker
    // step (Sm2Probe passes GpuCompletionProbe::internal, as it does around its own submissions).
    using StepWrapper=std::function<void(const std::function<void()>&)>;
    void set_submit_thread(bool enabled,StepWrapper wrap={});
private:
    struct Pending {
        EyeTextureCapture::Pair pair;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t value{};
    };
    void invalidate();
    bool update_tracking(const VrBackend::Tracking&,std::chrono::steady_clock::time_point);
    std::optional<EngineCameraProbe::VrEye> make_eye(const VrBackend::Tracking&,int,uint64_t,std::chrono::steady_clock::time_point)const;
    std::optional<ControllerPoses> make_controller_poses(std::chrono::steady_clock::time_point)const;
    bool request_viewer(std::optional<uint32_t> native_flags,std::chrono::steady_clock::time_point now);
    void poll_viewer(VrControllerInput&,bool foreground,std::chrono::steady_clock::time_point now,std::optional<uint32_t> native_flags);
    // Left B in one of its three contexts. A pulse ends when the context changes, so a Start that opened
    // the pause menu never carries into it, and one that closed it never reopens it from play.
    enum class MenuButtonContext : uint8_t { gameplay, menu, viewer };
    VrTapHold::Result menu_button_locked(MenuButtonContext,bool pressed,bool armed,std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::duration min_tap=std::chrono::steady_clock::duration::zero());
    void poll_controllers_locked(VrControllerInput&,bool foreground,std::chrono::steady_clock::time_point now,float native_camera_heading_offset,
        std::optional<bool> secondary_equipped,std::optional<uint32_t> native_flags,std::optional<std::array<float,2>> native_look);
    void update_sprint_camera(VrControllerInput&,std::chrono::steady_clock::time_point now);
    // One trace input record (SM2VR_TRACE) of what the game was given this poll. menu: the poll
    // took the menu branch (pause panel or a menu over stereo), so sample is the menu sample.
    void trace_input_locked(const VrControllerInput&,std::chrono::steady_clock::time_point now,const std::optional<VrControllerSample>& sample,
        const std::optional<VrControllerSample>& raw_sample,bool supporting,bool two_handed,bool menu);
    // Accepted head_forward_offset_metres plus the sprint offset blended in at `now`, in
    // body-scaled metres (divided by the body factor) so the eye keeps its game-unit place.
    float forward_offset_locked(std::chrono::steady_clock::time_point now) const;
    float height_offset_locked() const;
    // The body factor applied now: 1 outside first person or until a recenter has measured.
    float body_factor_locked() const;
    // The first-person scale times the body factor; 1 outside first person (VrEye::fit_factor).
    float fit_factor_locked() const;
    // Game units per tracked metre: the view mode's scale times the body factor.
    float units_locked() const;
    std::optional<EngineCameraProbe::VrEye> acquire_eye(const FrameSyncClock::Frame&,std::chrono::steady_clock::time_point,bool reuse);
    void retire();
    bool transfer_images(const EyeTextureCapture::Pair&,ID3D12CommandQueue*,std::chrono::steady_clock::time_point);
    // Submit thread state. Lock order: m_mutex, then Worker::mutex; the thread itself only ever
    // takes Worker::mutex, so joining it under m_mutex cannot deadlock.
    struct Worker {
        struct Job { EyeTextureCapture::Pair pair; Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue; Microsoft::WRL::ComPtr<ID3D12Fence> fence; std::chrono::steady_clock::time_point now; };
        std::mutex mutex; std::condition_variable wake; std::thread thread;
        bool stop{},failed{},busy{};
        std::optional<Job> queued;
        std::deque<Pending> pending; // Transferred pairs, leased until their fence value.
        ID3D12Fence* tick_fence{}; uint64_t tick{};
        uint64_t submitted{},failed_transfers{},replaced{},last_render_id{};
        double oldest_pose_age_ms{},submit_cpu_ms{};
    };
    void worker_loop(Worker&);
    void start_worker_locked();
    void stop_worker_locked();
    void collect_worker_locked(); // Retires worker leases; a worker failure stops the session.
    size_t in_flight_locked() const;
    bool m_submit_thread{}; StepWrapper m_step_wrap;
    std::unique_ptr<Worker> m_worker;
    mutable std::mutex m_mutex;
    std::function<void(std::chrono::steady_clock::time_point)> m_swing_observer;
    std::unique_ptr<VrBackend> m_backend;
    bool m_enabled{}, m_started{}, m_failed{};
    bool m_stereo{}; // Start with recentered mono head tracking; enable eye offsets after validation.
    VrEyeSourceDiagnostic m_eye_source;
    RenderPath m_render_path{RenderPath::afr};
    VrCalibration m_calibration;
    VrBodyScale m_body;
    bool m_measure_height{}; // The next reference comes from a player-initiated recenter.
    std::optional<bool> m_worn; // Latest known VrBackend::Tracking::worn.
    uint64_t m_body_readings{},m_body_rejected{};
    VrSprintCamera m_sprint_camera;
    std::optional<float> m_native_ground_speed; // World units/second, from the latest controller poll.
    VrViewSettings m_view_settings;
    uint64_t m_epoch{1}, m_last_eye{}, m_last_submitted{}, m_tick{};
    uint64_t m_reference_epoch{};
    std::optional<vr_camera::Matrix> m_reference;
    vr_camera::Matrix m_rig{vr_camera::IDENTITY},m_relative_head{vr_camera::IDENTITY};
    std::array<std::optional<vr_camera::Matrix>,2> m_relative_controllers;
    std::array<std::optional<vr_finger::Curl>,2> m_fingers;
    std::chrono::steady_clock::time_point m_head_received{};
    std::optional<VrSwingMelee::Pose> m_swing_pose;
    VrSwingMelee m_swing;
    bool m_swing_enabled{},m_swing_touch_gate{};
    VrHolster m_holster;
    std::optional<VrSwingMelee::Pose> m_left_pose; // Raw left controller and head, as m_swing_pose is for the right.
    VrHandMotion m_left_motion; // Left endpoint speed: a left punch must not become a right swing.
    VrTwoHanded m_two_handed; // Both hands on the sword: no left veto, no guard.
    VrGunMeleeGate m_gun_gate; // Bolter-out swings: triggers released and no support grip for 300 ms.
    VrLeftPush m_left_push;
    VrPowerTrigger m_power_trigger; // Power: right trigger pulled before the stroke (VrPowerTrigger::HOLD).
    VrGuard m_guard;
    VrGuardGuide::Tracker m_guard_guide;
    VrGrenadeThrow m_grenade;
    static constexpr auto PUT_BACK_TAP=std::chrono::milliseconds(200); // Native LB after a grenade put back.
    uint64_t m_grenade_cancels_seen{};std::chrono::steady_clock::time_point m_put_back_until{};
    static constexpr auto UNKNOWN_LOADOUT=std::chrono::seconds(2); // Held weapon unknown this long: draws, swings and primary triggers allowed.
    UnknownGrace m_loadout_grace{UNKNOWN_LOADOUT};
    VrSupportGrip m_support;
    VrCombatRouting m_combat;
    struct ThrowAim {vr_camera::Matrix relative{};std::optional<vr_camera::Matrix> world;uint64_t epoch{},sequence{};std::chrono::steady_clock::time_point released{};uintptr_t actor{};float speed_scale{1};};
    std::optional<ThrowAim> m_throw_aim;
    bool m_guard_enabled{},m_guard_belt_latch{},m_grenade_enabled{},m_left_push_enabled{};
    void update_rumble(bool foreground,std::chrono::steady_clock::time_point now);
    bool m_rumble_enabled{}; vr_rumble::Settings m_rumble_settings; vr_rumble::Mailbox* m_rumble_source{};
    vr_rumble::Driver m_rumble; uint64_t m_rumble_failures{},m_rumble_calls{},m_rumble_tests{}; bool m_rumble_test_pending{};
    std::optional<vr_camera::Matrix> m_trace_head;
    bool m_holster_enabled{};
    uintptr_t m_swing_actor{};
    VrLeftAButton m_left_a;
    VrTouchXButton m_touch_x; // Left X and Y on Touch controllers, in place of m_left_a.
    VrTapHold m_menu_button; // Left B, shared by gameplay, menu and viewer polls (menu_button_locked).
    MenuButtonContext m_menu_button_context{MenuButtonContext::gameplay};
    bool m_menu_tap{},m_menu_tap_start{}; // Left B's pulse on a menu, and whether it is Start (the pause menu) or Y.
    // Left B pauses only from a press this long: a brushed Left B opened the pause menu mid-fight twice
    // (29 Oct 08:19, 30 Oct 20:45:40 with Left A in the same thumb press; Joe chose a held press, 31 Oct).
    // It was 150 ms until the 31 Oct 16:04 playtest, in the middle of Joe's plain taps: 7 of his 12
    // tap-length presses in play did nothing, and three times he pressed twice, then longer.
    static constexpr auto PAUSE_MIN_PRESS=std::chrono::milliseconds(70);
    VrViewerSequencer m_viewer_sequence;
    VrViewerRig m_viewer;
    struct ViewerEntry {vr_camera::Matrix anchor{vr_camera::IDENTITY};float units_per_metre{1};uint64_t epoch{};std::optional<float> floor;};
    std::optional<ViewerEntry> m_viewer_entry,m_viewer_start;
    std::chrono::steady_clock::time_point m_viewer_polled{};
    uint64_t m_viewer_requests{},m_viewer_refusals{};
    vr_viewer::Play m_viewer_play;
    vr_viewer::Motion m_viewer_motion;
    bool m_play_button{}; // Right A at the last viewer poll (a press starts or cancels a play).
    bool m_photo_button{},m_photo_requested{}; // Right stick click at the last viewer poll; a click waiting for take_photo_request.
    vr_viewer::PlayEnd m_play_end{vr_viewer::PlayEnd::none};
    float m_play_seconds{};
    bool m_character_native{true};
    std::chrono::steady_clock::time_point m_menu_input_until{}; // Menu panel recently submitted: menu input allowed.
    std::chrono::steady_clock::time_point m_panel_over_stereo_until{}; // The last ordinary pair carried the menu panel: menu input.
    std::deque<EngineCameraProbe::VrEye> m_eyes;
    std::deque<OrdinaryFrame> m_ordinary_frames;
    std::chrono::steady_clock::time_point m_next_poll{};
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    std::deque<Pending> m_pending;
    Statistics m_statistics;
};
