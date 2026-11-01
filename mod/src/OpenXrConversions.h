#pragma once
#include "VrProjection.h"
#include <openxr/openxr.h>
#include <optional>

namespace openxr_conversion {
inline std::optional<vr_camera::Matrix> matrix(const XrPosef& pose) {
    const auto [x,y,z,w]=pose.orientation;
    const float norm=x*x+y*y+z*z+w*w;
    if(!std::isfinite(norm) || std::abs(norm-1)>1e-4f) return {};
    // Transposed quaternion rotation: the mod stores row-vector transforms.
    vr_camera::Matrix m{1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w),0,
        2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w),0,
        2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y),0,
        pose.position.x,pose.position.y,pose.position.z,1};
    return vr_camera::rigid(m)?std::optional{m}:std::nullopt;
}
inline std::optional<XrPosef> pose(const vr_camera::Matrix& m) {
    if(!vr_camera::rigid(m)) return {};
    XrPosef p{}; auto& q=p.orientation;
    const float trace=m[0]+m[5]+m[10];
    if(trace>0) {
        const float s=2*std::sqrt(trace+1); q.w=.25f*s;
        q.x=(m[6]-m[9])/s; q.y=(m[8]-m[2])/s; q.z=(m[1]-m[4])/s;
    } else if(m[0]>m[5] && m[0]>m[10]) {
        const float s=2*std::sqrt(1+m[0]-m[5]-m[10]); q.x=.25f*s;
        q.w=(m[6]-m[9])/s; q.y=(m[4]+m[1])/s; q.z=(m[8]+m[2])/s;
    } else if(m[5]>m[10]) {
        const float s=2*std::sqrt(1+m[5]-m[0]-m[10]); q.y=.25f*s;
        q.w=(m[8]-m[2])/s; q.x=(m[4]+m[1])/s; q.z=(m[9]+m[6])/s;
    } else {
        const float s=2*std::sqrt(1+m[10]-m[0]-m[5]); q.z=.25f*s;
        q.w=(m[1]-m[4])/s; q.x=(m[8]+m[2])/s; q.y=(m[9]+m[6])/s;
    }
    p.position={m[12],m[13],m[14]}; return p;
}
inline vr_projection::Tangents tangents(const XrFovf& f) {
    return {std::tan(f.angleLeft),std::tan(f.angleRight),std::tan(f.angleDown),std::tan(f.angleUp)};
}
// Recover the exact source image frustum, not a newer predicted camera's frustum.
inline std::optional<XrFovf> fov(const vr_camera::Matrix& projection) {
    if(!std::isfinite(projection[0]) || !std::isfinite(projection[5]) || projection[0]<=0 || projection[5]<=0) return {};
    vr_projection::Tangents t{(-projection[8]-1)/projection[0],(1-projection[8])/projection[0],
        (-projection[9]-1)/projection[5],(1-projection[9])/projection[5]};
    if(!t.valid()) return {};
    return XrFovf{std::atan(t.left),std::atan(t.right),std::atan(t.top),std::atan(t.bottom)};
}
// Whether the headset is worn: XR_EXT_user_presence when the runtime reports it, else the
// session state. SteamVR holds the session at SYNCHRONIZED while the headset is off;
// VISIBLE (the dashboard) says nothing either way.
inline std::optional<bool> worn(std::optional<bool> user_present,XrSessionState state) {
    if(user_present) return user_present;
    if(state==XR_SESSION_STATE_FOCUSED) return true;
    if(state==XR_SESSION_STATE_IDLE || state==XR_SESSION_STATE_READY || state==XR_SESSION_STATE_SYNCHRONIZED) return false;
    return {};
}
}
