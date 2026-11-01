#pragma once
#include <openxr/openxr.h>

// The framework pins OpenXR 1.0.22 headers, which predate these two extensions. Definitions
// are copied from OpenXR 1.1.63 (refs/OpenXR-SDK) and only apply when the header lacks them.
#ifndef XR_EXT_user_presence
#define XR_EXT_user_presence 1
#define XR_EXT_USER_PRESENCE_EXTENSION_NAME "XR_EXT_user_presence"
inline constexpr auto XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT=static_cast<XrStructureType>(1000470000);
typedef struct XrEventDataUserPresenceChangedEXT {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    XrSession session;
    XrBool32 isUserPresent;
} XrEventDataUserPresenceChangedEXT;
#endif

#ifndef XR_EXT_hand_tracking_data_source
#define XR_EXT_hand_tracking_data_source 1
#define XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME "XR_EXT_hand_tracking_data_source"
inline constexpr auto XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT=static_cast<XrStructureType>(1000428000);
typedef enum XrHandTrackingDataSourceEXT {
    XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT = 1,
    XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT = 2,
    XR_HAND_TRACKING_DATA_SOURCE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrHandTrackingDataSourceEXT;
// Extends XrHandTrackerCreateInfoEXT.
typedef struct XrHandTrackingDataSourceInfoEXT {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    uint32_t requestedDataSourceCount;
    XrHandTrackingDataSourceEXT* requestedDataSources;
} XrHandTrackingDataSourceInfoEXT;
#endif
