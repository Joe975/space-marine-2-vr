#pragma once
#include "VrArmPose.h"
#include <DirectXMath.h>

namespace vr_finger {
// SteamVR order: thumb, index, middle, ring, pinky. Zero is open, one is curled.
using Curl=std::array<float,5>;
inline bool valid(const Curl& value) {
    return std::all_of(value.begin(),value.end(),[](float v){return std::isfinite(v) && v>=0 && v<=1;});
}
inline vr_arm::Matrix blend(const vr_arm::Matrix& open,const vr_arm::Matrix& closed,float curl) {
    if (curl==1) { return closed; }
    auto result=closed;
    if (curl==0) { for(int r=0;r<3;++r)for(int c=0;c<3;++c)result[r*4+c]=open[r*4+c];return result; }
    DirectX::XMFLOAT4X4 a{},b{},out{};
    std::copy(open.begin(),open.end(),&a.m[0][0]);std::copy(closed.begin(),closed.end(),&b.m[0][0]);
    const auto qa=DirectX::XMQuaternionRotationMatrix(DirectX::XMLoadFloat4x4(&a));
    const auto qb=DirectX::XMQuaternionRotationMatrix(DirectX::XMLoadFloat4x4(&b));
    DirectX::XMStoreFloat4x4(&out,DirectX::XMMatrixRotationQuaternion(DirectX::XMQuaternionSlerp(qa,qb,curl)));
    for(int r=0;r<3;++r)for(int c=0;c<3;++c)result[r*4+c]=out.m[r][c];
    return result; // Keep authored native translations/segment lengths exactly.
}
struct Result { std::vector<vr_arm::Matrix> locals; size_t bones{}; float max_open_angle_degrees{}; };
inline std::optional<Result> solve(std::span<const vr_arm::Matrix> native,std::span<const vr_arm::Matrix> bind,
    std::span<const int32_t> parents,std::span<const std::string> names,size_t hand,const Curl& curls) {
    if (hand>1 || !valid(curls) || native.size()!=bind.size() || native.size()!=parents.size() || native.size()!=names.size()) { return {}; }
    Result result;result.locals.assign(native.begin(),native.end());
    const auto find=[&](const std::string& name)->std::optional<size_t>{
        const auto it=std::find(names.begin(),names.end(),name);
        if(it==names.end() || std::count(names.begin(),names.end(),name)!=1) { return {}; }
        return static_cast<size_t>(it-names.begin());
    };
    const auto prefix=hand==0?"L":"R";
    const auto wrist=find(hand==0?"HANDL":"HANDR");if(!wrist) { return {}; }
    const auto glove=find("HANDR_glove");
    if(hand==1 && std::count(names.begin(),names.end(),"HANDR_glove")>0 && !glove) { return {}; }
    // A present gauntlet must have complete fingers; do not leave its shell as a fist.
    for(size_t branch=0;branch<(hand==1 && glove?2u:1u);++branch) {
        for(size_t finger=0;finger<5;++finger) {
            size_t parent=branch?*glove:*wrist;
            for(size_t segment=1;segment<=3;++segment) {
                const auto name=std::string{prefix}+std::to_string(finger+1)+"_fing"+std::to_string(segment)+(branch?"_glove":"");
                const auto bone=find(name);
                if(!bone || parents[*bone]!=static_cast<int32_t>(parent) || !vr_camera::rigid(native[*bone]) || !vr_camera::rigid(bind[*bone])) { return {}; }
                result.locals[*bone]=blend(bind[*bone],native[*bone],curls[finger]);
                if(!vr_camera::rigid(result.locals[*bone])) { return {}; }
                const auto delta=vr_camera::multiply(native[*bone],vr_camera::inverse(bind[*bone]));
                const float cosine=std::clamp((delta[0]+delta[5]+delta[10]-1)*0.5f,-1.0f,1.0f);
                result.max_open_angle_degrees=std::max(result.max_open_angle_degrees,std::acos(cosine)*57.2957795f);
                ++result.bones;parent=*bone;
            }
        }
    }
    return result;
}
}
