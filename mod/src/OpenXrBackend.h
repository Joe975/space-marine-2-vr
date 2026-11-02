#pragma once
#include "VrSession.h"
#include "OpenXrDispatch.h"
#include <wrl/client.h>

// Rendering, head tracking and (with SM2VR_CONTROLLERS=1) controller actions, raw-equivalent
// controller poses, finger curls and the compact HUD quad. OpenVR stays selectable; both
// backends feed VrSession the same VrControllerSample and raw controller-origin poses.
class OpenXrBackend final : public VrBackend {
public:
    using Availability=bool(*)();
    using QueueFactory=HRESULT(*)(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC&,Microsoft::WRL::ComPtr<ID3D12CommandQueue>&);
    static HRESULT create_queue(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC&,Microsoft::WRL::ComPtr<ID3D12CommandQueue>&);
    OpenXrBackend();
    // Deterministic in-process runtime fixture; tests pass a failing queue factory for the fallback.
    OpenXrBackend(OpenXrDispatch,Availability,QueueFactory=create_queue);
    ~OpenXrBackend() override;
    bool available() override;
    bool prepare_graphics(ID3D12CommandQueue*) override;
    bool start() override;
    void tick() override;
    std::optional<Tracking> tracking() override;
    bool submit(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue) override;
    Transfer transfer(const EyeTextureCapture::Pair&,ID3D12CommandQueue*) override;
    ID3D12CommandQueue* completion_queue(ID3D12CommandQueue* caller) override;
    bool separate_frame_thread() const override { return true; }
    void stop() override;
    std::string status() const override;
    std::optional<VrControllerSample> controllers() override;
    bool test_input_available() const override;
    bool test_tracking_fallback_available() const override;
    std::optional<Tracking> test_tracking_seed() const override;
    std::string input_status() const override { return m_input_status; }
    std::string input_bindings() const override { return m_input_bindings; }
    bool rumble(const vr_rumble::Commands&) override;
    std::string finger_status() const override { return "left="+m_finger_status[0]+" right="+m_finger_status[1]; }
private:
    struct State;
    std::unique_ptr<State> m;
    OpenXrDispatch m_api;
    Availability m_available;
    QueueFactory m_create_queue;
    std::string m_status{"waiting for running SteamVR OpenXR runtime"};
    std::optional<std::pair<int,bool>> m_status_key; // session state and queue mode m_status was built for.
    std::string m_input_status{"controller input disabled"};
    std::string m_input_bindings; // Set before the session starts, read from other threads after.
    std::array<std::string,2> m_finger_status{"inactive","inactive"};
};
