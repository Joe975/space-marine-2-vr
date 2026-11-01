#pragma once
#include "VrSession.h"
#include "ValidatedQueue.h"
#include "VrCompactHud.h"
#include <openvr.h>

class OpenVrBackend final : public VrBackend {
public:
    ~OpenVrBackend() override { stop(); }
    bool available() override;
    bool start() override;
    std::optional<Tracking> tracking() override;
    bool submit(const EyeTextureCapture::Pair&,ID3D12CommandQueue*) override;
    void stop() override;
    std::string status() const override { return m_status; }
    std::optional<VrControllerSample> controllers() override;
    bool test_input_available() const override;
    bool test_tracking_fallback_available() const override;
    std::optional<Tracking> test_tracking_seed() const override;
    std::string input_status() const override { return m_input_status; }
    std::string finger_status() const override { return "left="+m_finger_status[0]+" right="+m_finger_status[1]; }
private:
    HMODULE m_api{};
    vr::IVRSystem* m_system{};
    vr::IVRCompositor* m_compositor{};
    ValidatedQueue<ID3D12CommandQueue> m_validated_queue;
    std::string m_status{"waiting for SteamVR and HMD"};
    double m_next_profile_activity{};
    std::string m_input_status{"controller input disabled"};
    vr::IVRInput* m_input{};
    vr::VRActionSetHandle_t m_actions{};
    std::array<vr::VRActionHandle_t,7> m_input_actions{};
    std::array<vr::VRActionHandle_t,7> m_combat_actions{};
    vr::VRActionHandle_t m_finisher_action{},m_fire_touch_action{};
    std::array<vr::VRActionHandle_t,2> m_finger_actions{};
    std::array<std::string,2> m_finger_status{"inactive","inactive"};
    void initialize_input();
    void update_hud();
    vr::VROverlayHandle_t m_hud{vr::k_ulOverlayHandleInvalid};
    std::chrono::steady_clock::time_point m_next_hud{};
};
