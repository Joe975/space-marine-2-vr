#pragma once
enum class VrRuntimeChoice { disabled,openvr,openxr,invalid };
inline VrRuntimeChoice vr_runtime_choice(bool openvr,bool openxr,bool synthetic) {
    if((openvr && openxr) || (synthetic && (openvr || openxr))) return VrRuntimeChoice::invalid;
    return openxr?VrRuntimeChoice::openxr:openvr?VrRuntimeChoice::openvr:VrRuntimeChoice::disabled;
}
