#include "OpenXrBackend.h"
#include "OpenXrConversions.h"
#include "OpenXrHands.h"
#include "OpenXrInput.h"
#include "PerformanceTrace.h"
#include "VrCompactHud.h"
#include <TlHelp32.h>
#include <share.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
bool steamvr_openxr_running() {
    wchar_t runtime[32768]{}; DWORD bytes=sizeof(runtime);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Khronos\\OpenXR\\1",L"ActiveRuntime",RRF_RT_REG_SZ,nullptr,runtime,&bytes)!=ERROR_SUCCESS) return false;
    std::wstring path{runtime}; std::transform(path.begin(),path.end(),path.begin(),::towlower);
    if(path.find(L"steamxr_win64.json")==std::wstring::npos) return false;
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry); bool found{};
    if(Process32FirstW(snapshot,&entry)) do {
        if(_wcsicmp(entry.szExeFile,L"vrserver.exe")==0) { found=true; break; }
    } while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot); return found;
}
bool environment_flag(const char* name) {
    char value[32]{}; return GetEnvironmentVariableA(name,value,sizeof(value)) && std::atoi(value)==1;
}
// Wide: the narrow read turns a character outside the code page into '?', and the file isn't found.
std::filesystem::path environment_path(const wchar_t* name) {
    std::wstring value(32768,L'\0'); const auto length=GetEnvironmentVariableW(name,value.data(),32768);
    if(!length || length>=32768) return {};
    value.resize(length); return value;
}
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; return b;
}
DXGI_FORMAT srgb(DXGI_FORMAT f) {
    if(f==DXGI_FORMAT_R8G8B8A8_UNORM) return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if(f==DXGI_FORMAT_B8G8R8A8_UNORM) return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    return f;
}
// Text both the frame thread (events, transfer) and the tracking/input callers write.
class SharedText {
public:
    SharedText()=default;
    SharedText(std::string text):m_text{std::move(text)} {}
    SharedText& operator=(std::string text) { std::lock_guard lock{m_mutex}; m_text=std::move(text); return *this; }
    SharedText& operator+=(const std::string& text) { std::lock_guard lock{m_mutex}; m_text+=text; return *this; }
    bool empty() const { std::lock_guard lock{m_mutex}; return m_text.empty(); }
    void clear() { std::lock_guard lock{m_mutex}; m_text.clear(); }
    std::string str() const { std::lock_guard lock{m_mutex}; return m_text; }
private:
    mutable std::mutex m_mutex; std::string m_text;
};
}

// Threads (VrSession's submit thread, docs/OPENXR_SUBMIT_THREAD.md): tick() and transfer() may run on
// one thread while tracking(), controllers(), rumble() and status() run on others. Fields the event
// loop writes and those callers read are atomic or SharedText; the swapchains, frame state and HUD
// belong to the frame side only, the action and hand state to the tracking side only.
struct OpenXrBackend::State {
    OpenXrDispatch api;
    SharedText error;
    XrInstance instance{}; XrSystemId system{}; XrSession session{};
    XrSpace stage{},view{};
    std::atomic<XrSessionState> session_state{XR_SESSION_STATE_UNKNOWN};
    std::atomic<bool> running{},terminal{},views_valid{};
    unsigned primes{};
    std::atomic<XrTime> origin_change{};
    std::atomic<XrDuration> display_period{};
    double next_profile{};
    PFN_xrGetD3D12GraphicsRequirementsKHR requirements{};
    PFN_xrConvertWin32PerformanceCounterToTimeKHR qpc_to_time{};
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
    // Own queue (the default; SM2VR_OPENXR_GAME_QUEUE=1 opts out): the session (so every runtime-side wait/signal)
    // and our copies use `queue`, a mod-owned direct queue; `game` is the game's queue the caller
    // passes. SteamVR's xrEndFrame otherwise holds the game queue ~13 ms and the next left eye's
    // depth-ready wait pays for it; with a loaded GPU xrBeginFrame then blocks the caller instead
    // (docs/OPENXR_OWN_QUEUE.md), which the submit thread takes off the render thread.
    // Each copy first makes our queue wait (GPU-side) for everything already on the game queue.
    ComPtr<ID3D12CommandQueue> game; ComPtr<ID3D12Fence> ordered; uint64_t ordered_value{};
    bool own_queue_unavailable{}; // Creating the own queue or its fence failed: the game's queue instead.
    ComPtr<ID3D12Fence> fence; uint64_t tick{};
    HANDLE fence_event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    struct Slot {
        ID3D12Resource* texture{}; // Runtime-owned; never Release it.
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        uint64_t pending{};
    };
    struct Chain {
        XrSwapchain handle{}; uint32_t width{},height{}; DXGI_FORMAT source_format{},format{};
        std::vector<Slot> images; bool acquired{},waited{}; uint32_t index{};
    };
    std::array<Chain,2> chains;
    // Controller input (SM2VR_CONTROLLERS=1). Failures here never stop rendering.
    bool controls{},hand_extension{},hand_source_extension{},motion_range_extension{};
    XrActionSet action_set{}; std::array<XrAction,openxr_input::COUNT> actions{}; XrAction hand_pose{},haptic{};
    std::array<XrPath,2> hand_paths{}; std::array<XrSpace,2> grips{};
    bool attached{}; std::atomic<bool> touch_layout{}; SharedText profile{"no interaction profile"}; std::string accepted_profiles;
    openxr_hands::GripOffset grip_offset{openxr_hands::GripOffset::index_render_model()};
    PFN_xrCreateHandTrackerEXT create_hand_tracker{}; PFN_xrDestroyHandTrackerEXT destroy_hand_tracker{};
    PFN_xrLocateHandJointsEXT locate_hand_joints{}; std::array<XrHandTrackerEXT,2> hand_trackers{};
    // Profiling and the scripted-head optics fallback.
    PFN_xrGetDisplayRefreshRateFB display_refresh{}; std::atomic<int> user_present{-1}; // -1 unknown, else 0/1.
    std::optional<bool> presence() const { const int v=user_present; return v<0?std::nullopt:std::optional<bool>{v!=0}; }
    std::atomic<bool> head_lost{}; std::optional<VrBackend::Tracking> optics;
    // Compact HUD: head-locked quad layer replacing the OpenVR overlay.
    Chain hud; ComPtr<ID3D12Resource> hud_upload; uint64_t hud_pending{}; bool hud_ready{},hud_visible{};
    std::chrono::steady_clock::time_point next_hud{}; std::unique_ptr<VrCompactHud::Pixels> hud_pixels;
    XrCompositionLayerQuad hud_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    explicit State(OpenXrDispatch dispatch):api{dispatch} {}
    FILE* grip_log{}; // SM2VR_OPENXR_GRIP_LOG: same CSV layout as scripts/openvr_pose_witness.cpp.
    ~State() { if(fence_event) CloseHandle(fence_event); if(grip_log) std::fclose(grip_log); }
    bool check(XrResult r,const char* call) {
        if(XR_SUCCEEDED(r)) return true;
        error=std::string{call}+" failed: "+std::to_string(r); return false;
    }
    bool wait_gpu(uint64_t value) {
        if(!value) return true;
        if(!fence || !fence_event || fence->GetCompletedValue()==UINT64_MAX) return false;
        if(fence->GetCompletedValue()>=value) return true;
        return SUCCEEDED(fence->SetEventOnCompletion(value,fence_event)) && WaitForSingleObject(fence_event,2000)==WAIT_OBJECT_0;
    }
    void events() {
        if(!instance || terminal) return;
        for(unsigned n=0;n<64;++n) {
            XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
            const auto result=api.PollEvent(instance,&buffer);
            if(result==XR_EVENT_UNAVAILABLE) break;
            if(!check(result,"xrPollEvent")) { terminal=true; break; }
            if(buffer.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { terminal=true; error="OpenXR instance loss pending"; break; }
            if(buffer.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                const auto& change=*reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&buffer);
                if(change.session==session && change.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_STAGE) origin_change=change.changeTime;
            }
            if(buffer.type==XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) { refresh_profile(); continue; }
            if(buffer.type==XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT) {
                const auto& change=*reinterpret_cast<const XrEventDataUserPresenceChangedEXT*>(&buffer);
                if(change.session==session) user_present=change.isUserPresent!=XR_FALSE?1:0;
                continue;
            }
            if(buffer.type!=XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
            const auto& change=*reinterpret_cast<const XrEventDataSessionStateChanged*>(&buffer);
            if(change.session!=session) continue;
            session_state=change.state;
            if(session_state==XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running=check(api.BeginSession(session,&begin),"xrBeginSession");
                if(!running) terminal=true;
            } else if(session_state==XR_SESSION_STATE_STOPPING && running) {
                check(api.EndSession(session),"xrEndSession"); running=false; views_valid=false; primes=0;
            } else if(session_state==XR_SESSION_STATE_EXITING || session_state==XR_SESSION_STATE_LOSS_PENDING) {
                terminal=true; error="OpenXR session exit/loss pending";
            }
        }
    }
    struct Frame {
        State& s; XrFrameState state{XR_TYPE_FRAME_STATE}; bool begun{};
        explicit Frame(State& owner):s{owner} {}
        bool begin() {
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            { PerformanceTrace::Scope scope{"openxr_wait_frame_cpu"};
              if(!s.check(s.api.WaitFrame(s.session,&wait,&state),"xrWaitFrame")) return false; }
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            { PerformanceTrace::Scope scope{"openxr_begin_frame_cpu"};
              if(!s.check(s.api.BeginFrame(s.session,&begin),"xrBeginFrame")) return false; }
            begun=true; s.display_period=state.predictedDisplayPeriod; return true;
        }
        bool end(std::span<const XrCompositionLayerBaseHeader* const> layers={}) {
            if(!begun) return false;
            begun=false;
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO}; end.displayTime=state.predictedDisplayTime;
            end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount=uint32_t(layers.size()); end.layers=layers.empty()?nullptr:layers.data();
            PerformanceTrace::Scope scope{"openxr_end_frame_cpu"};
            return s.check(s.api.EndFrame(s.session,&end),"xrEndFrame");
        }
        ~Frame() { if(begun) end(); } // Every successful Begin is balanced, including errors/skips.
    };
    // Own-queue mode: our next submission runs after all work already queued on the caller's queue
    // (the game queue, or the isolated bridge queue of -HandoffControl isolated).
    ID3D12CommandQueue* source{};
    bool after_game_work() {
        if(!game) return true;
        auto* producer=source?source:game.Get();
        if(FAILED(producer->Signal(ordered.Get(),++ordered_value)) || FAILED(queue->Wait(ordered.Get(),ordered_value))) { error="OpenXR own-queue ordering failed"; return false; }
        return true;
    }
    // Own-queue mode accepts any direct queue of the session's device as the caller.
    bool caller_queue(ID3D12CommandQueue* caller) const {
        if(!game) return caller==queue.Get();
        if(caller==game.Get()) return true;
        ComPtr<ID3D12Device> owner;
        return caller!=queue.Get() && caller->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT && SUCCEEDED(caller->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get()==device.Get();
    }
    bool make_chain(Chain& chain,const D3D12_RESOURCE_DESC& desc) {
        if(chain.handle && chain.width==desc.Width && chain.height==desc.Height && chain.source_format==desc.Format) return true;
        if(!wait_gpu(tick)) { error="OpenXR resize could not drain GPU transfers"; return false; }
        if(chain.handle) {
            if(chain.acquired) { error="OpenXR resize while swapchain acquired"; return false; }
            if(!check(api.DestroySwapchain(chain.handle),"xrDestroySwapchain")) return false;
            chain={};
        }
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1) {
            error="OpenXR requires a single-sample 2D retained eye"; return false;
        }
        uint32_t count{};
        if(!check(api.EnumerateSwapchainFormats(session,0,&count,nullptr),"xrEnumerateSwapchainFormats") || !count || count>256) return false;
        std::vector<int64_t> formats(count);
        if(!check(api.EnumerateSwapchainFormats(session,count,&count,formats.data()),"xrEnumerateSwapchainFormats")) return false;
        const auto preferred=srgb(desc.Format);
        auto format=std::ranges::find(formats,int64_t(preferred));
        if(format==formats.end()) format=std::ranges::find(formats,int64_t(desc.Format));
        if(format==formats.end()) { error="OpenXR runtime cannot copy this source colour format"; return false; }
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create.format=*format; chain.format=DXGI_FORMAT(*format); create.sampleCount=1; create.width=static_cast<uint32_t>(desc.Width); create.height=desc.Height;
        create.faceCount=1; create.arraySize=1; create.mipCount=1;
        if(!check(api.CreateSwapchain(session,&create,&chain.handle),"xrCreateSwapchain")) return false;
        chain.width=create.width; chain.height=create.height; chain.source_format=desc.Format;
        if(!check(api.EnumerateSwapchainImages(chain.handle,0,&count,nullptr),"xrEnumerateSwapchainImages") || !count || count>32) return false;
        std::vector<XrSwapchainImageD3D12KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        if(!check(api.EnumerateSwapchainImages(chain.handle,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"xrEnumerateSwapchainImages")) return false;
        chain.images.resize(count);
        for(size_t n=0;n<count;++n) {
            auto& slot=chain.images[n]; slot.texture=images[n].texture;
            if(!slot.texture || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.allocator))) ||
                FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(&slot.commands))) || FAILED(slot.commands->Close())) {
                error="OpenXR copy command allocation failed"; return false;
            }
        }
        return true;
    }
    bool acquire(Chain& chain) {
        PerformanceTrace::Scope scope{"openxr_acquire_wait_cpu"};
        if(!chain.acquired) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if(!check(api.AcquireSwapchainImage(chain.handle,&acquire,&chain.index),"xrAcquireSwapchainImage")) return false;
            chain.acquired=true;
            if(chain.index>=chain.images.size()) { error="OpenXR swapchain index out of bounds"; return false; }
        }
        if(chain.waited) return true;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout=100000000; // 100ms; retry held acquisition after timeout.
        const auto r=api.WaitSwapchainImage(chain.handle,&wait);
        if(r==XR_TIMEOUT_EXPIRED) return false;
        if(!check(r,"xrWaitSwapchainImage")) return false;
        chain.waited=true; return true;
    }
    bool release(Chain& chain) {
        if(!chain.acquired || !chain.waited) return true;
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if(!check(api.ReleaseSwapchainImage(chain.handle,&release),"xrReleaseSwapchainImage")) return false;
        chain.acquired=chain.waited=false; return true;
    }
    bool copy(const EyeTextureCapture::Pair& pair) {
        PerformanceTrace::Scope scope{"openxr_copy_enqueue_cpu"};
        const EyeTextureCapture::Image* sources[]{&pair.left,&pair.right};
        std::array<ID3D12CommandList*,2> lists{};
        for(size_t eye=0;eye<2;++eye) {
            auto& slot=chains[eye].images[chains[eye].index];
            if(!wait_gpu(slot.pending) || FAILED(slot.allocator->Reset()) || FAILED(slot.commands->Reset(slot.allocator.Get(),nullptr))) {
                error="OpenXR transfer allocator is not reusable"; return false;
            }
            auto* source=sources[eye]->texture();
            const D3D12_RESOURCE_BARRIER before[]{barrier(source,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE),
                barrier(slot.texture,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST)};
            slot.commands->ResourceBarrier(2,before); slot.commands->CopyResource(slot.texture,source);
            const D3D12_RESOURCE_BARRIER after[]{barrier(source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                barrier(slot.texture,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET)};
            slot.commands->ResourceBarrier(2,after);
            if(FAILED(slot.commands->Close())) { error="OpenXR transfer command close failed"; return false; }
            lists[eye]=slot.commands.Get();
        }
        if(!after_game_work()) return false;
        queue->ExecuteCommandLists(2,lists.data()); ++tick;
        for(auto& chain:chains) chain.images[chain.index].pending=tick;
        if(FAILED(queue->Signal(fence.Get(),tick))) { error="OpenXR transfer fence signal failed"; return false; }
        return true;
    }
    void refresh_profile() {
        profile="no interaction profile";touch_layout=false;
        if(!attached) return;
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        if(XR_FAILED(api.GetCurrentInteractionProfile(session,hand_paths[1],&state)) || state.interactionProfile==XR_NULL_PATH) return;
        char text[XR_MAX_PATH_LENGTH]{}; uint32_t length{};
        if(XR_FAILED(api.PathToString(instance,state.interactionProfile,sizeof(text),&length,text))) return;
        const auto* known=openxr_input::profile(text);
        profile=known?known->name:std::string{"unsupported profile "}+text;
        touch_layout=known && known->touch_layout;
    }
    // The SteamVR manifest's actions, suggested for every supported controller. Returns an
    // empty string on success, else the reason controls stay neutral.
    std::string initialize_input() {
        const auto path=[&](const char* text,XrPath& out){return XR_SUCCEEDED(api.StringToPath(instance,text,&out));};
        for(size_t hand=0;hand<2;++hand) if(!path(openxr_input::HANDS[hand],hand_paths[hand])) return "OpenXR hand paths unavailable";
        XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(set.actionSetName,"sm2vr"); std::strcpy(set.localizedActionSetName,"SM2VR");
        if(XR_FAILED(api.CreateActionSet(instance,&set,&action_set))) return "OpenXR action set creation failed";
        for(size_t n=0;n<openxr_input::COUNT;++n) {
            const auto& definition=openxr_input::ACTIONS[n];
            XrActionCreateInfo create{XR_TYPE_ACTION_CREATE_INFO};
            create.actionType=definition.kind==openxr_input::Kind::boolean?XR_ACTION_TYPE_BOOLEAN_INPUT:
                definition.kind==openxr_input::Kind::floating?XR_ACTION_TYPE_FLOAT_INPUT:XR_ACTION_TYPE_VECTOR2F_INPUT;
            std::strcpy(create.actionName,definition.name); std::strcpy(create.localizedActionName,definition.name);
            if(XR_FAILED(api.CreateAction(action_set,&create,&actions[n]))) return std::string{"OpenXR action creation failed: "}+definition.name;
        }
        XrActionCreateInfo pose{XR_TYPE_ACTION_CREATE_INFO}; pose.actionType=XR_ACTION_TYPE_POSE_INPUT;
        std::strcpy(pose.actionName,"hand_pose"); std::strcpy(pose.localizedActionName,"hand_pose");
        pose.countSubactionPaths=2; pose.subactionPaths=hand_paths.data();
        if(XR_FAILED(api.CreateAction(action_set,&pose,&hand_pose))) return "OpenXR pose action creation failed";
        // Game rumble (docs/HAPTICS.md). Kept out of openxr_input: the SteamVR manifest has no output action.
        XrActionCreateInfo vibration{XR_TYPE_ACTION_CREATE_INFO}; vibration.actionType=XR_ACTION_TYPE_VIBRATION_OUTPUT;
        std::strcpy(vibration.actionName,"haptic"); std::strcpy(vibration.localizedActionName,"haptic");
        vibration.countSubactionPaths=2; vibration.subactionPaths=hand_paths.data();
        if(XR_FAILED(api.CreateAction(action_set,&vibration,&haptic))) return "OpenXR haptic action creation failed";
        unsigned accepted{};
        // Left-handed play swaps the bindings here and nowhere else (docs/LEFT_HANDED.md): poses and haptics stay physical.
        const vr_hand::Settings hand{environment_flag("SM2VR_LEFT_HANDED"),environment_flag("SM2VR_LEFT_HANDED_KEEP_STICKS")};
        for(const auto& definition:openxr_input::PROFILES) {
            std::vector<XrActionSuggestedBinding> bindings; XrPath profile_path{},binding{};
            bool valid=path(definition.path,profile_path);
            for(const auto& b:openxr_input::bindings(definition,hand)) { valid=valid && path(b.path.c_str(),binding); bindings.push_back({actions[b.action],binding}); }
            for(const auto* grip:{"/user/hand/left/input/grip/pose","/user/hand/right/input/grip/pose"}) { valid=valid && path(grip,binding); bindings.push_back({hand_pose,binding}); }
            for(const auto* motor:{"/user/hand/left/output/haptic","/user/hand/right/output/haptic"}) { valid=valid && path(motor,binding); bindings.push_back({haptic,binding}); }
            XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested.interactionProfile=profile_path; suggested.countSuggestedBindings=uint32_t(bindings.size()); suggested.suggestedBindings=bindings.data();
            if(valid && XR_SUCCEEDED(api.SuggestInteractionProfileBindings(instance,&suggested))) { ++accepted; accepted_profiles+=(accepted_profiles.empty()?"":",")+std::string{definition.name}; }
        }
        if(!accepted) return "OpenXR runtime rejected every suggested controller binding";
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; attach.countActionSets=1; attach.actionSets=&action_set;
        if(XR_FAILED(api.AttachSessionActionSets(session,&attach))) return "OpenXR action set attach failed";
        attached=true;
        for(size_t hand=0;hand<2;++hand) {
            XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO}; space.action=hand_pose; space.subactionPath=hand_paths[hand];
            space.poseInActionSpace.orientation.w=1;
            if(XR_FAILED(api.CreateActionSpace(session,&space,&grips[hand]))) return "OpenXR grip space creation failed";
        }
        if(hand_extension && create_hand_tracker && locate_hand_joints && destroy_hand_tracker) for(size_t hand=0;hand<2;++hand) {
            XrHandTrackingDataSourceEXT sources[]{XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT,XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT};
            XrHandTrackingDataSourceInfoEXT source{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT}; source.requestedDataSourceCount=2; source.requestedDataSources=sources;
            XrHandTrackerCreateInfoEXT create{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            create.hand=hand?XR_HAND_RIGHT_EXT:XR_HAND_LEFT_EXT; create.handJointSet=XR_HAND_JOINT_SET_DEFAULT_EXT;
            if(hand_source_extension) create.next=&source;
            if(XR_FAILED(create_hand_tracker(session,&create,&hand_trackers[hand]))) hand_trackers[hand]=XR_NULL_HANDLE;
        }
        return {};
    }
    std::optional<XrTime> now() {
        LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter); XrTime time{};
        if(!qpc_to_time || XR_FAILED(qpc_to_time(instance,&counter,&time))) return {};
        return time;
    }
    bool focused() const { return running && !terminal && attached && session_state==XR_SESSION_STATE_FOCUSED; }
    // Raw-equivalent OpenVR controller origin, only while position and orientation are both
    // valid and actively tracked (the OpenVR TrackingResult_Running_OK requirement).
    std::optional<vr_camera::Matrix> controller(size_t hand,XrTime time,int64_t log_qpc=0) {
        if(!grips[hand]) return {};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if(XR_FAILED(api.LocateSpace(grips[hand],stage,time,&location))) return {};
        constexpr auto flags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|
            XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        if((location.locationFlags&flags)!=flags) return {};
        const auto grip=openxr_conversion::matrix(location.pose); if(!grip) return {};
        if(grip_log && log_qpc) { // Phase 0 fit input: the grip before conversion, at this QPC.
            std::fprintf(grip_log,"%lld,%zu",static_cast<long long>(log_qpc),hand);
            for(float v:*grip) std::fprintf(grip_log,",%.9g",v);
            std::fputc('\n',grip_log);
        }
        return grip_offset.raw(hand,*grip);
    }
    std::optional<vr_finger::Curl> fingers(size_t hand,XrTime time,std::string& status) {
        if(!hand_trackers[hand]) { status=hand_extension?"hand tracker unavailable":"XR_EXT_hand_tracking unavailable"; return {}; }
        std::array<XrHandJointLocationEXT,XR_HAND_JOINT_COUNT_EXT> joints{};
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT}; locations.jointCount=uint32_t(joints.size()); locations.jointLocations=joints.data();
        XrHandJointsMotionRangeInfoEXT range{XR_TYPE_HAND_JOINTS_MOTION_RANGE_INFO_EXT};
        range.handJointsMotionRange=XR_HAND_JOINTS_MOTION_RANGE_UNOBSTRUCTED_EXT; // OpenVR VRSummaryType_FromDevice.
        XrHandJointsLocateInfoEXT locate{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT}; locate.baseSpace=stage; locate.time=time;
        if(motion_range_extension) locate.next=&range;
        const auto result=locate_hand_joints(hand_trackers[hand],&locate,&locations);
        if(XR_FAILED(result) || !locations.isActive) { status="hand joints inactive/error "+std::to_string(result); return {}; }
        const auto curls=openxr_hands::curls(joints);
        status=curls?"hand joint curls active":"invalid hand joint curls";
        return curls;
    }
    bool sync_actions() {
        if(!focused()) return false;
        XrActiveActionSet active{action_set,XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO}; sync.countActiveActionSets=1; sync.activeActionSets=&active;
        return api.SyncActions(session,&sync)==XR_SUCCESS; // Fails with XR_SESSION_NOT_FOCUSED too.
    }
    std::optional<openxr_input::Values> read_actions() {
        if(!sync_actions()) return {};
        openxr_input::Values values{};
        for(size_t n=0;n<openxr_input::COUNT;++n) {
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.action=actions[n];
            auto& value=values[n];
            switch(openxr_input::ACTIONS[n].kind) {
            case openxr_input::Kind::boolean: { XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                if(XR_FAILED(api.GetActionStateBoolean(session,&get,&state))) return {};
                value={state.isActive!=XR_FALSE,state.currentState?1.0f:0.0f}; break; }
            case openxr_input::Kind::floating: { XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
                if(XR_FAILED(api.GetActionStateFloat(session,&get,&state))) return {};
                value={state.isActive!=XR_FALSE,state.currentState}; break; }
            case openxr_input::Kind::vector2: { XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
                if(XR_FAILED(api.GetActionStateVector2f(session,&get,&state))) return {};
                value={state.isActive!=XR_FALSE,state.currentState.x,state.currentState.y}; break; }
            }
        }
        return values;
    }
    // Uploads only changed pixels, at the OpenVR overlay's 100 ms cadence. Unlike an overlay,
    // a quad layer is only visible on submitted frames.
    const XrCompositionLayerBaseHeader* compact_hud(std::chrono::steady_clock::time_point time) {
        if(time>=next_hud) {
            next_hud=time+std::chrono::milliseconds(100);
            const auto data=VrCompactHud::get().data(time);
            hud_visible=false;
            if(data) {
                auto pixels=std::make_unique<VrCompactHud::Pixels>(VrCompactHud::render(*data));
                if(hud_ready && hud_pixels && *hud_pixels==*pixels) hud_visible=true;
                else if(upload_hud(*pixels)) { hud_pixels=std::move(pixels); hud_visible=true; }
            }
        }
        if(!hud_visible || !hud_ready) return nullptr;
        const auto settings=VrCompactHud::get().settings().values.values;
        hud_layer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        hud_layer.space=view; hud_layer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        hud_layer.subImage.swapchain=hud.handle; hud_layer.subImage.imageRect.extent={int32_t(hud.width),int32_t(hud.height)};
        hud_layer.pose.orientation={0,0,0,1}; hud_layer.pose.position={0,settings[1],-settings[2]};
        hud_layer.size={settings[0],settings[0]*float(VrCompactHud::HEIGHT)/float(VrCompactHud::WIDTH)};
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud_layer);
    }
    bool upload_hud(const VrCompactHud::Pixels& pixels) {
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=VrCompactHud::WIDTH; desc.Height=VrCompactHud::HEIGHT;
        desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        const auto saved=error.str(); // The HUD is optional: its failures never poison the session.
        const auto failed=[&]{ error=saved; return false; };
        if(!make_chain(hud,desc)) return failed();
        constexpr UINT pitch=VrCompactHud::WIDTH*4; // 2560: already 256-byte aligned.
        if(!hud_upload) {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_UPLOAD; heap.CreationNodeMask=heap.VisibleNodeMask=1;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=UINT64(pitch)*VrCompactHud::HEIGHT;
            buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&hud_upload)))) return failed();
        }
        if(!wait_gpu(hud_pending)) return failed();
        void* mapped{}; if(FAILED(hud_upload->Map(0,nullptr,&mapped))) return failed();
        std::memcpy(mapped,pixels.data(),pixels.size()); hud_upload->Unmap(0,nullptr);
        if(!acquire(hud)) return failed(); // Timeout keeps the previous image; the acquisition is retried.
        auto& slot=hud.images[hud.index];
        if(!wait_gpu(slot.pending) || FAILED(slot.allocator->Reset()) || FAILED(slot.commands->Reset(slot.allocator.Get(),nullptr))) { release(hud); return failed(); }
        const auto before=barrier(slot.texture,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
        slot.commands->ResourceBarrier(1,&before);
        D3D12_TEXTURE_COPY_LOCATION target{slot.texture,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}}; target.SubresourceIndex=0;
        D3D12_TEXTURE_COPY_LOCATION source{hud_upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};
        source.PlacedFootprint.Footprint={hud.format,VrCompactHud::WIDTH,VrCompactHud::HEIGHT,1,pitch};
        slot.commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        const auto after=barrier(slot.texture,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
        slot.commands->ResourceBarrier(1,&after);
        if(FAILED(slot.commands->Close())) { release(hud); return failed(); }
        ID3D12CommandList* list=slot.commands.Get(); queue->ExecuteCommandLists(1,&list); ++tick;
        slot.pending=hud_pending=tick;
        if(FAILED(queue->Signal(fence.Get(),tick)) || !release(hud)) return failed();
        hud_ready=true; return true;
    }
    bool destroy() {
        if(!wait_gpu(tick)) return false; // Preserve GPU resources AND runtime handles on uncertain completion.
        Chain* all[]{&chains[0],&chains[1],&hud};
        for(auto* chain:all) if(chain->acquired) {
            if(!acquire(*chain) || !release(*chain)) return false;
        }
        for(auto* chain:all) if(chain->handle) api.DestroySwapchain(chain->handle);
        chains={}; hud={}; hud_ready=hud_visible=false;
        for(auto& tracker:hand_trackers) if(tracker && destroy_hand_tracker) destroy_hand_tracker(tracker);
        hand_trackers={};
        for(auto& grip:grips) if(grip) api.DestroySpace(grip);
        grips={};
        if(action_set) api.DestroyActionSet(action_set);
        action_set=XR_NULL_HANDLE; actions={}; hand_pose=haptic=XR_NULL_HANDLE; attached=false;
        if(view) api.DestroySpace(view); if(stage) api.DestroySpace(stage);
        if(session) api.DestroySession(session);
        if(instance) api.DestroyInstance(instance);
        view=stage=XR_NULL_HANDLE; session=XR_NULL_HANDLE; instance=XR_NULL_HANDLE; return true;
    }
};

HRESULT OpenXrBackend::create_queue(ID3D12Device* device,const D3D12_COMMAND_QUEUE_DESC& desc,ComPtr<ID3D12CommandQueue>& queue) {
    return device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue));
}
OpenXrBackend::OpenXrBackend():OpenXrBackend(OpenXrDispatch{},steamvr_openxr_running) {}
OpenXrBackend::OpenXrBackend(OpenXrDispatch api,Availability available,QueueFactory create_queue):m_api{api},m_available{available},m_create_queue{create_queue} {}
OpenXrBackend::~OpenXrBackend() { stop(); }
bool OpenXrBackend::available() { return m_available && m_available(); }
bool OpenXrBackend::prepare_graphics(ID3D12CommandQueue* queue) {
    if(!queue || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    if(!m) m=std::make_unique<State>(m_api);
    if(m->session) return (m->game?m->game:m->queue).Get()==queue;
    if(FAILED(queue->GetDevice(IID_PPV_ARGS(&m->device))) || m->device->GetNodeCount()!=1) return false;
    m->queue=queue;
    if(!environment_flag("SM2VR_OPENXR_GAME_QUEUE")) {
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT; ComPtr<ID3D12CommandQueue> own; ComPtr<ID3D12Fence> ordered;
        if(SUCCEEDED(m_create_queue(m->device.Get(),desc,own)) && SUCCEEDED(m->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ordered)))) {
            own->SetName(L"SM2VR OpenXR runtime queue"); m->game=queue; m->queue=own; m->ordered=ordered;
        } else m->own_queue_unavailable=true;
    }
    return true;
}
bool OpenXrBackend::start() {
    if(!m || !m->device || !m->queue) { m_status="OpenXR requires the game's graphics queue"; return false; }
    auto& s=*m;
    const auto failed=[this] { m_status=m->error.str(); stop(); return false; };
    const char* requested[]{XR_KHR_D3D12_ENABLE_EXTENSION_NAME,XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
    uint32_t count{};
    if(!s.check(s.api.EnumerateInstanceExtensionProperties(nullptr,0,&count,nullptr),"xrEnumerateInstanceExtensionProperties") || count>1024) return failed();
    std::vector<XrExtensionProperties> extensions(count,{XR_TYPE_EXTENSION_PROPERTIES});
    if(!s.check(s.api.EnumerateInstanceExtensionProperties(nullptr,count,&count,extensions.data()),"xrEnumerateInstanceExtensionProperties")) return failed();
    const auto supported=[&](const char* name){return std::ranges::any_of(extensions,[&](const auto& e){return std::strcmp(e.extensionName,name)==0;});};
    for(const auto* name:requested) if(!supported(name)) {
        s.error=std::string{"OpenXR missing required extension: "}+name; return failed();
    }
    std::vector<const char*> enabled{std::begin(requested),std::end(requested)};
    const auto optional=[&](const char* name){const bool on=supported(name);if(on) enabled.push_back(name);return on;};
    s.controls=environment_flag("SM2VR_CONTROLLERS");
    m_input_status=s.controls?"waiting for OpenXR controller bindings":"controller input disabled";
    if(s.controls) {
        // Raw-origin conversion: the render-model candidate unless a calibration file is named.
        if(const auto path=environment_path(L"SM2VR_OPENXR_GRIP_CALIBRATION"); !path.empty()) {
            std::ifstream input{path};
            const auto offset=path.is_absolute()?openxr_hands::GripOffset::read(input):std::nullopt;
            if(offset) s.grip_offset=*offset;
            else { s.controls=false; m_input_status="invalid SM2VR_OPENXR_GRIP_CALIBRATION; controls disabled"; }
        }
        if(const auto log=environment_path(L"SM2VR_OPENXR_GRIP_LOG"); !log.empty() && log.is_absolute() &&
           (s.grip_log=_wfsopen(log.c_str(),L"w",_SH_DENYWR))) { // Readable while the game runs.
            std::fprintf(s.grip_log,"qpc,hand");for(int n=0;n<16;++n) std::fprintf(s.grip_log,",m%d",n);std::fputc('\n',s.grip_log);
        }
        s.hand_extension=s.controls && optional(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        s.hand_source_extension=s.hand_extension && optional(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
        s.motion_range_extension=s.hand_extension && optional(XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME);
    }
    optional(XR_EXT_USER_PRESENCE_EXTENSION_NAME); // Presence then arrives as events.
    const bool refresh=optional(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(create.applicationInfo.applicationName,"SM2VR OpenXR"); std::strcpy(create.applicationInfo.engineName,"sm2vr");
    create.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0); create.enabledExtensionCount=uint32_t(enabled.size()); create.enabledExtensionNames=enabled.data();
    if(!s.check(s.api.CreateInstance(&create,&s.instance),"xrCreateInstance")) return failed();
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if(!s.check(s.api.GetInstanceProperties(s.instance,&properties),"xrGetInstanceProperties")) return failed();
    s.error=std::string{"OpenXR runtime: "}+properties.runtimeName;
    XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO}; get.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if(!s.check(s.api.GetSystem(s.instance,&get,&s.system),"xrGetSystem") ||
       !s.check(s.api.GetInstanceProcAddr(s.instance,"xrGetD3D12GraphicsRequirementsKHR",reinterpret_cast<PFN_xrVoidFunction*>(&s.requirements)),"xrGetInstanceProcAddr") || !s.requirements ||
       !s.check(s.api.GetInstanceProcAddr(s.instance,"xrConvertWin32PerformanceCounterToTimeKHR",reinterpret_cast<PFN_xrVoidFunction*>(&s.qpc_to_time)),"xrGetInstanceProcAddr") || !s.qpc_to_time) return failed();
    const auto lookup=[&](const char* name,auto& out){
        if(XR_FAILED(s.api.GetInstanceProcAddr(s.instance,name,reinterpret_cast<PFN_xrVoidFunction*>(&out)))) out=nullptr;
    };
    if(s.hand_extension) {
        lookup("xrCreateHandTrackerEXT",s.create_hand_tracker); lookup("xrDestroyHandTrackerEXT",s.destroy_hand_tracker); lookup("xrLocateHandJointsEXT",s.locate_hand_joints);
    }
    if(refresh) lookup("xrGetDisplayRefreshRateFB",s.display_refresh);
    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if(!s.check(s.requirements(s.instance,s.system,&requirements),"xrGetD3D12GraphicsRequirementsKHR")) return failed();
    const auto luid=s.device->GetAdapterLuid();
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{1,&requirements.minFeatureLevel,{}};
    if(luid.HighPart!=requirements.adapterLuid.HighPart || luid.LowPart!=requirements.adapterLuid.LowPart ||
       FAILED(s.device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,&levels,sizeof(levels)))) {
        s.error="OpenXR HMD adapter/feature level differs from game device"; return failed();
    }
    std::array<XrViewConfigurationView,2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
    if(!s.check(s.api.EnumerateViewConfigurationViews(s.instance,s.system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views.data()),"xrEnumerateViewConfigurationViews") || count!=2) return failed();
    // Eyes still render at the game's output size; the runtime's size is the target for docs/EYE_SIZE.md.
    s.error+="; recommended eye "+std::to_string(views[0].recommendedImageRectWidth)+"x"+std::to_string(views[0].recommendedImageRectHeight)+
        " (max "+std::to_string(views[0].maxImageRectWidth)+"x"+std::to_string(views[0].maxImageRectHeight)+")";
    if(s.game) s.error+="; runtime on the mod's own queue";
    else if(s.own_queue_unavailable) s.error+="; runtime on the game's queue (own queue unavailable)";
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR}; binding.device=s.device.Get(); binding.queue=s.queue.Get();
    XrSessionCreateInfo session{XR_TYPE_SESSION_CREATE_INFO}; session.next=&binding; session.systemId=s.system;
    if(!s.check(s.api.CreateSession(s.instance,&session,&s.session),"xrCreateSession")) return failed();
    if(!s.check(s.api.EnumerateReferenceSpaces(s.session,0,&count,nullptr),"xrEnumerateReferenceSpaces") || count>64) return failed();
    std::vector<XrReferenceSpaceType> spaces(count);
    if(!s.check(s.api.EnumerateReferenceSpaces(s.session,count,&count,spaces.data()),"xrEnumerateReferenceSpaces") ||
       std::ranges::find(spaces,XR_REFERENCE_SPACE_TYPE_STAGE)==spaces.end()) { s.error="OpenXR standing STAGE space unavailable"; return failed(); }
    XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; reference.poseInReferenceSpace.orientation.w=1;
    reference.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_STAGE;
    if(!s.check(s.api.CreateReferenceSpace(s.session,&reference,&s.stage),"xrCreateReferenceSpace(STAGE)")) return failed();
    reference.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;
    if(!s.check(s.api.CreateReferenceSpace(s.session,&reference,&s.view),"xrCreateReferenceSpace(VIEW)")) return failed();
    if(FAILED(s.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence))) || !s.fence_event) { s.error="OpenXR fence creation failed"; return failed(); }
    if(s.controls) {
        // Bindings must be suggested and attached before xrBeginSession. Like OpenVR, a
        // failure leaves rendering running with neutral controls.
        const auto problem=s.initialize_input();
        const bool hands=std::ranges::any_of(s.hand_trackers,[](auto t){return t!=XR_NULL_HANDLE;});
        m_input_bindings=s.accepted_profiles;
        m_input_status=problem.empty()?"waiting for OpenXR controller focus; bindings "+s.accepted_profiles+"; hand tracking "+(hands?"on":"off")+
            (s.grip_offset.measured?"":"; grip offset unmeasured (render-model candidate)"):problem;
    }
    m_status=s.error.str()+"; waiting for session readiness"; s.error.clear(); return true;
}
void OpenXrBackend::tick() {
    if(!m || !m->session) return;
    m->events();
    // Prime session visibility on the Present thread, never blocking a camera worker.
    if(m->running && !m->terminal && !m->views_valid && m->primes<8) {
        ++m->primes; State::Frame frame{*m}; if(frame.begin()) frame.end();
    }
}
std::optional<VrBackend::Tracking> OpenXrBackend::tracking() {
    PerformanceTrace::Scope scope{"openxr_tracking_cpu"};
    if(!m || !m->running || m->terminal) return {};
    auto& s=*m; LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter); XrTime time{};
    if(!s.check(s.qpc_to_time(s.instance,&counter,&time),"xrConvertWin32PerformanceCounterToTimeKHR")) return {};
    // An origin change never qualifies for the scripted-head optics fallback.
    if(s.origin_change && time>=s.origin_change) { s.origin_change=0; s.views_valid=false; s.head_lost=false; return {}; }
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if(!s.check(s.api.LocateSpace(s.view,s.stage,time,&head),"xrLocateSpace")) return {};
    constexpr auto flags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if((head.locationFlags&flags)!=flags) { s.views_valid=false; s.head_lost=true; m_status="OpenXR head tracking invalid"; return {}; }
    const auto head_matrix=openxr_conversion::matrix(head.pose); if(!head_matrix) return {};
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO}; locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; locate.displayTime=time; locate.space=s.view;
    XrViewState state{XR_TYPE_VIEW_STATE}; uint32_t count{};
    std::array<XrView,2> views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    if(!s.check(s.api.LocateViews(s.session,&locate,&state,2,&count,views.data()),"xrLocateViews") || count!=2 ||
       (state.viewStateFlags&(XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT))!=(XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return {};
    Tracking out{}; out.head=*head_matrix; out.worn=openxr_conversion::worn(s.presence(),s.session_state);
    for(size_t eye=0;eye<2;++eye) {
        const auto offset=openxr_conversion::matrix(views[eye].pose); if(!offset) return {};
        out.eyes[eye]=*offset; out.fov[eye]=openxr_conversion::tangents(views[eye].fov);
        if(!out.fov[eye].valid()) return {};
    }
    s.views_valid=true; s.head_lost=false;
    // Rebuilt only when it changes: one heap string per frame otherwise.
    if(const auto key=std::pair{int(s.session_state),bool(s.game)};!m_status_key || *m_status_key!=key || !m_status.starts_with("OpenXR tracking active")) {
        m_status_key=key;m_status="OpenXR tracking active; session state="+std::to_string(s.session_state)+(s.game?"; queue=own":"; queue=game");
    }
    s.optics=out; s.optics->head=vr_camera::IDENTITY; s.optics->head[13]=1.7f;
    if(s.attached) {
        // Poses and curls at the head's time; curls need input focus like OpenVR's skeleton.
        // Action spaces only locate once xrSyncActions has made the pose action active, so
        // sync first; unfocused sessions get no controller poses at all. Only the sync: the action
        // values are read by controllers(), not here (MOD_REVIEW_2026-10-29).
        if(s.focused()) s.sync_actions();
        for(size_t hand=0;hand<2;++hand) out.controllers[hand]=s.controller(hand,time,counter.QuadPart);
        if(s.grip_log) std::fflush(s.grip_log); // The launcher kills the game: never rely on fclose.
        const bool focus=s.focused();
        for(size_t hand=0;hand<2;++hand) {
            if(!out.controllers[hand] || !focus) { m_finger_status[hand]="no tracked wrist or input focus"; continue; }
            out.fingers[hand]=s.fingers(hand,time,m_finger_status[hand]);
        }
    }
    if(scope.start && scope.start>=s.next_profile) {
        s.next_profile=scope.start+1000; auto& trace=PerformanceTrace::get();
        trace.record("xr_session_state",scope.start,0,s.session_state);
        if(s.display_period>0) trace.record("xr_predicted_refresh_hz",scope.start,0,1e9/double(s.display_period));
        float hz{};
        if(s.display_refresh && XR_SUCCEEDED(s.display_refresh(s.session,&hz)) && hz>0) trace.record("hmd_refresh_hz",scope.start,0,hz);
        if(const auto present=s.presence()) trace.record("xr_user_present",scope.start,0,*present);
    }
    return out;
}
bool OpenXrBackend::submit(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue) { return transfer(pair,queue)==Transfer::accepted; }
VrBackend::Transfer OpenXrBackend::transfer(const EyeTextureCapture::Pair& pair,ID3D12CommandQueue* queue) {
    PerformanceTrace::Scope scope{"openxr_pair_cpu"};
    if(!m || m->terminal || !queue || !m->caller_queue(queue)) return Transfer::failed;
    m->source=queue;
    auto& s=*m;
    if(!s.running) return Transfer::skipped;
    State::Frame frame{s}; if(!frame.begin()) return Transfer::failed;
    auto& trace=PerformanceTrace::get();
    if(trace.enabled()) trace.record("xr_should_render",PerformanceTrace::now_ms(),0,frame.state.shouldRender);
    if(!frame.state.shouldRender) return frame.end()?Transfer::skipped:Transfer::failed;
    const EyeTextureCapture::Image* images[]{&pair.left,&pair.right};
    std::array<XrCompositionLayerProjectionView,2> views{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    for(size_t eye=0;eye<2;++eye) {
        if(!images[eye]->texture() || images[eye]->metadata.eye!=int(eye)) { s.error="OpenXR source eye identity invalid"; return Transfer::failed; }
        const auto& meta=images[eye]->metadata;
        const auto pose=openxr_conversion::pose(vr_camera::multiply(meta.runtime_eye_to_head,meta.runtime_head));
        const auto fov=openxr_conversion::fov(meta.projection);
        if(!pose || !fov || !s.make_chain(s.chains[eye],images[eye]->texture()->GetDesc())) return Transfer::failed;
        views[eye].pose=*pose; views[eye].fov=*fov;
    }
    bool ready=true;
    for(auto& chain:s.chains) if(!s.acquire(chain)) { ready=false; break; }
    if(!ready) {
        const bool fatal=!s.error.empty();
        for(auto& chain:s.chains) if(!s.release(chain)) return Transfer::failed;
        if(!frame.end()) return Transfer::failed;
        return fatal?Transfer::failed:Transfer::skipped;
    }
    if(!s.copy(pair)) return Transfer::failed;
    for(size_t eye=0;eye<2;++eye) {
        auto& chain=s.chains[eye]; if(!s.release(chain)) return Transfer::failed;
        views[eye].subImage.swapchain=chain.handle;
        views[eye].subImage.imageRect.extent={int32_t(chain.width),int32_t(chain.height)};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space=s.stage; layer.viewCount=2; layer.views=views.data();
    std::array<const XrCompositionLayerBaseHeader*,2> layers{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    const auto* hud=s.compact_hud(std::chrono::steady_clock::now());
    if(hud) layers[1]=hud;
    if(!frame.end({layers.data(),hud?2u:1u})) return Transfer::failed;
    if(trace.enabled()) trace.record("pair_submitted",PerformanceTrace::now_ms(),0,
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-std::min(pair.left.metadata.pose_captured,pair.right.metadata.pose_captured)).count());
    return Transfer::accepted;
}
ID3D12CommandQueue* OpenXrBackend::completion_queue(ID3D12CommandQueue* caller) {
    return m && m->game && caller && m->caller_queue(caller) ? m->queue.Get() : caller;
}
void OpenXrBackend::stop() {
    VrCompactHud::get().allow(0,0);
    if(!m) return;
    if(!m->error.empty()) m_status=m->error.str();
    if(!m->destroy()) {
        m_status="OpenXR GPU/runtime teardown incomplete; retained until process exit";
        // A bounded drain failed. Intentionally retain all COM resources and XR handles.
        static auto* retained=new std::vector<State*>; retained->push_back(m.release());
    } else m.reset();
}
std::string OpenXrBackend::status() const { return m && !m->error.empty()?m->error.str():m_status; }

// Same contract and status wording as OpenVrBackend::controllers.
std::optional<VrControllerSample> OpenXrBackend::controllers() {
    if(!m || !m->attached || !m->running || m->terminal) return {};
    auto& s=*m;
    if(!s.focused()) { m_input_status="OpenXR input focus lost; controls neutral"; return {}; }
    // Sync before locating: action spaces follow the action state of the last xrSyncActions.
    const auto values=s.read_actions();
    if(!values) { m_input_status="OpenXR action sync failed; controls neutral"; return {}; }
    // No pose gate: a hand that loses tracking keeps the other hand's stick and every button, and
    // gestures check their own pose. A controller that sleeps or dies still goes neutral: its stick
    // action turns inactive below.
    const auto sample=openxr_input::sample(*values,s.touch_layout);
    if(!sample) { m_input_status="stick action data inactive ("+s.profile.str()+")"; return {}; }
    m_input_status=s.profile.str()+(sample->complete?" locomotion and combat actions active":" locomotion active; some combat bindings unavailable");
    if(!s.grip_offset.measured) m_input_status+="; grip offset unmeasured";
    return sample->sample;
}
bool OpenXrBackend::rumble(const vr_rumble::Commands& commands) {
    // Haptics follow the action set of the last xrSyncActions, which controllers() runs each poll.
    if(!m || !m->haptic || !m->focused()) return false;
    auto& s=*m; bool ok=true;
    for(size_t hand=0;hand<2;++hand) {
        const auto& command=commands[hand];
        if(command.op==vr_rumble::Command::Op::none) continue;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO}; info.action=s.haptic; info.subactionPath=s.hand_paths[hand];
        if(command.op==vr_rumble::Command::Op::stop) { ok=XR_SUCCEEDED(s.api.StopHapticFeedback(s.session,&info)) && ok; continue; }
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.amplitude=command.amplitude; vibration.frequency=command.frequency>0?command.frequency:XR_FREQUENCY_UNSPECIFIED;
        vibration.duration=std::chrono::duration_cast<std::chrono::nanoseconds>(command.duration).count();
        // Only XR_SUCCESS plays. XR_SESSION_NOT_FOCUSED and XR_SESSION_LOSS_PENDING are success codes
        // that play nothing; count them as failures so "no haptics" shows in the VrRumble line.
        ok=s.api.ApplyHapticFeedback(s.session,&info,reinterpret_cast<const XrHapticBaseHeader*>(&vibration))==XR_SUCCESS && ok;
    }
    return ok;
}
bool OpenXrBackend::test_input_available() const { return m && m->focused(); }
bool OpenXrBackend::test_tracking_fallback_available() const {
    return m && m->running && !m->terminal && m->head_lost && m->optics && !m->origin_change;
}
std::optional<VrBackend::Tracking> OpenXrBackend::test_tracking_seed() const {
    // Only the explicitly opted-in scripted-head decorator consumes this: the last runtime
    // optics with a standing head, as OpenVR's seed reads eye transforms without a head pose.
    if(!test_tracking_fallback_available()) return {};
    auto out=*m->optics;
    for(size_t eye=0;eye<2;++eye) if(!vr_camera::rigid(out.eyes[eye]) || !out.fov[eye].valid()) return {};
    return out;
}
