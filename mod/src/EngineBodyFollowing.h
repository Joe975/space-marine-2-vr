#pragma once
#include "EngineBodyProbe.h"
#include "EngineCameraProbe.h"
#include "VrBodyFollowing.h"
#include "EngineCinematicState.h"

class EngineBodyFollowing {
public:
    struct Api { using Yaw=void(*)(void*,float); Yaw set_yaw{};std::function<std::optional<bool>()> scripted; };
    static constexpr uintptr_t SET_YAW_RVA=0x185de20, CONTEXT_UPDATE_RVA=0x2b21390;
    static EngineBodyFollowing& get();
    static bool matches_build(uint32_t,std::span<const uint8_t> yaw,std::span<const uint8_t> context);
    bool install_in_game(HMODULE);
    bool install(void* context_update,Api api);
    bool installed() const { return m_installed; }
    void uninstall();
    void enable(bool);
    bool enabled() const { return m_enabled; }
    bool diag_no_yaw() const { return m_diag_no_yaw; }
    bool diag_no_room() const { return m_diag_no_room; }
    void allow(bool);
    void dodge(bool pressed,VrBodyFollowing::Clock::time_point now);
    void melee(bool pressed,VrBodyFollowing::Clock::time_point now); // VrBodyFollowing::melee
    void combo(uint16_t buttons,VrBodyFollowing::Clock::time_point now); // VrBodyFollowing::combo
    void stick(float x,float y,VrBodyFollowing::Clock::time_point now); // VrBodyFollowing::stick
    void head_bone(uintptr_t actor,const VrBodyFollowing::Vector& position,VrBodyFollowing::Clock::time_point received);
    void character_motion(const vr_viewer::Motion::Sample&);
    float movement_heading_offset(VrBodyFollowing::Clock::time_point now) const;
    std::optional<float> ground_speed(VrBodyFollowing::Clock::time_point now) const; // World units/second.
    bool set_settings(const VrBodyFollowing::Settings&);
    VrBodyFollowing::Settings settings() const;
    vr_camera::Matrix camera(const vr_camera::Matrix&,const EngineCameraProbe::VrEye&);
    void body(const EngineBodyProbe::Body&);
    struct Snapshot { VrBodyFollowing::Snapshot follow; uint64_t context_calls{},matched_contexts{}; VrBodyFollowing::Vector native_velocity{},body_forward{}; float facing_error_degrees{}; };
    Snapshot snapshot() const;
    std::optional<VrBodyFollowing::Vector> body_position(VrBodyFollowing::Clock::time_point now) const;
    // Diagnostic (-TakeoverControl): feed VrBodyFollowing::pulled_back in place of the native
    // camera, so an animation takeover can be exercised in the test room.
    void inject_takeover(bool value);
private:
    std::atomic<bool> m_inject_takeover{};
    VrBodyFollowing::Clock::time_point m_inject_since{};
    using Update=void(*)(void*,void*,float*);
    static void context_hook(void*,void*,float*);
    mutable std::mutex m_mutex;
    VrBodyFollowing m_follow;
    Api m_api;
    Update m_original{};
    void* m_target{};
    std::atomic<bool> m_installed{},m_enabled{};
    // Diagnostic (SM2VR_DIAG_FOLLOW_OFF=yaw,room; 30 Oct parry bisection): keep the first-person camera but
    // leave the body's facing (no set_yaw) and/or its movement (no room velocity) to the game.
    bool m_diag_no_yaw{},m_diag_no_room{};
    uint64_t m_context_calls{},m_matched_contexts{};
    VrBodyFollowing::Vector m_native_velocity{};
    VrBodyFollowing::Vector m_body_forward{};
    float m_facing_error_degrees{};
};
