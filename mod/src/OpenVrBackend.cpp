#include "OpenVrBackend.h"
#include "PerformanceTrace.h"
#include "OpenVrCombatActions.h"
#include "OpenVrConversions.h"
#include <TlHelp32.h>
#include <filesystem>
#include <dxgi1_6.h>

namespace {
bool steamvr_running() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snapshot == INVALID_HANDLE_VALUE) { return false; }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot,&entry)) do {
        if (_wcsicmp(entry.szExeFile,L"vrserver.exe") == 0) { found = true; break; }
    } while (Process32NextW(snapshot,&entry));
    CloseHandle(snapshot); return found;
}
}
bool OpenVrBackend::available() {
    // Do not autostart SteamVR from an offline test or from another project's XR tests.
    if (!steamvr_running()) { m_status = "waiting for running SteamVR"; return false; }
    if (!m_api) {
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr,executable,32768);
        if (!length || length >= 32768) { m_status = "cannot resolve game directory"; return false; }
        const auto path = std::filesystem::path{executable}.parent_path()/L"openvr_api.dll";
        m_api = LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_api) { m_status = "openvr_api.dll missing; use the runtime-enabled test launcher"; return false; }
    }
    if (!vr::VR_IsHmdPresent() || !vr::VR_IsRuntimeInstalled()) { m_status = "waiting for HMD"; return false; }
    return true;
}
bool OpenVrBackend::start() {
    auto error = vr::VRInitError_None;
    m_system = vr::VR_Init(&error,vr::VRApplication_Scene);
    if (error != vr::VRInitError_None || !m_system) { m_system = nullptr; m_status = "VR_Init failed: " + std::to_string(error); return false; }
    m_compositor = vr::VRCompositor();
    if (!m_compositor) { stop(); m_status = "OpenVR compositor unavailable"; return false; }
    m_compositor->SetTrackingSpace(vr::TrackingUniverseStanding);
    initialize_input();
    m_status = "OpenVR active; waiting for valid tracking";
    return true;
}
std::optional<VrBackend::Tracking> OpenVrBackend::tracking() {
    update_hud();
    PerformanceTrace::Scope tracking_scope{"openvr_tracking_cpu"};
    if (!m_system || !m_compositor) { return std::nullopt; }
    if(tracking_scope.start && tracking_scope.start>=m_next_profile_activity) {
        m_next_profile_activity=tracking_scope.start+1000;
        auto& trace=PerformanceTrace::get();
        trace.record("hmd_activity",tracking_scope.start,0,m_system->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd));
        trace.record("hmd_refresh_hz",tracking_scope.start,0,m_system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,vr::Prop_DisplayFrequency_Float));
    }
    vr::VREvent_t event{};
    bool reset{};
    while (m_system->PollNextEvent(&event,sizeof(event))) {
        if (event.eventType == vr::VREvent_Quit) { stop(); m_status = "SteamVR requested shutdown"; return std::nullopt; }
        reset |= event.eventType == vr::VREvent_StandingZeroPoseReset || event.eventType == vr::VREvent_SeatedZeroPoseReset;
    }
    if (reset) { m_status = "tracking origin changed; recenter pending"; return std::nullopt; }
    std::array<vr::TrackedDevicePose_t,vr::k_unMaxTrackedDeviceCount> poses{};
    m_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding,0,poses.data(),poses.size());
    const auto& head = poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!head.bDeviceIsConnected || !head.bPoseIsValid || head.eTrackingResult != vr::TrackingResult_Running_OK) {
        m_status = "tracking lost; camera passive"; return std::nullopt;
    }
    Tracking out{};
    out.head = openvr_conversion::from_matrix(head.mDeviceToAbsoluteTracking);
    out.worn = openvr_conversion::worn(m_system->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd));
    const std::array<vr::TrackedDeviceIndex_t,2> roles{
        m_system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand),
        m_system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand)};
    out.controllers = openvr_conversion::controllers(poses,roles);
    m_finger_status={"no tracked wrist or input focus","no tracked wrist or input focus"};
    if (m_input && m_actions && m_system->IsInputAvailable() && !vr::VROverlay()->IsDashboardVisible()) {
        vr::VRActiveActionSet_t active{};active.ulActionSet=m_actions;
        if (m_input->UpdateActionState(&active,sizeof(active),1)==vr::VRInputError_None) {
            for(size_t hand=0;hand<2;++hand) if(out.controllers[hand] && m_finger_actions[hand]) {
                vr::InputSkeletalActionData_t action{};vr::InputOriginInfo_t origin{};vr::VRSkeletalSummaryData_t summary{};
                auto error=m_input->GetSkeletalActionData(m_finger_actions[hand],&action,sizeof(action));
                if(error!=vr::VRInputError_None || !action.bActive) { m_finger_status[hand]="skeletal action inactive/error "+std::to_string(error);continue; }
                error=m_input->GetOriginTrackedDeviceInfo(action.activeOrigin,&origin,sizeof(origin));
                if(error!=vr::VRInputError_None || origin.trackedDeviceIndex!=roles[hand]) { m_finger_status[hand]="wrong skeletal device/error "+std::to_string(error);continue; }
                error=m_input->GetSkeletalSummaryData(m_finger_actions[hand],vr::VRSummaryType_FromDevice,&summary);
                if(error!=vr::VRInputError_None) { m_finger_status[hand]="device curl error "+std::to_string(error);continue; }
                out.fingers[hand]=openvr_conversion::fingers(summary,origin.trackedDeviceIndex,roles[hand]);
                m_finger_status[hand]=out.fingers[hand]?"device curls active":"invalid device curls";
            }
        }
    }
    for (int eye = 0; eye < 2; ++eye) {
        const auto vr_eye = static_cast<vr::EVREye>(eye);
        out.eyes[eye] = openvr_conversion::from_matrix(m_system->GetEyeToHeadTransform(vr_eye));
        float left{},right{},top{},bottom{};
        m_system->GetProjectionRaw(vr_eye,&left,&right,&top,&bottom);
        out.fov[eye] = openvr_conversion::tangents(left,right,top,bottom);
    }
    m_status = "OpenVR tracking active";
    return out;
}
bool OpenVrBackend::test_tracking_fallback_available() const {
    return m_system && m_compositor && m_status=="tracking lost; camera passive";
}
std::optional<VrBackend::Tracking> OpenVrBackend::test_tracking_seed() const {
    if(!test_tracking_fallback_available())return {};
    // Only the explicitly opted-in scripted-head decorator consumes this pose.
    // Optics come from the runtime even when no physical head pose is available.
    Tracking out{};out.head[13]=1.7f;
    for(int eye=0;eye<2;++eye){
        const auto index=static_cast<vr::EVREye>(eye);
        out.eyes[eye]=openvr_conversion::from_matrix(m_system->GetEyeToHeadTransform(index));
        float left{},right{},top{},bottom{};m_system->GetProjectionRaw(index,&left,&right,&top,&bottom);
        out.fov[eye]=openvr_conversion::tangents(left,right,top,bottom);
        if(!vr_camera::rigid(out.eyes[eye]) || !out.fov[eye].valid())return {};
    }
    return out;
}
void OpenVrBackend::update_hud() {
    if(!m_system)return;
    const auto now=std::chrono::steady_clock::now();if(now<m_next_hud)return;m_next_hud=now+std::chrono::milliseconds(100);
    auto* overlay=vr::VROverlay();if(!overlay)return;
    const auto data=VrCompactHud::get().data(now);
    if(!data){if(m_hud!=vr::k_ulOverlayHandleInvalid)overlay->HideOverlay(m_hud);return;}
    if(m_hud==vr::k_ulOverlayHandleInvalid && overlay->CreateOverlay("sm2vr.compact.status","SM2VR status",&m_hud)!=vr::VROverlayError_None)return;
    auto pixels=VrCompactHud::render(*data);const auto settings=VrCompactHud::get().settings().values.values;
    vr::HmdMatrix34_t transform{{{1,0,0,0},{0,1,0,settings[1]},{0,0,1,-settings[2]}}};
    overlay->SetOverlayTransformTrackedDeviceRelative(m_hud,vr::k_unTrackedDeviceIndex_Hmd,&transform);
    overlay->SetOverlayWidthInMeters(m_hud,settings[0]);
    if(overlay->SetOverlayRaw(m_hud,pixels.data(),VrCompactHud::WIDTH,VrCompactHud::HEIGHT,4)==vr::VROverlayError_None)overlay->ShowOverlay(m_hud);
}
bool OpenVrBackend::submit(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue) {
    PerformanceTrace::Scope pair_scope{"openvr_pair_cpu"};
    if (!m_system || !m_compositor || !queue) { return false; }
    // Runtime transfer work uses the queue that produced these shader resources,
    // on the game's device and the HMD's graphics adapter.
    const bool adapter_valid = m_validated_queue.accepts(queue,[this](ID3D12CommandQueue* candidate) {
    PerformanceTrace::Scope adapter_scope{"openvr_adapter_validation_cpu"};
    int adapter_index{-1}; m_system->GetDXGIOutputInfo(&adapter_index);
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (adapter_index < 0 || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapters1(adapter_index,&adapter)) || FAILED(candidate->GetDevice(IID_PPV_ARGS(&device)))) { return false; }
    DXGI_ADAPTER_DESC1 desc{}; if (FAILED(adapter->GetDesc1(&desc))) { return false; }
    const auto luid = device->GetAdapterLuid();
    if (luid.HighPart != desc.AdapterLuid.HighPart || luid.LowPart != desc.AdapterLuid.LowPart) { m_status = "game GPU differs from HMD GPU"; return false; }
    return true;
    });
    if (!adapter_valid) { return false; }
    std::array<vr::TrackedDevicePose_t,vr::k_unMaxTrackedDeviceCount> poses{};
    vr::EVRCompositorError error;
    {
        PerformanceTrace::Scope wait_scope{"openvr_wait_poses_cpu"};
        error = m_compositor->WaitGetPoses(poses.data(),poses.size(),nullptr,0);
    }
    if (error != vr::VRCompositorError_None) { m_validated_queue.reset(); m_status = "WaitGetPoses failed: " + std::to_string(error); return false; }
    for (const auto* image : {&pair.left,&pair.right}) {
        vr::D3D12TextureData_t data{image->texture(),queue,0};
        vr::VRTextureWithPose_t texture{};
        texture.handle = &data; texture.eType = vr::TextureType_DirectX12; texture.eColorSpace = vr::ColorSpace_Auto;
        texture.mDeviceToAbsoluteTracking = openvr_conversion::to_matrix(image->metadata.runtime_head);
        const vr::VRTextureBounds_t bounds{0,0,1,1};
        {
            PerformanceTrace::Scope submit_scope{image->metadata.eye?"openvr_submit_right_cpu":"openvr_submit_left_cpu"};
            error = m_compositor->Submit(static_cast<vr::EVREye>(image->metadata.eye),&texture,&bounds,vr::Submit_TextureWithPose);
        }
        if (error != vr::VRCompositorError_None) { m_validated_queue.reset(); m_status = "eye submission failed: " + std::to_string(error); return false; }
    }
    {
        PerformanceTrace::Scope handoff_scope{"openvr_handoff_cpu"};
        m_compositor->PostPresentHandoff();
    }
    auto& performance=PerformanceTrace::get();
    if(performance.enabled()) performance.record("pair_submitted",PerformanceTrace::now_ms(),0,
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-std::min(pair.left.metadata.pose_captured,pair.right.metadata.pose_captured)).count());
    return true;
}
void OpenVrBackend::stop() {
    m_validated_queue.reset();
    if(m_system && m_hud!=vr::k_ulOverlayHandleInvalid)if(auto* overlay=vr::VROverlay())overlay->DestroyOverlay(m_hud);
    m_hud=vr::k_ulOverlayHandleInvalid;m_next_hud={};VrCompactHud::get().allow(0,0);
    if (m_system) { vr::VR_Shutdown(); }
    m_system = nullptr; m_compositor = nullptr;
    m_input = nullptr; m_actions = {}; m_input_actions = {};m_finger_actions={};
    m_combat_actions={};m_finisher_action={};m_fire_touch_action={};
}

void OpenVrBackend::initialize_input() {
    char enabled[32]{}, manifest[32768]{};
    if (!GetEnvironmentVariableA("SM2VR_CONTROLLERS",enabled,sizeof(enabled)) || std::atoi(enabled) != 1) { return; }
    const auto length = GetEnvironmentVariableA("SM2VR_ACTION_MANIFEST",manifest,sizeof(manifest));
    if (!length || length >= sizeof(manifest) || !std::filesystem::path{manifest}.is_absolute()) {
        m_input_status = "absolute SM2VR_ACTION_MANIFEST path required"; return;
    }
    m_input = vr::VRInput();
    if (!m_input) { m_input_status = "SteamVR Input unavailable"; return; }
    auto error = m_input->SetActionManifestPath(manifest);
    if (error == vr::VRInputError_None) { error = m_input->GetActionSetHandle("/actions/sm2vr",&m_actions); }
    const char* names[]{"move","turn","confirm","interact","sprint","menu","recenter"};
    for (size_t i = 0; error == vr::VRInputError_None && i < m_input_actions.size(); ++i) {
        const auto path = std::string{"/actions/sm2vr/in/"}+names[i];
        error = m_input->GetActionHandle(path.c_str(),&m_input_actions[i]);
    }
    if (error != vr::VRInputError_None) {
        m_input_status = "SteamVR action initialization failed: "+std::to_string(error); m_actions = {}; return;
    }
    // Optional hand actions must never prevent the established locomotion bindings working.
    for(size_t hand=0;hand<2;++hand) {
        const auto path=hand==0?"/actions/sm2vr/in/left_fingers":"/actions/sm2vr/in/right_fingers";
        if(m_input->GetActionHandle(path,&m_finger_actions[hand])!=vr::VRInputError_None) { m_finger_actions[hand]={}; }
    }
    m_input_status = "waiting for Index stick bindings";
    for(size_t i=0;i<m_combat_actions.size();++i) {
        const auto path=std::string{"/actions/sm2vr/in/"}+openvr_combat::ANALOG[i];
        if(m_input->GetActionHandle(path.c_str(),&m_combat_actions[i])!=vr::VRInputError_None) m_combat_actions[i]={};
    }
    if(m_input->GetActionHandle("/actions/sm2vr/in/finisher",&m_finisher_action)!=vr::VRInputError_None) m_finisher_action={};
    if(m_input->GetActionHandle("/actions/sm2vr/in/fire_touch",&m_fire_touch_action)!=vr::VRInputError_None) m_fire_touch_action={};
}

std::optional<VrControllerSample> OpenVrBackend::controllers() {
    if (!m_system || !m_input || !m_actions) { return std::nullopt; }
    if (!m_system->IsInputAvailable() || vr::VROverlay()->IsDashboardVisible()) {
        m_input_status = "SteamVR input focus lost; controls neutral"; return std::nullopt;
    }
    std::array<vr::TrackedDevicePose_t,vr::k_unMaxTrackedDeviceCount> poses{};
    m_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding,0,poses.data(),poses.size());
    for (const auto role : {vr::TrackedControllerRole_LeftHand,vr::TrackedControllerRole_RightHand}) {
        const auto index = m_system->GetTrackedDeviceIndexForControllerRole(role);
        if (index >= poses.size() || !poses[index].bDeviceIsConnected || !poses[index].bPoseIsValid || poses[index].eTrackingResult != vr::TrackingResult_Running_OK) {
            m_input_status = "both controllers must be tracked; controls neutral"; return std::nullopt;
        }
    }
    vr::VRActiveActionSet_t active{}; active.ulActionSet = m_actions;
    auto error = m_input->UpdateActionState(&active,sizeof(active),1);
    vr::InputAnalogActionData_t move{},turn{};
    if (error == vr::VRInputError_None) { error = m_input->GetAnalogActionData(m_input_actions[0],&move,sizeof(move),vr::k_ulInvalidInputValueHandle); }
    if (error == vr::VRInputError_None) { error = m_input->GetAnalogActionData(m_input_actions[1],&turn,sizeof(turn),vr::k_ulInvalidInputValueHandle); }
    if (error != vr::VRInputError_None || !move.bActive || !turn.bActive) {
        m_input_status = "stick action data inactive/error: "+std::to_string(error); return std::nullopt;
    }
    std::array<bool,5> buttons{};
    for (size_t i = 0; i < buttons.size(); ++i) {
        vr::InputDigitalActionData_t button{};
        error = m_input->GetDigitalActionData(m_input_actions[i+2],&button,sizeof(button),vr::k_ulInvalidInputValueHandle);
        if (error != vr::VRInputError_None) { m_input_status = "button action error: "+std::to_string(error); return std::nullopt; }
        buttons[i] = button.bActive && button.bState;
    }
    VrControllerSample sample{move.x,move.y,turn.x,buttons[0],buttons[1],buttons[2],buttons[3],buttons[4]};
    sample.turn_y=turn.y; // Viewer mode flies up/down; first person ignores it.
    const bool combat=openvr_combat::poll(m_combat_actions,m_finisher_action,m_fire_touch_action,
        [this](auto handle,auto& data){return m_input->GetAnalogActionData(handle,&data,sizeof(data),vr::k_ulInvalidInputValueHandle);},
        [this](auto handle,auto& data){return m_input->GetDigitalActionData(handle,&data,sizeof(data),vr::k_ulInvalidInputValueHandle);},sample);
    m_input_status = combat ? "Index locomotion and combat actions active" : "Index locomotion active; some combat bindings unavailable";
    return sample;
}
bool OpenVrBackend::test_input_available() const {
    return m_system && m_compositor && m_system->IsInputAvailable() && vr::VROverlay() && !vr::VROverlay()->IsDashboardVisible();
}
