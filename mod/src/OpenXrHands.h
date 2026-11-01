#pragma once
#include "VrCameraPose.h"
#include "VrFingerPose.h"
#include <openxr/openxr.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <istream>
#include <optional>
#include <span>
#include <sstream>
#include <string>

namespace openxr_hands {
// Every hand calibration, holster, swing and aim threshold is expressed relative to the
// raw OpenVR controller origin. OpenXR only exposes the grip pose, so the backend converts
// grip -> raw-equivalent origin and VrSession sees the numbers it was tuned on.
// grip_in_raw is the grip frame expressed in the raw device frame (render-model style:
// metres + XYZ degrees, row-vector X then Y then Z like EngineArmDriver::grip).
struct GripOffset {
    std::array<std::array<float,3>,2> metres{},degrees{};
    bool measured{};
    bool operator==(const GripOffset&) const = default;
    // Unmeasured candidate: the installed Index render model's "grip" component
    // (valve_controller_knu_1_0_{left,right}.json). Whether SteamVR's /input/grip/pose
    // equals this component is unverified; measure it (docs/OPENXR_MIGRATION_PLAN.md phase 0).
    static GripOffset index_render_model() {
        GripOffset o;o.metres={{{0,-.015f,.13f},{0,-.015f,.13f}}};
        o.degrees={{{15.392f,-2.071f,.303f},{15.392f,2.071f,-.303f}}};return o;
    }
    static vr_camera::Matrix rotation(const std::array<float,3>& degrees) {
        auto out=vr_camera::IDENTITY;
        for(int i=0;i<3;++i) {
            const auto a=degrees[i]*0.01745329252f;auto r=vr_camera::IDENTITY;
            const int j=(i+1)%3,k=(i+2)%3;r[j*4+j]=r[k*4+k]=std::cos(a);r[j*4+k]=std::sin(a);r[k*4+j]=-std::sin(a);
            out=vr_camera::multiply(out,r);
        }
        return out;
    }
    vr_camera::Matrix grip_in_raw(size_t hand) const {
        auto m=rotation(degrees[hand]);for(int i=0;i<3;++i) m[12+i]=metres[hand][i];return m;
    }
    // raw_world = raw_in_grip * grip_world (row vectors).
    std::optional<vr_camera::Matrix> raw(size_t hand,const vr_camera::Matrix& grip_world) const {
        if(hand>1 || !vr_camera::rigid(grip_world)) return {};
        const auto out=vr_camera::multiply(vr_camera::inverse(grip_in_raw(hand)),grip_world);
        return vr_camera::rigid(out)?std::optional{out}:std::nullopt;
    }
    bool valid() const {
        for(size_t h=0;h<2;++h) for(int i=0;i<3;++i)
            if(!std::isfinite(metres[h][i]) || std::abs(metres[h][i])>.5f || !std::isfinite(degrees[h][i]) || std::abs(degrees[h][i])>180) return false;
        return true;
    }
    // Keys: {left,right}_grip_in_raw_{metres,degrees}=x y z, measured=0|1. All required;
    // unknown, duplicate or malformed lines reject the whole file.
    static std::optional<GripOffset> read(std::istream& input) {
        GripOffset o;std::array<bool,5> seen{};std::string line;
        static constexpr const char* KEYS[]{"left_grip_in_raw_metres","left_grip_in_raw_degrees","right_grip_in_raw_metres","right_grip_in_raw_degrees","measured"};
        while(std::getline(input,line)) {
            const auto first=line.find_first_not_of(" \t\r");
            if(first==std::string::npos || line[first]=='#') continue;
            const auto split=line.find('=');if(split==std::string::npos) return {};
            const auto name=line.substr(first,split-first);std::istringstream values{line.substr(split+1)};
            size_t key=5;for(size_t k=0;k<5;++k) if(name==KEYS[k]) key=k;
            if(key==5 || seen[key]) return {};
            seen[key]=true;
            if(key==4) {int flag{};if(!(values>>flag) || (flag!=0 && flag!=1)) return {};o.measured=flag;}
            else {
                auto& target=key%2?o.degrees[key/2]:o.metres[key/2];
                for(auto& v:target) if(!(values>>v)) return {};
            }
            if(!(values>>std::ws).eof()) return {};
        }
        for(bool s:seen) if(!s) return {};
        return o.valid()?std::optional{o}:std::nullopt;
    }
};

// XR_EXT_hand_tracking joint poses -> SteamVR-style curls (thumb..pinky, 0 open, 1 curled).
// Joint -Z points toward the fingertip and +Y out of the back of the hand, so flexion
// turns a child's forward axis toward its parent's -Y. Per-joint signed flexion is summed
// along each finger (abduction is ignored) and normalised by FULL_CURL_RADIANS.
// The normalisation is a starting point to fit against OpenVR skeletal summaries.
inline constexpr std::array<float,5> FULL_CURL_RADIANS{1.6f,4.0f,4.0f,4.0f,4.0f};
struct Vector { float x,y,z; };
inline Vector rotate(const XrQuaternionf& q,Vector v) {
    // v + 2w(q x v) + 2 q x (q x v)
    const Vector u{q.x,q.y,q.z};
    const auto cross=[](Vector a,Vector b){return Vector{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
    const auto t=cross(u,v);const Vector t2{2*t.x,2*t.y,2*t.z};const auto c=cross(u,t2);
    return {v.x+q.w*t2.x+c.x,v.y+q.w*t2.y+c.y,v.z+q.w*t2.z+c.z};
}
inline float flexion(const XrQuaternionf& parent,const XrQuaternionf& child) {
    const auto forward=rotate(parent,{0,0,-1}),down=rotate(parent,{0,-1,0}),child_forward=rotate(child,{0,0,-1});
    const auto dot=[](Vector a,Vector b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    return std::atan2(dot(child_forward,down),dot(child_forward,forward));
}
// joints: XR_HAND_JOINT_COUNT_EXT (26) orientations in XrHandJointEXT order.
inline std::optional<vr_finger::Curl> curls(std::span<const XrHandJointLocationEXT> joints) {
    if(joints.size()!=XR_HAND_JOINT_COUNT_EXT) return {};
    for(const auto& j:joints) {
        if(!(j.locationFlags&XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return {};
        const auto& q=j.pose.orientation;const float n=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
        if(!std::isfinite(n) || std::abs(n-1)>1e-3f) return {};
    }
    // Chains from the metacarpal to the distal joint. Thumb has no intermediate joint.
    static constexpr std::array<std::array<int,4>,5> CHAINS{{
        {XR_HAND_JOINT_THUMB_METACARPAL_EXT,XR_HAND_JOINT_THUMB_PROXIMAL_EXT,XR_HAND_JOINT_THUMB_DISTAL_EXT,-1},
        {XR_HAND_JOINT_INDEX_METACARPAL_EXT,XR_HAND_JOINT_INDEX_PROXIMAL_EXT,XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT,XR_HAND_JOINT_INDEX_DISTAL_EXT},
        {XR_HAND_JOINT_MIDDLE_METACARPAL_EXT,XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT,XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT,XR_HAND_JOINT_MIDDLE_DISTAL_EXT},
        {XR_HAND_JOINT_RING_METACARPAL_EXT,XR_HAND_JOINT_RING_PROXIMAL_EXT,XR_HAND_JOINT_RING_INTERMEDIATE_EXT,XR_HAND_JOINT_RING_DISTAL_EXT},
        {XR_HAND_JOINT_LITTLE_METACARPAL_EXT,XR_HAND_JOINT_LITTLE_PROXIMAL_EXT,XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT,XR_HAND_JOINT_LITTLE_DISTAL_EXT}}};
    vr_finger::Curl out{};
    for(size_t finger=0;finger<5;++finger) {
        float total{};
        for(size_t n=1;n<4 && CHAINS[finger][n]>=0;++n)
            total+=flexion(joints[CHAINS[finger][n-1]].pose.orientation,joints[CHAINS[finger][n]].pose.orientation);
        out[finger]=std::clamp(total/FULL_CURL_RADIANS[finger],0.0f,1.0f);
    }
    return vr_finger::valid(out)?std::optional{out}:std::nullopt;
}
}
