#pragma once
#include "VrArmPose.h"
#include <span>

namespace vr_head {
// Prototype: collapse every mapped head/helmet/facial basis; native skin palettes need
// explicit descendant scales rather than relying on the head parent's scale alone.
// Native animation republishes original locals when off; head shadows also shrink.
inline std::optional<std::vector<vr_arm::Matrix>> hide(const std::vector<vr_arm::Matrix>& locals,
    const std::vector<int32_t>& parents,const std::vector<std::string>& names,std::span<const int16_t> mapping={}) {
    if(locals.size()!=parents.size() || locals.size()!=names.size()) return {};
    if(!mapping.empty() && mapping.size()!=locals.size()) return {};
    size_t head=names.size(),neck=names.size(),helmet=names.size();
    for(size_t i=0;i<names.size();++i) {
        auto choose=[&](size_t& index){if(index!=names.size()) return false;index=i;return true;};
        if((names[i]=="HEAD" && !choose(head)) || (names[i]=="NECK" && !choose(neck)) || (names[i]=="helmet" && !choose(helmet))) return {};
        if(parents[i]<-1 || parents[i]>=static_cast<int32_t>(i) || !vr_camera::rigid(locals[i])) return {};
    }
    if(head==names.size() || neck==names.size() || helmet==names.size() || parents[head]!=static_cast<int32_t>(neck) || parents[helmet]!=static_cast<int32_t>(head)) return {};
    if(!mapping.empty() && mapping[head]<0) return {};
    // A malformed rig must not make arms or torso inherit the hidden head scale.
    for(size_t i=0;i<names.size();++i) if(names[i].starts_with("ARM") || names[i].starts_with("HAND") || names[i].starts_with("BACK") || names[i]=="CENTRE") {
        for(int32_t p=static_cast<int32_t>(i);p>=0;p=parents[p]) if(p==static_cast<int32_t>(head)) return {};
    }
    auto out=locals;
    for(size_t i=0;i<locals.size();++i) {
        if(!mapping.empty() && mapping[i]<0) continue;
        bool descendant=false;
        for(int32_t p=static_cast<int32_t>(i);p>=0;p=parents[p]) if(p==static_cast<int32_t>(head)) {descendant=true;break;}
        if(!descendant) continue;
        for(size_t row=0;row<3;++row) for(size_t column=0;column<3;++column) out[i][row*4+column]*=0.001f;
        if(i!=head) for(size_t axis=0;axis<3;++axis) out[i][12+axis]*=0.001f;
    }
    return out;
}
}
