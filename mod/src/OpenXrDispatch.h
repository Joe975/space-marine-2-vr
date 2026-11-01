#pragma once
#include <Windows.h>
#include <d3d12.h>
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "OpenXrExtensions.h"

// Injectable API boundary. Unit tests never initialize a real XR runtime.
struct OpenXrDispatch {
#define SM2_XR_FUNCTIONS(X) \
    X(EnumerateInstanceExtensionProperties) X(CreateInstance) X(DestroyInstance) \
    X(GetInstanceProperties) X(GetSystem) X(GetSystemProperties) X(GetInstanceProcAddr) \
    X(EnumerateViewConfigurationViews) X(CreateSession) X(DestroySession) \
    X(EnumerateReferenceSpaces) X(CreateReferenceSpace) X(DestroySpace) \
    X(PollEvent) X(BeginSession) X(EndSession) X(RequestExitSession) \
    X(LocateSpace) X(LocateViews) X(EnumerateSwapchainFormats) X(CreateSwapchain) \
    X(DestroySwapchain) X(EnumerateSwapchainImages) X(AcquireSwapchainImage) \
    X(WaitSwapchainImage) X(ReleaseSwapchainImage) X(WaitFrame) X(BeginFrame) X(EndFrame) \
    X(StringToPath) X(PathToString) X(CreateActionSet) X(DestroyActionSet) X(CreateAction) \
    X(SuggestInteractionProfileBindings) X(AttachSessionActionSets) X(SyncActions) \
    X(GetActionStateBoolean) X(GetActionStateFloat) X(GetActionStateVector2f) \
    X(CreateActionSpace) X(GetCurrentInteractionProfile) X(ApplyHapticFeedback) X(StopHapticFeedback)
#define SM2_XR_MEMBER(name) PFN_xr##name name{&xr##name};
    SM2_XR_FUNCTIONS(SM2_XR_MEMBER)
#undef SM2_XR_MEMBER
};
