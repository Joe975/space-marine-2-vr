#pragma once
#include "EngineArmProbe.h"
#include "VrArmPose.h"
#include "VrFingerPose.h"
#include "VrHeadVisibility.h"
#include "VrWeaponAim.h"
#include "VrTrace.h"
#include "VrUprightTorso.h"
#include "VrBodyRenderPlacement.h"
#include "NativeArmPoseInvalidation.h"
#include "VrMeleeSweep.h"
#include <filesystem>
#include <iosfwd>
#include <atomic>
class EngineArmDriver {
public:
    using Clock=EngineArmProbe::Clock;
    using Matrix=vr_arm::Matrix;
    struct Grip { vr_arm::Vector metres{},degrees{}; bool operator==(const Grip&) const = default; };
    struct Settings {
        std::array<Grip,2> grips{};
        std::array<vr_arm::Vector,2> elbow_poles{{{0.4f,-0.8f,0},{-0.4f,-0.8f,0}}}; // Character axes, unitless directions.
        bool valid() const;
        bool operator==(const Settings&) const = default;
        static std::optional<Settings> read(std::istream&);
        static std::optional<Settings> load(const std::filesystem::path&);
        bool save(const std::filesystem::path&) const;
    };
    // firearm_trigger: the pad's RT or LT is pressed (a shot or aim); the upright twist stands down (vr_upright::FireHold).
    // units_per_metre: the Marine's (VrEye::marine_units_per_metre), for grips and body placement; wrists are already world poses.
    struct Targets { uintptr_t actor{}; uint64_t reference{},epoch{}; Clock::time_point captured{}; std::array<std::optional<Matrix>,2> wrists; std::array<std::optional<vr_finger::Curl>,2> fingers; float units_per_metre{1}; bool firearm_trigger{}; };
    struct Api {
        std::function<bool(const EngineArmProbe::Pose&,Clock::time_point)> current;
        std::function<void(uintptr_t)> enter,leave;
        std::function<bool(uintptr_t,const Matrix&)> write;
        // Injectable bounded reader; native configuration uses read_control().
        std::function<vr_body_placement::ControlRead(const std::filesystem::path&)> read_body_control;
        std::function<vr_arm_cache::Receipt(uintptr_t)> invalidate_pose;
        std::function<bool(uintptr_t,uint32_t,Matrix&)> read_evaluated;
        // Optional: writes adjacent patched slots in one call (the native configuration); unset, write per bone.
        std::function<bool(uintptr_t,const void*,size_t)> write_bytes;
    };
    struct Snapshot { uint64_t updates{},hands{},bone_writes{},rejected{},failures{},finger_hands{},finger_rejected{},head_updates{},head_rejected{}; std::array<float,2> errors{},finger_open_angles{}; bool enabled{},fingers_enabled{true},head_hidden{};
        bool upright_enabled{}; uint64_t upright_updates{},upright_rejected{}; float upright_before_degrees{},upright_after_degrees{},upright_twist_before_degrees{},upright_twist_after_degrees{};
        float body_back_metres{}; uint64_t body_placement_updates{},body_placement_rejected{},body_placement_suspended{};
        float body_applied_back_metres{};uint64_t body_control_polls{},body_control_rejected{},body_control_revision{},body_control_superseded{};
        vr_body_placement::ControlStatus body_control_status{vr_body_placement::ControlStatus::Disabled};
        bool invalidate_pose_enabled{};uint64_t invalidation_calls{},invalidation_refused{};
        vr_arm_cache::Receipt invalidation;
        bool resolve_pose_enabled{},pose_recovery_failed{};uint64_t recovery_calls{},recovery_failures{};
        std::array<uint64_t,static_cast<size_t>(vr_body_placement::Refusal::Count)> body_placement_refusals{};
        std::optional<vr_weapon::Grip> weapon_grip; uint64_t weapon_grip_switches{},weapon_left_refused{},weapon_primary_draws{}; bool weapon_seated{true}; }; // The committed grip; settled grip changes; left readings refused for a primary (frames); primaries landed by a draw from the pistol; the fit came from the grip's socket.
    static EngineArmDriver& get();
    void configure(Api);
    void configure_native(bool diagnostic=false);
    static Targets diagnostic_targets(const EngineArmProbe::Pose&);
    void enable(bool);
    void enable_fingers(bool);
    void enable_pose_invalidation(bool);
    void enable_pose_resolution(bool);
    void hide_head(bool);
    // Opt-in upright upper body (VrUprightTorso.h, docs/BODY_CLIPPING.md); camera unchanged.
    void enable_upright(bool);
    bool set_upright_settings(const vr_upright::Settings&);
    vr_upright::Settings upright_settings() const;
    bool set_body_placement(const vr_body_placement::Settings&);
    bool set_body_placement_control(const std::filesystem::path&);
    void follow_weapon(bool);
    // A weapon switch was requested (the native D-pad down went out): for GripSwitch::SWITCH_WINDOW a
    // primary may give way to the pistol in the left hand (vr_weapon::GripSwitch).
    void weapon_switch(Clock::time_point now);
    bool set_weapon_settings(const vr_weapon::Settings&);
    vr_weapon::Settings weapon_settings() const;
    void set_weapon_observer(std::function<void(const VrWeaponAim::Pose&)>);
    bool weapon_enabled() const;
    // head=false stops the configured head hiding from the next publication on while the arms
    // and fingers stay driven: the viewer freezes the player's pose with a full-size head.
    void allow(bool,uint64_t epoch,bool head=true);
    void targets(const Targets&);
    void apply(const EngineArmProbe::Pose&);
    void apply(const EngineArmProbe::Pose&,Clock::time_point now);
    bool set_settings(const Settings&);
    Settings settings() const;
    Matrix grip(size_t) const;
    Snapshot snapshot() const;
    // The animated HEAD bone of the last publication (native animation, before any change by
    // the driver), read for every publication whether or not the driver may write. Body
    // following uses it to draw the head during takeovers once the view is clear of it.
    struct NativeHead { uintptr_t actor{}; vr_arm::Vector position{}; Clock::time_point received{}; };
    std::optional<NativeHead> native_head() const;
    // The root, HEAD, HANDL and HANDR of the last publication in world units (native animation,
    // read like the head for every publication). A viewer play ends once they settle (VrViewer.h).
    struct NativeMotion { uintptr_t actor{}; std::array<std::optional<vr_arm::Vector>,4> points{}; Clock::time_point received{}; };
    std::optional<NativeMotion> native_motion() const;
    // After each committed write: GUN_R relative to the rig's root bone as written (final) and as
    // animated (native), with the evaluated-node indices of GUN_R and the root bone. The game's
    // melee hit check reads that node (EngineMeleeSweep). Called under the driver's lock.
    struct Sword { uintptr_t instance{}; int32_t sword{-1},root{-1}; Matrix final{},native{}; Clock::time_point time{}; };
    void set_sword_observer(std::function<void(const Sword&)>);
private:
    mutable std::mutex m_mutex;
    Api m_api;
    Settings m_settings;
    Snapshot m_state;
    bool m_allowed{},m_head_allowed{true};
    uint64_t m_epoch{};
    std::optional<Targets> m_targets;
    bool m_weapon_enabled{};
    vr_upright::Settings m_upright;vr_upright::FireHold m_fire_hold;
    vr_body_placement::Settings m_body_placement;
    std::atomic<bool> m_body_control_enabled{false};
    std::filesystem::path m_body_control_path;
    Clock::time_point m_next_body_control_poll{};
    uint64_t m_body_control_generation{};
    uint64_t m_body_control_poll_sequence{};
    void poll_body_placement_control(Clock::time_point);
    void apply_publication(const EngineArmProbe::Pose&,Clock::time_point);
    uint64_t m_weapon_sequence{};
    uintptr_t m_weapon_actor{};
    std::shared_ptr<const EngineArmProbe::RigLayout> m_weapon_layout;
    std::optional<size_t> m_weapon_hand;
    std::optional<Matrix> m_weapon_relative;
    std::optional<vr_weapon::Grip> m_weapon_grip; // Committed with the hand and fit.
    bool m_weapon_seated{true};                   // Committed with the fit: false while it is provisional.
    vr_weapon::GripSwitch m_weapon_switch;        // Debounces the socket reading (VrWeaponAim.h).
    float m_trace_units{1}; // Last known world units per metre, for trace metrics when no targets are live.
    std::optional<NativeHead> m_native_head;
    std::optional<NativeMotion> m_native_motion;
    vr_weapon::Settings m_weapon_settings;
    std::function<void(const VrWeaponAim::Pose&)> m_weapon_observer;
    std::function<void(const Sword&)> m_sword_observer;
};
