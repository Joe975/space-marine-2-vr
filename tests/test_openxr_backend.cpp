#include "GpuTestHost.h"
#include "OpenXrBackend.h"
#include "OpenXrConversions.h"
#include "OpenXrHands.h"
#include "OpenXrInput.h"
#include "VrCompactHud.h"
#include "VrRuntimeChoice.h"
#include <deque>
#include <map>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
template<class> struct Unsupported;
template<class R,class... Args> struct Unsupported<R(XRAPI_PTR*)(Args...)> {
    static R XRAPI_CALL call(Args...) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
};
struct Fixture {
    Host& gpu;
    bool missing_extension{},wrong_adapter{},should_render{true},invalid_pose{},timeout{},fail_end{};
    bool fail_swapchain{},check_pixels{true},own_queue{};ID3D12CommandQueue* session_queue{};
    unsigned requirements_calls{},sessions{},instances{},begins{},ends{},layers{},acquires{},waits{},releases{},destroyed_chains{};
    unsigned frame_waits{},frame_begins{}; bool frame_active{};
    XrTime time{1000000000};
    XrVector3f expected_head{0,1.7f,0};float expected_left_tangent=-1.1f;
    struct Chain { ComPtr<ID3D12Resource> texture; bool acquired{},waited{}; };
    std::array<Chain,3> chains; // Two eyes, then the compact HUD quad.
    // Controller input fixture: paths are index+1, actions index+100, grip spaces 20/21.
    bool input_extensions{},focused{true},attached{},reject_touch{},fist{};
    bool synced{}; // Like SteamVR: action spaces only locate after a focused xrSyncActions.
    std::vector<std::string> paths; std::vector<std::string> action_names;
    std::map<std::string,std::vector<std::pair<std::string,std::string>>> suggested;
    std::map<std::string,openxr_input::Value> values;
    struct Haptic { std::string hand; float amplitude{}; int64_t duration{}; float frequency{}; }; // amplitude<0: stop.
    std::vector<Haptic> haptics;
    std::array<XrPosef,2> grip{{{{0,0,0,1},{-.2f,1.1f,-.3f}},{{0,.38268343f,0,.92387953f},{.2f,1.1f,-.3f}}}};
    std::array<bool,2> grip_tracked{true,true};
    unsigned trackers{},controller_sources{},motion_ranges{},hud_layers{},action_sets{},grip_spaces{};
    float hud_width{},hud_y{};
    bool profile_event{};
    std::string current_profile{"/interaction_profiles/valve/index_controller"};
    unsigned created{};
    std::deque<XrSessionState> events{XR_SESSION_STATE_READY,XR_SESSION_STATE_FOCUSED};
    inline static Fixture* f{};
    explicit Fixture(Host& host):gpu{host} { f=this; }
    ~Fixture() { f=nullptr; }
    static XrInstance instance() { return reinterpret_cast<XrInstance>(1); }
    static XrSession session() { return reinterpret_cast<XrSession>(2); }
    static size_t index(XrSwapchain chain) { return reinterpret_cast<uintptr_t>(chain)-10; }
    static XrResult XRAPI_CALL requirements(XrInstance,XrSystemId,XrGraphicsRequirementsD3D12KHR* out) {
        ++f->requirements_calls; out->adapterLuid=f->gpu.device->GetAdapterLuid();
        if(f->wrong_adapter) ++out->adapterLuid.LowPart;
        out->minFeatureLevel=D3D_FEATURE_LEVEL_11_0; return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL convert(XrInstance,const LARGE_INTEGER*,XrTime* out) { *out=f->time; return XR_SUCCESS; }
    static XrResult XRAPI_CALL create_tracker(XrSession,const XrHandTrackerCreateInfoEXT* info,XrHandTrackerEXT* out) {
        require(info->handJointSet==XR_HAND_JOINT_SET_DEFAULT_EXT,"default hand joint set expected");
        if(const auto* source=static_cast<const XrHandTrackingDataSourceInfoEXT*>(info->next)) {
            require(source->type==XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT && source->requestedDataSourceCount>=1,"data source chain malformed");
            if(source->requestedDataSources[0]==XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT) ++f->controller_sources;
        }
        *out=reinterpret_cast<XrHandTrackerEXT>(uintptr_t(30+info->hand)); ++f->trackers; return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL destroy_tracker(XrHandTrackerEXT) { --f->trackers; return XR_SUCCESS; }
    // Open hand: identity joints. Fist: each finger joint flexes toward the palm (-Y)
    // by its share of openxr_hands::FULL_CURL_RADIANS.
    static XrResult XRAPI_CALL locate_joints(XrHandTrackerEXT,const XrHandJointsLocateInfoEXT* info,XrHandJointLocationsEXT* out) {
        if(const auto* range=static_cast<const XrHandJointsMotionRangeInfoEXT*>(info->next))
            if(range->handJointsMotionRange==XR_HAND_JOINTS_MOTION_RANGE_UNOBSTRUCTED_EXT) ++f->motion_ranges;
        require(out->jointCount==XR_HAND_JOINT_COUNT_EXT,"all joints requested");
        out->isActive=XR_TRUE;
        for(uint32_t n=0;n<out->jointCount;++n) {
            auto& joint=out->jointLocations[n]; joint.locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
            const bool thumb=n>=XR_HAND_JOINT_THUMB_METACARPAL_EXT && n<=XR_HAND_JOINT_THUMB_TIP_EXT;
            const uint32_t base=thumb?XR_HAND_JOINT_THUMB_METACARPAL_EXT:n<XR_HAND_JOINT_THUMB_METACARPAL_EXT?n:XR_HAND_JOINT_INDEX_METACARPAL_EXT+(n-XR_HAND_JOINT_INDEX_METACARPAL_EXT)/5*5;
            const float depth=n<XR_HAND_JOINT_THUMB_METACARPAL_EXT?0.f:float(std::min(n-base,thumb?2u:3u));
            const float angle=f->fist?-depth*(thumb?openxr_hands::FULL_CURL_RADIANS[0]/2:openxr_hands::FULL_CURL_RADIANS[1]/3):0.f;
            joint.pose.orientation={std::sin(angle/2),0,0,std::cos(angle/2)};
        }
        return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL refresh_rate(XrSession,float* out) { *out=90; return XR_SUCCESS; }
    static XrPath path(const std::string& text) {
        auto it=std::find(f->paths.begin(),f->paths.end(),text);
        if(it==f->paths.end()) { f->paths.push_back(text); return f->paths.size(); }
        return XrPath(it-f->paths.begin()+1);
    }
    static const std::string& action(XrAction handle) { return f->action_names.at(reinterpret_cast<uintptr_t>(handle)-100); }
    void pixels(size_t eye) {
        auto* texture=chains[eye].texture.Get();
        gpu.begin(); auto before=barrier(texture,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
        gpu.commands->ResourceBarrier(1,&before); gpu.execute();
        {
            EyeTextureCapture read;
            check(read.capture(gpu.device.Get(),gpu.queue.Get(),texture,meta(1,0,1)));
            const auto image=read.latest(0); wait(image->fence.Get(),image->fence_value);
            gpu.pixels(*image,eye==0?255:0,0,eye==1?255:0);
        }
        gpu.begin(); auto after=barrier(texture,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        gpu.commands->ResourceBarrier(1,&after); gpu.execute();
    }
    OpenXrDispatch api() {
        OpenXrDispatch a;
#define SM2_FAKE_UNSUPPORTED(name) a.name=&Unsupported<PFN_xr##name>::call;
        SM2_XR_FUNCTIONS(SM2_FAKE_UNSUPPORTED)
#undef SM2_FAKE_UNSUPPORTED
        a.EnumerateInstanceExtensionProperties=[](const char*,uint32_t capacity,uint32_t* count,XrExtensionProperties* out) {
            static constexpr const char* NAMES[]{XR_KHR_D3D12_ENABLE_EXTENSION_NAME,XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
                XR_EXT_HAND_TRACKING_EXTENSION_NAME,XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME,XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME,
                XR_EXT_USER_PRESENCE_EXTENSION_NAME,XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME};
            *count=f->missing_extension?1:f->input_extensions?7:2;
            if(capacity) for(uint32_t n=0;n<*count;++n) std::strcpy(out[n].extensionName,NAMES[n]);
            return XR_SUCCESS;
        };
        a.CreateInstance=[](const XrInstanceCreateInfo* info,XrInstance* out) {
            const auto enabled=[&](const char* name){return std::any_of(info->enabledExtensionNames,info->enabledExtensionNames+info->enabledExtensionCount,[&](const char* e){return std::strcmp(e,name)==0;});};
            require(enabled(XR_KHR_D3D12_ENABLE_EXTENSION_NAME) && enabled(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) &&
                info->enabledExtensionCount==(f->input_extensions?7u:2u) && info->applicationInfo.apiVersion==XR_MAKE_VERSION(1,0,0),"required extensions/version must be explicit");
            *out=instance(); ++f->instances; return XR_SUCCESS;
        };
        a.DestroyInstance=[](XrInstance) { --f->instances; return XR_SUCCESS; };
        a.GetInstanceProperties=[](XrInstance,XrInstanceProperties* out) { std::strcpy(out->runtimeName,"deterministic test runtime"); return XR_SUCCESS; };
        a.GetSystem=[](XrInstance,const XrSystemGetInfo* info,XrSystemId* out) {
            require(info->formFactor==XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,"must request an HMD"); *out=3; return XR_SUCCESS;
        };
        a.GetInstanceProcAddr=[](XrInstance,const char* name,PFN_xrVoidFunction* out) {
            if(std::strcmp(name,"xrGetD3D12GraphicsRequirementsKHR")==0) *out=reinterpret_cast<PFN_xrVoidFunction>(&requirements);
            else if(std::strcmp(name,"xrConvertWin32PerformanceCounterToTimeKHR")==0) *out=reinterpret_cast<PFN_xrVoidFunction>(&convert);
            else if(std::strcmp(name,"xrCreateHandTrackerEXT")==0) *out=reinterpret_cast<PFN_xrVoidFunction>(&create_tracker);
            else if(std::strcmp(name,"xrDestroyHandTrackerEXT")==0) *out=reinterpret_cast<PFN_xrVoidFunction>(&destroy_tracker);
            else if(std::strcmp(name,"xrLocateHandJointsEXT")==0) *out=reinterpret_cast<PFN_xrVoidFunction>(&locate_joints);
            else if(std::strcmp(name,"xrGetDisplayRefreshRateFB")==0) *out=reinterpret_cast<PFN_xrVoidFunction>(&refresh_rate);
            else return XR_ERROR_FUNCTION_UNSUPPORTED;
            return XR_SUCCESS;
        };
        a.EnumerateViewConfigurationViews=[](XrInstance,XrSystemId,XrViewConfigurationType type,uint32_t capacity,uint32_t* count,XrViewConfigurationView* views) {
            require(type==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,"must use stereo"); *count=2;
            // Index at SteamVR's 100% render target; reported for docs/EYE_SIZE.md, not yet used for sizing.
            if(views && capacity>=2) for(uint32_t eye=0;eye<2;++eye) {
                views[eye].recommendedImageRectWidth=2016;views[eye].recommendedImageRectHeight=2240;
                views[eye].maxImageRectWidth=4096;views[eye].maxImageRectHeight=4096;
            }
            return XR_SUCCESS;
        };
        a.CreateSession=[](XrInstance,const XrSessionCreateInfo* info,XrSession* out) {
            const auto& binding=*static_cast<const XrGraphicsBindingD3D12KHR*>(info->next);
            require(f->requirements_calls==1 && binding.device==f->gpu.device.Get() && (f->own_queue?binding.queue && binding.queue!=f->gpu.queue.Get():binding.queue==f->gpu.queue.Get()),"requirements must precede session using actual game's device and queue (or the mod's own queue)");
            f->session_queue=binding.queue;
            *out=session(); ++f->sessions; return XR_SUCCESS;
        };
        a.DestroySession=[](XrSession) { require(!f->frame_active,"shutdown must balance XR frames"); --f->sessions; return XR_SUCCESS; };
        a.EnumerateReferenceSpaces=[](XrSession,uint32_t capacity,uint32_t* count,XrReferenceSpaceType* out) {
            *count=2; if(capacity) {out[0]=XR_REFERENCE_SPACE_TYPE_STAGE;out[1]=XR_REFERENCE_SPACE_TYPE_VIEW;} return XR_SUCCESS;
        };
        a.CreateReferenceSpace=[](XrSession,const XrReferenceSpaceCreateInfo* info,XrSpace* out) {
            require(info->poseInReferenceSpace.orientation.w==1,"space pose must be identity");
            *out=reinterpret_cast<XrSpace>(info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_STAGE?4:5); return XR_SUCCESS;
        };
        a.PollEvent=[](XrInstance,XrEventDataBuffer* buffer) {
            if(f->profile_event) {
                f->profile_event=false; XrEventDataInteractionProfileChanged event{XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED};
                event.session=session(); std::memcpy(buffer,&event,sizeof(event)); return XR_SUCCESS;
            }
            if(f->events.empty()) return XR_EVENT_UNAVAILABLE;
            XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
            event.session=session(); event.state=f->events.front(); f->events.pop_front();
            std::memcpy(buffer,&event,sizeof(event)); return XR_SUCCESS;
        };
        a.BeginSession=[](XrSession,const XrSessionBeginInfo*) { ++f->begins; return XR_SUCCESS; };
        a.EndSession=[](XrSession) { ++f->ends; return XR_SUCCESS; };
        a.LocateSpace=[](XrSpace space,XrSpace base,XrTime,XrSpaceLocation* out) {
            if(const auto hand=reinterpret_cast<uintptr_t>(space)-20;hand<2) {
                require(base==reinterpret_cast<XrSpace>(4),"grips are located in standing space");
                out->pose=f->grip[hand];
                out->locationFlags=!f->synced?0:XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|
                    (f->grip_tracked[hand]?XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT:0);
                return XR_SUCCESS;
            }
            out->locationFlags=f->invalid_pose?0:XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            out->pose.orientation.w=1; out->pose.position={1,1.7f,2}; return XR_SUCCESS;
        };
        a.LocateViews=[](XrSession,const XrViewLocateInfo* info,XrViewState* state,uint32_t,uint32_t* count,XrView* out) {
            require(info->space==reinterpret_cast<XrSpace>(5),"eye offsets must be located relative to the head");
            *count=2; state->viewStateFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            for(int eye=0;eye<2;++eye) {out[eye].pose.orientation.w=1;out[eye].pose.position.x=eye?.032f:-.032f;out[eye].fov={-.8f,.7f,.9f,-.7f};}
            return XR_SUCCESS;
        };
        a.WaitFrame=[](XrSession,const XrFrameWaitInfo*,XrFrameState* out) {
            require(!f->frame_active,"no overlapping XR frames"); ++f->frame_waits;
            out->shouldRender=f->should_render; out->predictedDisplayTime=f->time+11000000; out->predictedDisplayPeriod=11111111; return XR_SUCCESS;
        };
        a.BeginFrame=[](XrSession,const XrFrameBeginInfo*) { f->frame_active=true; ++f->frame_begins; return XR_SUCCESS; };
        a.EndFrame=[](XrSession,const XrFrameEndInfo* info) {
            require(f->frame_active && info->displayTime==f->time+11000000,"end must use this XR frame's predicted time");
            f->frame_active=false;
            if(f->fail_end) return XR_ERROR_RUNTIME_FAILURE;
            if(info->layerCount==2) {
                const auto& quad=*reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[1]);
                require(quad.type==XR_TYPE_COMPOSITION_LAYER_QUAD && quad.space==reinterpret_cast<XrSpace>(5) && quad.eyeVisibility==XR_EYE_VISIBILITY_BOTH &&
                    quad.subImage.imageRect.extent.width==int32_t(VrCompactHud::WIDTH) && !f->chains[2].acquired,"HUD must be a released head-locked quad");
                ++f->hud_layers; f->hud_width=quad.size.width; f->hud_y=quad.pose.position.y;
            }
            if(info->layerCount) {
                require(info->layerCount<=2,"stereo layer plus optional HUD quad expected");
                const auto& layer=*reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
                require(layer.viewCount==2 && layer.space==reinterpret_cast<XrSpace>(4),"stereo layer must use standing space");
                for(size_t eye=0;eye<2;++eye) {
                    require(!f->chains[eye].acquired,"images must be released before end frame");
                    const auto& view=layer.views[eye];
                    require(std::abs(view.pose.position.y-f->expected_head.y)<1e-5f && std::abs(view.pose.position.x-(f->expected_head.x+(eye?.032f:-.032f)))<1e-5f && std::abs(view.pose.position.z-f->expected_head.z)<1e-5f,
                        "submission must describe source image poses, not newly located/predicted headset pose");
                    require(std::abs(std::tan(view.fov.angleLeft)-f->expected_left_tangent)<1e-5f && view.subImage.imageRect.extent.width==2,"source image FOV and size must survive");
                    if(f->check_pixels) f->pixels(eye);
                }
                ++f->layers;
            }
            return XR_SUCCESS;
        };
        a.EnumerateSwapchainFormats=[](XrSession,uint32_t capacity,uint32_t* count,int64_t* out) {
            *count=1; if(capacity) out[0]=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; return XR_SUCCESS;
        };
        a.CreateSwapchain=[](XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out) {
            if(f->fail_swapchain) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
            const bool hud=f->created==2; // Eyes first, then the HUD quad.
            require((hud?info->width==VrCompactHud::WIDTH && info->height==VrCompactHud::HEIGHT:info->width==2 && info->height==2) && info->sampleCount==1 && info->arraySize==1 &&
                (info->usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT),"swapchain must retain source resolution and RT usage");
            auto& chain=f->chains.at(f->created);
            D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=info->width;desc.Height=info->height;
            desc.DepthOrArraySize=desc.MipLevels=1; desc.Format=DXGI_FORMAT(info->format);desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
            check(f->gpu.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&chain.texture)));
            *out=reinterpret_cast<XrSwapchain>(10+f->created++); return XR_SUCCESS;
        };
        a.DestroySwapchain=[](XrSwapchain handle) { auto& chain=f->chains.at(index(handle));require(!chain.acquired,"destroy cannot retain an acquired image");chain.texture.Reset();++f->destroyed_chains;return XR_SUCCESS; };
        a.EnumerateSwapchainImages=[](XrSwapchain handle,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* out) {
            *count=1;if(capacity) reinterpret_cast<XrSwapchainImageD3D12KHR*>(out)->texture=f->chains.at(index(handle)).texture.Get();return XR_SUCCESS;
        };
        a.AcquireSwapchainImage=[](XrSwapchain handle,const XrSwapchainImageAcquireInfo*,uint32_t* out) {
            auto& chain=f->chains.at(index(handle)); require(!chain.acquired,"must not reacquire a held image");chain.acquired=true;*out=0;++f->acquires;return XR_SUCCESS;
        };
        a.WaitSwapchainImage=[](XrSwapchain handle,const XrSwapchainImageWaitInfo*) {
            auto& chain=f->chains.at(index(handle));require(chain.acquired,"wait must follow acquisition");++f->waits;
            if(f->timeout) return XR_TIMEOUT_EXPIRED;chain.waited=true;return XR_SUCCESS;
        };
        a.ReleaseSwapchainImage=[](XrSwapchain handle,const XrSwapchainImageReleaseInfo*) {
            auto& chain=f->chains.at(index(handle));require(chain.acquired && chain.waited,"cannot release without a successful wait");chain.acquired=chain.waited=false;++f->releases;return XR_SUCCESS;
        };
        a.StringToPath=[](XrInstance,const char* text,XrPath* out) { require(text[0]=='/',"paths are absolute"); *out=path(text); return XR_SUCCESS; };
        a.PathToString=[](XrInstance,XrPath value,uint32_t capacity,uint32_t* count,char* out) {
            const auto& text=f->paths.at(value-1); *count=uint32_t(text.size()+1);
            if(capacity) { require(capacity>text.size(),"path buffer too small"); std::strcpy(out,text.c_str()); }
            return XR_SUCCESS;
        };
        a.CreateActionSet=[](XrInstance,const XrActionSetCreateInfo* info,XrActionSet* out) {
            require(std::strcmp(info->actionSetName,"sm2vr")==0,"action set must match the SteamVR manifest set");
            ++f->action_sets; *out=reinterpret_cast<XrActionSet>(uintptr_t(90)); return XR_SUCCESS;
        };
        a.DestroyActionSet=[](XrActionSet) { --f->action_sets; return XR_SUCCESS; };
        a.CreateAction=[](XrActionSet,const XrActionCreateInfo* info,XrAction* out) {
            require(!f->attached,"actions must be created before attach");
            require(std::find(f->action_names.begin(),f->action_names.end(),info->actionName)==f->action_names.end(),"action names must be unique");
            f->action_names.push_back(info->actionName);
            *out=reinterpret_cast<XrAction>(uintptr_t(99+f->action_names.size())); return XR_SUCCESS;
        };
        a.SuggestInteractionProfileBindings=[](XrInstance,const XrInteractionProfileSuggestedBinding* info) {
            require(!f->attached,"suggestions must precede attach");
            const auto profile=f->paths.at(info->interactionProfile-1);
            if(f->reject_touch && profile.find("touch")!=std::string::npos) return XR_ERROR_PATH_UNSUPPORTED;
            auto& list=f->suggested[profile];
            for(uint32_t n=0;n<info->countSuggestedBindings;++n)
                list.emplace_back(action(info->suggestedBindings[n].action),f->paths.at(info->suggestedBindings[n].binding-1));
            return XR_SUCCESS;
        };
        a.AttachSessionActionSets=[](XrSession,const XrSessionActionSetsAttachInfo* info) {
            require(info->countActionSets==1 && f->begins==0,"one action set attached before the session begins");
            f->attached=true; return XR_SUCCESS;
        };
        a.SyncActions=[](XrSession,const XrActionsSyncInfo* info) {
            require(f->attached && info->countActiveActionSets==1,"sync needs the attached set");
            f->synced=f->focused; return f->focused?XR_SUCCESS:XR_SESSION_NOT_FOCUSED;
        };
        a.GetActionStateBoolean=[](XrSession,const XrActionStateGetInfo* info,XrActionStateBoolean* out) {
            const auto v=f->values[action(info->action)]; out->isActive=v.active; out->currentState=v.x!=0; return XR_SUCCESS;
        };
        a.GetActionStateFloat=[](XrSession,const XrActionStateGetInfo* info,XrActionStateFloat* out) {
            const auto v=f->values[action(info->action)]; out->isActive=v.active; out->currentState=v.x; return XR_SUCCESS;
        };
        a.GetActionStateVector2f=[](XrSession,const XrActionStateGetInfo* info,XrActionStateVector2f* out) {
            const auto v=f->values[action(info->action)]; out->isActive=v.active; out->currentState={v.x,v.y}; return XR_SUCCESS;
        };
        a.CreateActionSpace=[](XrSession,const XrActionSpaceCreateInfo* info,XrSpace* out) {
            require(action(info->action)=="hand_pose" && info->poseInActionSpace.orientation.w==1,"grip spaces come from the pose action");
            const auto& hand=f->paths.at(info->subactionPath-1);
            *out=reinterpret_cast<XrSpace>(uintptr_t(hand=="/user/hand/left"?20:21)); ++f->grip_spaces; return XR_SUCCESS;
        };
        a.GetCurrentInteractionProfile=[](XrSession,XrPath,XrInteractionProfileState* out) {
            out->interactionProfile=path(f->current_profile.c_str()); return XR_SUCCESS;
        };
        a.ApplyHapticFeedback=[](XrSession,const XrHapticActionInfo* info,const XrHapticBaseHeader* header) {
            require(action(info->action)=="haptic" && header->type==XR_TYPE_HAPTIC_VIBRATION,"rumble plays through the haptic output action");
            const auto* v=reinterpret_cast<const XrHapticVibration*>(header);
            f->haptics.push_back({f->paths.at(info->subactionPath-1),v->amplitude,v->duration,v->frequency}); return f->focused?XR_SUCCESS:XR_SESSION_NOT_FOCUSED;
        };
        a.StopHapticFeedback=[](XrSession,const XrHapticActionInfo* info) {
            require(action(info->action)=="haptic","rumble stops through the haptic output action");
            f->haptics.push_back({f->paths.at(info->subactionPath-1),-1}); return XR_SUCCESS;
        };
        a.DestroySpace=[](XrSpace space) { if(reinterpret_cast<uintptr_t>(space)-20<2) --f->grip_spaces; return XR_SUCCESS; };
        return a;
    }
};
void math_and_selection() {
    for(const auto q:std::array<XrQuaternionf,5>{{{0,0,0,1},{.70710678f,0,0,.70710678f},{0,.70710678f,0,.70710678f},{0,0,1,0},{0,1,0,0}}}) {
        const XrPosef source{q,{1,2,3}}; const auto m=openxr_conversion::matrix(source); require(m.has_value(),"quaternion must convert");
        const auto pose=openxr_conversion::pose(*m);require(pose.has_value(),"matrix must convert back");
        const auto result=openxr_conversion::matrix(*pose);
        for(int n=0;n<16;++n) require(std::abs((*m)[n]-(*result)[n])<1e-5f,"row-vector quaternion roundtrip must preserve rotation/translation");
    }
    const auto yaw=openxr_conversion::matrix({{0,.70710678f,0,.70710678f},{}});
    require((*yaw)[8]>.999f && (*yaw)[2]<-.999f,"positive yaw must map XR -Z toward -X");
    require(!openxr_conversion::matrix({{0,0,0,0},{}}),"invalid quaternion must be rejected");
    vr_camera::Matrix projection{}; const vr_projection::Tangents slopes{-1.1f,.9f,-.8f,1.2f};
    require(vr_projection::build(slopes,.1f,1000,projection),"projection fixture must build");
    const auto fov=openxr_conversion::fov(projection);const auto result=openxr_conversion::tangents(*fov);
    require(std::abs(result.left-slopes.left)<1e-5f && std::abs(result.top-slopes.top)<1e-5f,"asymmetric projection must survive without flipping Y");
    require(!openxr_conversion::fov({}),"empty projection must fail");
    require(openxr_conversion::worn(std::nullopt,XR_SESSION_STATE_FOCUSED)==true && openxr_conversion::worn(std::nullopt,XR_SESSION_STATE_SYNCHRONIZED)==false &&
        openxr_conversion::worn(std::nullopt,XR_SESSION_STATE_READY)==false && !openxr_conversion::worn(std::nullopt,XR_SESSION_STATE_VISIBLE) &&
        !openxr_conversion::worn(std::nullopt,XR_SESSION_STATE_STOPPING),"session state: focused is worn, synchronized is off, the dashboard says nothing");
    require(openxr_conversion::worn(false,XR_SESSION_STATE_FOCUSED)==false && openxr_conversion::worn(true,XR_SESSION_STATE_SYNCHRONIZED)==true,
        "user presence, when reported, wins over the session state");
    require(vr_runtime_choice(true,true,false)==VrRuntimeChoice::invalid && vr_runtime_choice(false,true,true)==VrRuntimeChoice::invalid,"conflicting and synthetic runtime modes must fail closed");
    require(vr_runtime_choice(false,true,false)==VrRuntimeChoice::openxr && vr_runtime_choice(true,false,false)==VrRuntimeChoice::openvr,"runtime selection must remain explicit");
}
void single_frame_submission(Host& gpu) {
    Fixture fixture{gpu};
    fixture.expected_head={1,1.7f,2};fixture.expected_left_tangent=std::tan(-.8f);
    auto backend=std::make_unique<OpenXrBackend>(fixture.api(),+[]{return true;});
    VrSession session{std::move(backend)};session.enable(true);session.set_stereo(true);
    require(session.set_render_path(VrSession::RenderPath::ordinary),"OpenXR must select the real single-frame stereo path");
    const auto now=std::chrono::steady_clock::now();session.poll(now,gpu.queue.Get());
    const auto frame=session.ordinary_frame({31,51,0},now);require(frame.has_value(),"one native job issues both OpenXR eye poses");
    auto red=gpu.color(1,0,0),blue=gpu.color(0,0,1);std::array<EyeTextureCapture,2> captures;
    for(int eye=0;eye<2;++eye){
        const auto& issued=frame->eyes[eye];auto image=meta(51,eye,31);
        image.tracked_stereo=true;image.tracking_epoch=issued.tracking_epoch;image.pose_captured=issued.captured;
        image.runtime_head=issued.runtime_head;image.runtime_eye_to_head=issued.runtime_eye_to_head;
        require(vr_projection::build(*issued.fov,.1f,1000,image.projection),"single-frame source projection must preserve runtime optics");
        check(captures[eye].capture(gpu.device.Get(),gpu.queue.Get(),eye?blue.Get():red.Get(),image));
        const auto retained=*captures[eye].latest(eye);wait(retained.fence.Get(),retained.fence_value);
    }
    VrSession::OrdinaryImages pair{{*captures[0].latest(0),*captures[1].latest(1)},77};
    auto wrong=pair;++wrong.images.right.metadata.render_id;
    require(!session.submit_ordinary(wrong,gpu.queue.Get(),now) && fixture.layers==0,"alternating-frame IDs cannot enter the OpenXR single-frame path");
    require(!session.submit(pair.images,gpu.queue.Get(),now),"same-job OpenXR images cannot route through AFR");
    require(session.submit_ordinary(pair,gpu.queue.Get(),now) && fixture.layers==1 && fixture.acquires==2 && fixture.releases==2,"one same-job pair reaches real WARP runtime swapchains with independent red/blue eyes");
    require(!session.submit_ordinary(pair,gpu.queue.Get(),now) && fixture.layers==1,"single-frame OpenXR pair submits exactly once");
    require(session.statistics().submitted_pairs==1 && !session.statistics().failed_transfers,"single-frame runtime counters count successful transfers");
    check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);session.enable(false);
}
}
// Controls: bindings, the VrControllerSample contract, raw-equivalent grips, curls, focus,
// the scripted-head fallback and the compact HUD quad, all through the injected runtime.
void controller_input(Host& gpu,const EyeTextureCapture::Pair& pair) {
    SetEnvironmentVariableA("SM2VR_CONTROLLERS","1");
    const auto grip_log=std::filesystem::temp_directory_path()/"sm2vr_openxr_grip_log_test.csv";
    std::filesystem::remove(grip_log);SetEnvironmentVariableA("SM2VR_OPENXR_GRIP_LOG",grip_log.string().c_str());
    Fixture fixture{gpu};fixture.input_extensions=true;fixture.check_pixels=false;
    fixture.expected_head={0,1.7f,0};fixture.expected_left_tangent=-1.1f;
    {
        OpenXrBackend backend{fixture.api(),+[]{return true;}};
        require(backend.prepare_graphics(gpu.queue.Get()) && backend.start(),"controls fixture must start");
        require(fixture.attached && fixture.action_names.size()==openxr_input::COUNT+2 && fixture.grip_spaces==2,"all manifest actions and both grips must exist before begin");
        require(fixture.trackers==2 && fixture.controller_sources==2,"hand trackers must prefer the controller data source");
        const auto& index=fixture.suggested.at("/interaction_profiles/valve/index_controller");
        const auto bound=[&](const char* a,const char* p){return std::ranges::find(index,std::pair<std::string,std::string>{a,p})!=index.end();};
        require(bound("melee","/user/hand/right/input/squeeze/force") && bound("utility","/user/hand/left/input/trackpad") &&
            bound("recenter","/user/hand/left/input/a/click") && bound("hand_pose","/user/hand/right/input/grip/pose"),"Index bindings must mirror bindings_index.json");
        require(bound("haptic","/user/hand/left/output/haptic") && bound("haptic","/user/hand/right/output/haptic"),"both controllers' haptics are bound");
        require(fixture.suggested.contains("/interaction_profiles/oculus/touch_controller"),"Touch bindings must be suggested");
        require(backend.input_status().find("bindings Index,Touch; hand tracking on")!=std::string::npos,"startup status names accepted bindings and hand tracking");
        require(!backend.test_input_available() && !backend.controllers(),"no input before the session is focused");
        using RumbleOp=vr_rumble::Command::Op;
        const vr_rumble::Commands rumble{{{RumbleOp::apply,.4f},{RumbleOp::stop,0}}};
        require(!backend.rumble(rumble) && fixture.haptics.empty(),"no haptics before the session is focused");
        backend.tick();require(fixture.begins==1 && backend.test_input_available(),"FOCUSED enables test input");
        fixture.profile_event=true;backend.tick();
        // Controller poses: raw-equivalent origins of the fixture grips.
        fixture.fist=true;
        const auto tracking=backend.tracking();require(tracking.has_value(),"tracking must succeed");
        const auto offset=openxr_hands::GripOffset::index_render_model();
        for(size_t hand=0;hand<2;++hand) {
            const auto expected=offset.raw(hand,*openxr_conversion::matrix(fixture.grip[hand]));
            require(tracking->controllers[hand].has_value(),"tracked grips must produce controllers");
            for(int n=0;n<16;++n) require(std::abs((*tracking->controllers[hand])[n]-(*expected)[n])<1e-5f,"controllers must be raw-equivalent grip origins");
            require(tracking->fingers[hand] && std::ranges::all_of(*tracking->fingers[hand],[](float c){return c>.99f;}),"fist joints must read as full curls");
        }
        require(fixture.motion_ranges==2,"joints must use the unobstructed (device) motion range");
        {   // Readable while the backend is alive: the launcher kills the game, so no fclose.
            std::ifstream log{grip_log};std::string header,left,right;std::getline(log,header);std::getline(log,left);std::getline(log,right);
            require(header.starts_with("qpc,hand,m0") && left.find(",0,")!=std::string::npos && right.find(",1,")!=std::string::npos,"grip log rows must be flushed as they are written");
        }
        // Sample contract, as openvr_combat::poll.
        auto& v=fixture.values;
        for(const auto& d:openxr_input::ACTIONS) v[d.name]={true,0,0};
        v["move"]={true,.5f,-.25f};v["turn"]={true,.8f,0};v["confirm"]={true,1};v["fire"]={true,.7f};v["fire_touch"]={true,1};
        v["melee"]={true,.4f};v["utility"]={true,.1f,-.2f};v["finisher"]={true,1};
        auto sample=backend.controllers();
        require(sample && sample->move_x==.5f && sample->move_y==-.25f && sample->turn_x==.8f && sample->confirm && !sample->interact &&
            sample->fire==.7f && sample->fire_touch && sample->fire_touch_valid && sample->melee==.4f && sample->utility_x==.1f && sample->utility_y==-.2f && sample->finisher,
            "actions must reach the same sample fields as OpenVR");
        require(backend.input_status().starts_with("Index locomotion and combat actions active"),"complete bindings status");
        require(!sample->touch_layout,"Index samples keep the trackpad layout");
        // A Quest over Steam Link: SteamVR reports Touch (docs/QUEST_STEAM_LINK.md).
        fixture.current_profile="/interaction_profiles/oculus/touch_controller";fixture.profile_event=true;backend.tick();
        sample=backend.controllers();
        require(sample && sample->touch_layout && backend.input_status().starts_with("Touch "),"a Touch profile change marks samples with the Touch layout");
        fixture.current_profile="/interaction_profiles/valve/index_controller";fixture.profile_event=true;backend.tick();
        sample=backend.controllers();require(sample && !sample->touch_layout && backend.input_status().starts_with("Index "),"and back to Index");
        v["grenade"]={false,0};v["fire_touch"]={false,0};sample=backend.controllers();
        require(sample && sample->grenade==0 && !sample->fire_touch_valid && backend.input_status().find("some combat bindings unavailable")!=std::string::npos,
            "unbound optional actions release only themselves");
        v["move"]={false,0};require(!backend.controllers(),"inactive stick is neutral");v["move"]={true,0,0};
        fixture.grip_tracked[1]=false;
        require(!backend.controllers() && backend.input_status().starts_with("both controllers"),"an untracked controller makes controls neutral");
        require(!backend.tracking()->controllers[1],"an untracked grip drops that hand");
        fixture.grip_tracked[1]=true;
        // Focus loss (dashboard) is neutral and stops the test driver.
        fixture.events.push_back(XR_SESSION_STATE_VISIBLE);backend.tick();fixture.focused=false;
        require(!backend.controllers() && !backend.test_input_available() && !backend.tracking()->fingers[0],"VISIBLE means no input focus");
        fixture.events.push_back(XR_SESSION_STATE_FOCUSED);backend.tick();fixture.focused=true;
        require(backend.controllers().has_value(),"focus returns");
        require(backend.rumble(rumble) && fixture.haptics.size()==2 && fixture.haptics[0].hand=="/user/hand/left" && std::abs(fixture.haptics[0].amplitude-.4f)<1e-6f &&
            fixture.haptics[0].duration==100'000'000 && fixture.haptics[1].hand=="/user/hand/right" && fixture.haptics[1].amplitude<0,"rumble commands become per-hand OpenXR pulses and stops");
        require(backend.rumble({}) && fixture.haptics.size()==2,"no-op commands call nothing");
        require(fixture.haptics[0].frequency==XR_FREQUENCY_UNSPECIFIED,"frequency 0 leaves it to the runtime");
        {
            vr_rumble::Command tuned{RumbleOp::apply,1,160,std::chrono::milliseconds(500)};
            require(backend.rumble({tuned,{RumbleOp::none,0}}) && fixture.haptics.size()==3 && fixture.haptics[2].frequency==160 &&
                fixture.haptics[2].duration==500'000'000,"an explicit frequency and duration reach xrApplyHapticFeedback");
        }
        fixture.focused=false;
        require(!backend.rumble({{{RumbleOp::apply,.4f},{RumbleOp::none,0}}}),"a pulse the runtime answers XR_SESSION_NOT_FOCUSED played nothing: a failure, not a success");
        fixture.focused=true;fixture.haptics.clear();
        // Scripted-head fallback keeps the last runtime optics only while the head is lost.
        require(!backend.test_tracking_fallback_available(),"no fallback while the head is tracked");
        fixture.invalid_pose=true;require(!backend.tracking(),"head loss is passive");
        const auto seed=backend.test_tracking_seed();
        require(backend.test_tracking_fallback_available() && seed && seed->head[13]==1.7f && seed->eyes[0][12]<0 && seed->fov[0].valid(),"seed must carry cached optics");
        fixture.invalid_pose=false;require(backend.tracking() && !backend.test_tracking_fallback_available(),"tracking recovery ends the fallback");
        // Compact HUD: a quad while data is live, hidden after it is withdrawn.
        auto& hud=VrCompactHud::get();hud.allow(7,9);
        VrCompactHud::Data data{7,9,std::chrono::steady_clock::now()};data.health=.5f;require(hud.publish(data),"HUD data must publish");
        require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted && fixture.hud_layers==1 &&
            std::abs(fixture.hud_width-hud.settings().values.values[0])<1e-6f && std::abs(fixture.hud_y-hud.settings().values.values[1])<1e-6f,"HUD quad uses the overlay settings");
        const auto uploads=fixture.releases;
        require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted && fixture.hud_layers==2 && fixture.releases==uploads+2,"an unchanged HUD is not re-uploaded");
        hud.allow(0,0);std::this_thread::sleep_for(std::chrono::milliseconds(120));
        require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted && fixture.hud_layers==2,"withdrawn HUD data hides the quad");
        backend.stop();
        require(fixture.instances==0 && fixture.action_sets==0 && fixture.grip_spaces==0 && fixture.trackers==0 && fixture.destroyed_chains==3,"teardown releases input objects and the HUD swapchain");
    }
    {
        Fixture touchless{gpu};touchless.reject_touch=true;
        OpenXrBackend backend{touchless.api(),+[]{return true;}};
        require(backend.prepare_graphics(gpu.queue.Get()) && backend.start() && touchless.attached,"a rejected profile must not block the others");
        backend.stop();
    }
    SetEnvironmentVariableA("SM2VR_CONTROLLERS",nullptr);SetEnvironmentVariableA("SM2VR_OPENXR_GRIP_LOG",nullptr);
    std::filesystem::remove(grip_log);
    std::cout<<"OpenXR controls, grips, curls, focus, scripted-head fallback and HUD quad passed\n";
}
int main() {
    try {
        // The fixtures below cover the game-queue path (the SM2VR_OPENXR_GAME_QUEUE opt-out); the own
        // queue, the default, has its own blocks further down.
        SetEnvironmentVariableW(L"SM2VR_OPENXR_GAME_QUEUE",L"1");
        math_and_selection(); Host gpu;single_frame_submission(gpu);
        auto red=gpu.color(1,0,0),blue=gpu.color(0,0,1);
        EyeTextureCapture capture;
        auto left=meta(1,0,1),right=meta(2,1,2);
        for(auto* image:{&left,&right}) {
            image->runtime_head[13]=1.7f;image->runtime_eye_to_head[12]=image->eye?.032f:-.032f;
            image->pose_captured=std::chrono::steady_clock::now();
            require(vr_projection::build({-1.1f,.9f,-.9f,1.1f},.1f,1000,image->projection),"source projection must build");
        }
        check(capture.capture(gpu.device.Get(),gpu.queue.Get(),red.Get(),left));check(capture.capture(gpu.device.Get(),gpu.queue.Get(),blue.Get(),right));
        auto image=capture.latest(1);wait(image->fence.Get(),image->fence_value); const auto pair=*capture.pair(false);
        {
            Fixture fixture{gpu};OpenXrBackend backend{fixture.api(),+[]{return true;}};
            require(!backend.prepare_graphics(nullptr),"graphics queue required");
            require(backend.prepare_graphics(gpu.queue.Get()) && backend.start(),"real WARP queue must initialize fake runtime");
            require(backend.status().find("recommended eye 2016x2240 (max 4096x4096); waiting for session readiness")!=std::string::npos,
                "startup status reports the runtime's recommended eye size");
            backend.tick();require(fixture.begins==1,"READY event must begin session once");
            const auto tracking=backend.tracking();require(tracking && tracking->head[13]==1.7f && tracking->eyes[0][12]<0 && tracking->fov[0].top>0,"head, eye and FOV conversion must reach production tracking");
            require(backend.status().ends_with("; queue=game"),"the running status names the game's queue");
            fixture.invalid_pose=true;require(!backend.tracking(),"invalid tracking must become passive");fixture.invalid_pose=false;
            fixture.should_render=false;
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::skipped && fixture.layers==0,"shouldRender false must submit no projection and remain retryable");
            fixture.should_render=true;fixture.timeout=true;
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::skipped && fixture.acquires==1 && fixture.releases==0,"timeout must retain acquisition and avoid illegal release");
            fixture.timeout=false;
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted && fixture.layers==1 && fixture.acquires==2 && fixture.releases==2,"retry must reuse held acquisition and copy independent eye pixels");
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted && fixture.layers==2,"runtime images/allocators must be reusable after their GPU fences");
            fixture.fail_end=true;
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::failed && !fixture.frame_active,"end-frame failure must not leave an unmatched frame");
            fixture.fail_end=false;fixture.events.push_back(XR_SESSION_STATE_STOPPING);backend.tick();
            require(fixture.ends==1 && backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::skipped,"STOPPING must end session and suppress transfers");
            backend.stop();require(fixture.instances==0 && fixture.sessions==0 && fixture.destroyed_chains==2,"shutdown must release runtime objects after GPU work");
        }
        {
            // Default: the runtime gets a mod-owned direct queue on the game's device.
            SetEnvironmentVariableW(L"SM2VR_OPENXR_GAME_QUEUE",nullptr);
            Fixture fixture{gpu};fixture.own_queue=true;fixture.check_pixels=false;
            OpenXrBackend backend{fixture.api(),+[]{return true;}};
            require(backend.prepare_graphics(gpu.queue.Get()) && backend.start(),"own-queue session starts");
            require(backend.status().find("runtime on the mod's own queue")!=std::string::npos,"status names the own queue");
            SetEnvironmentVariableW(L"SM2VR_OPENXR_GAME_QUEUE",L"1");
            require(fixture.session_queue && fixture.session_queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT,"the session is bound to the mod's own direct queue");
            require(backend.prepare_graphics(gpu.queue.Get()) && !backend.prepare_graphics(fixture.session_queue),"the game's queue stays the caller's queue");
            backend.tick();
            require(backend.tracking() && backend.status().ends_with("; queue=own"),"the running status names the own queue");
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted && fixture.layers==1,"transfers still take the game's queue");
            require(backend.transfer(pair,fixture.session_queue)==VrBackend::Transfer::failed,"never the mod's own queue as the caller");
            {   // -HandoffControl isolated hands over images from its own bridge queue: ordered after that queue instead.
                D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue> isolated;check(gpu.device->CreateCommandQueue(&desc,IID_PPV_ARGS(&isolated)));
                const auto before=fixture.layers;
                require(backend.transfer(pair,isolated.Get())==VrBackend::Transfer::accepted && fixture.layers==before+1 && backend.completion_queue(isolated.Get())==fixture.session_queue,"a bridge queue of the same device is accepted and leases end on the mod's queue");
                require(backend.completion_queue(gpu.queue.Get())==fixture.session_queue,"game-queue leases end on the mod's queue too");
            }
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted && fixture.layers==3,"slots are reusable after their own-queue fences");
            // Order the game queue after the mod's queue, then read the runtime images: each eye's own pixels.
            ComPtr<ID3D12Fence> after;check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&after)));
            check(fixture.session_queue->Signal(after.Get(),1));check(gpu.queue->Wait(after.Get(),1));
            fixture.pixels(0);fixture.pixels(1);
            // Ordering: a source still being written on the game queue (held behind a gate) is copied only
            // after that write. The left eye starts green; the pending game-queue copy makes it red.
            {
                auto green=gpu.color(0,1,0);EyeTextureCapture late;auto l=meta(11,0,11),r=meta(12,1,12);
                for(auto* image:{&l,&r}){image->runtime_head[13]=1.7f;image->runtime_eye_to_head[12]=image->eye?.032f:-.032f;image->pose_captured=std::chrono::steady_clock::now();
                    require(vr_projection::build({-1.1f,.9f,-.9f,1.1f},.1f,1000,image->projection),"source projection must build");}
                check(late.capture(gpu.device.Get(),gpu.queue.Get(),green.Get(),l));check(late.capture(gpu.device.Get(),gpu.queue.Get(),blue.Get(),r));
                const auto latest=late.latest(1);wait(latest->fence.Get(),latest->fence_value);const auto pending_pair=*late.pair(false);
                ComPtr<ID3D12Fence> gate;check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
                check(gpu.queue->Wait(gate.Get(),1));
                gpu.begin();
                const D3D12_RESOURCE_BARRIER to_copy[]{barrier(pending_pair.left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST),barrier(red.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE)};
                gpu.commands->ResourceBarrier(2,to_copy);gpu.commands->CopyResource(pending_pair.left.texture(),red.Get());
                const D3D12_RESOURCE_BARRIER back[]{barrier(pending_pair.left.texture(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),barrier(red.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON)};
                gpu.commands->ResourceBarrier(2,back);check(gpu.commands->Close());
                ID3D12CommandList* lists[]{gpu.commands.Get()};gpu.queue->ExecuteCommandLists(1,lists);
                require(backend.transfer(pending_pair,gpu.queue.Get())==VrBackend::Transfer::accepted,"a pending source is accepted; the GPU orders it");
                check(gate->Signal(1));
                check(fixture.session_queue->Signal(after.Get(),2));check(gpu.queue->Wait(after.Get(),2));
                check(gpu.queue->Signal(gpu.fence.Get(),++gpu.tick));wait(gpu.fence.Get(),gpu.tick);
                fixture.pixels(0);fixture.pixels(1); // Red, not the green it held when transfer() ran.
            }
            // The compact HUD quad uploads on the mod's queue too.
            auto& hud=VrCompactHud::get();hud.allow(7,9);
            VrCompactHud::Data data{7,9,std::chrono::steady_clock::now()};data.health=.5f;require(hud.publish(data),"HUD data must publish");
            const auto layers=fixture.hud_layers;std::this_thread::sleep_for(std::chrono::milliseconds(120)); // The HUD is polled every 100 ms.
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted && fixture.hud_layers==layers+1,"the HUD quad is submitted in own-queue mode");
            hud.allow(0,0);
            // A runtime session restart keeps the mod's queue.
            fixture.events.push_back(XR_SESSION_STATE_STOPPING);backend.tick();
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::skipped,"STOPPING suppresses transfers");
            fixture.events.push_back(XR_SESSION_STATE_READY);fixture.events.push_back(XR_SESSION_STATE_FOCUSED);backend.tick();
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted,"a restarted session transfers again on the mod's queue");
            backend.stop();
        }
        {
            // The own queue can't be created: VR still starts, on the game's queue, and says so.
            SetEnvironmentVariableW(L"SM2VR_OPENXR_GAME_QUEUE",nullptr);
            Fixture fixture{gpu};
            OpenXrBackend backend{fixture.api(),+[]{return true;},+[](ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC&,ComPtr<ID3D12CommandQueue>&){return E_OUTOFMEMORY;}};
            require(backend.prepare_graphics(gpu.queue.Get()) && backend.start(),"a failed own queue falls back to the game's queue");
            SetEnvironmentVariableW(L"SM2VR_OPENXR_GAME_QUEUE",L"1");
            require(fixture.session_queue==gpu.queue.Get(),"the fallback session is bound to the game's queue");
            require(backend.status().find("runtime on the game's queue (own queue unavailable)")!=std::string::npos,"status names the fallback");
            backend.tick();
            require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::accepted && backend.completion_queue(gpu.queue.Get())==gpu.queue.Get(),"the fallback transfers and ends leases on the game's queue");
            backend.stop();
        }
        controller_input(gpu,pair);
        for(int failure=0;failure<3;++failure) {
            Fixture fixture{gpu};fixture.missing_extension=failure==0;fixture.wrong_adapter=failure==1;fixture.fail_swapchain=failure==2;
            OpenXrBackend backend{fixture.api(),+[]{return true;}};require(backend.prepare_graphics(gpu.queue.Get()),"fixture queue must be valid");
            if(failure<2) require(!backend.start() && fixture.sessions==0 && fixture.instances==0,"extension/adapter failure must never leave a session");
            else {require(backend.start(),"swapchain failure fixture must start");backend.tick();require(backend.transfer(pair,gpu.queue.Get())==VrBackend::Transfer::failed && !fixture.frame_active,"swapchain failure must balance begun frame");backend.stop();}
        }
        ComPtr<ID3D12InfoQueue> info;
        if(SUCCEEDED(gpu.device.As(&info))) for(UINT64 n=0;n<info->GetNumStoredMessages();++n) {
            SIZE_T size{};info->GetMessage(n,nullptr,&size);std::vector<char> buffer(size);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(buffer.data());info->GetMessage(n,message,&size);
            require(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR && message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,"OpenXR copies must pass the D3D12 debug layer");
        }
        std::cout<<"OpenXR fixture, real WARP pixels, lifecycle and failure recovery passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
