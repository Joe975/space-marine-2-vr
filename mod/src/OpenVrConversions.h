#pragma once
#include "VrCameraPose.h"
#include "VrProjection.h"
#include "VrFingerPose.h"
#include <openvr.h>
#include <optional>
#include <span>

namespace openvr_conversion {
// Whether the headset is worn, from the HMD's activity level (the Index proximity sensor
// puts it in standby once it comes off). Idle and the timeouts say nothing either way.
inline std::optional<bool> worn(vr::EDeviceActivityLevel level) {
    if(level==vr::k_EDeviceActivityLevel_UserInteraction) return true;
    if(level==vr::k_EDeviceActivityLevel_Standby) return false;
    return {};
}
inline vr_camera::Matrix from_matrix(const vr::HmdMatrix34_t& m) {
    auto out = vr_camera::IDENTITY;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) { out[c*4+r] = m.m[r][c]; }
    return out;
}
inline vr::HmdMatrix34_t to_matrix(const vr_camera::Matrix& m) {
    vr::HmdMatrix34_t out{};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) { out.m[r][c] = m[c*4+r]; }
    return out;
}
inline vr_projection::Tangents tangents(float left,float right,float top,float bottom) { return {left,right,-bottom,-top}; }
inline std::optional<vr_finger::Curl> fingers(const vr::VRSkeletalSummaryData_t& summary,
    vr::TrackedDeviceIndex_t origin,vr::TrackedDeviceIndex_t role) {
    if(origin!=role || role>=vr::k_unMaxTrackedDeviceCount) { return {}; }
    vr_finger::Curl curls{};std::copy_n(summary.flFingerCurl,5,curls.begin());
    return vr_finger::valid(curls)?std::optional{curls}:std::nullopt;
}
// Roles are resolved anew each sample: waking/reassigned devices must not use stale indices.
// Raw controller origins are diagnostic targets; a calibrated controller-to-wrist offset is
// still required before these can drive a character's hands.
inline std::array<std::optional<vr_camera::Matrix>,2> controllers(
    std::span<const vr::TrackedDevicePose_t> poses,const std::array<vr::TrackedDeviceIndex_t,2>& roles) {
    std::array<std::optional<vr_camera::Matrix>,2> out{};
    if (roles[0] == roles[1]) { return out; }
    for (size_t hand=0;hand<out.size();++hand) {
        if (roles[hand] >= poses.size()) { continue; }
        const auto& pose=poses[roles[hand]];
        if (!pose.bDeviceIsConnected || !pose.bPoseIsValid || pose.eTrackingResult != vr::TrackingResult_Running_OK) { continue; }
        const auto matrix=from_matrix(pose.mDeviceToAbsoluteTracking);
        if (vr_camera::rigid(matrix)) { out[hand]=matrix; }
    }
    return out;
}
}
