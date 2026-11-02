#pragma once
#include "VrUprightTorso.h"
#include <span>

// Camera mode keeps where the player was looking (Joe, 2 Nov: "I'd like my head direction to be
// kept for the pose"; docs/VIEWER_MODE.md). First person hides the head, so nothing ever turned it:
// the game animates it on its own and the freeze held that. In the publications up to the freeze
// the neck and head turn to the headset's gaze, as render bones only, like the arms; the last
// publication before the pause is the one that stays on screen.
namespace vr_head_aim {
using Matrix=vr_arm::Matrix;
using Vector=vr_arm::Vector;

// The direction the Marine's eyes look, in the HEAD bone's axes: +x up the neck, +y out of the
// face, +z to the Marine's left. BACKup and NECK use the same axes. From the rig dump of 13 Oct
// (build/arms_owned_rig_first.json): the x row of eye_L and of eye_R. It sits 11 degrees above +y,
// and in that dump's idle stance it points level, straight ahead.
inline constexpr Vector FACE{.1934254f,.9811150f,0};

// How far the head goes from the chest, in the chest's own axes. A gaze past these stops at them:
// a head turned further would sink into the pauldrons. Not tuned in a headset yet.
struct Limits {
    float yaw_degrees{70},up_degrees{35},down_degrees{45};
    float neck_share{.4f}; // [0,1] of the turn taken at the neck, so the head doesn't pivot on a stiff neck.
    bool valid() const {
        return std::isfinite(yaw_degrees) && yaw_degrees>=0 && yaw_degrees<=90 && std::isfinite(up_degrees) && up_degrees>=0 && up_degrees<=90 &&
            std::isfinite(down_degrees) && down_degrees>=0 && down_degrees<=90 && std::isfinite(neck_share) && neck_share>=0 && neck_share<=1;
    }
};
struct Result {
    std::vector<Matrix> locals;
    float turned_degrees{}; // How far the eyes turned from the animation.
    float yaw_degrees{},pitch_degrees{}; // The head from the chest as posed: left and up are positive.
    bool limited{}; // The gaze was past Limits.
};

// The headset's gaze in the world: the eyes' forward in vr_camera::compose, without the eye offsets.
inline std::optional<Vector> gaze_world(const Matrix& anchor,const Matrix& relative_head) {
    Matrix level{};
    if(!vr_camera::level_anchor(anchor,level,false) || !vr_camera::rigid(relative_head)) return std::nullopt;
    const Vector back{relative_head[8],relative_head[9],relative_head[10]};
    Vector out{};
    for(int j=0;j<3;++j) out[j]=-(back[0]*level[j]+back[1]*level[4+j]-back[2]*level[8+j]);
    return vr_arm::finite(out) && vr_arm::length(out)>.5f?std::optional{vr_arm::scale(out,1/vr_arm::length(out))}:std::nullopt;
}

inline std::optional<size_t> only(std::span<const std::string> names,const char* name) {
    if(std::count(names.begin(),names.end(),name)!=1) return std::nullopt;
    return static_cast<size_t>(std::find(names.begin(),names.end(),name)-names.begin());
}

// locals: the publication as it stands, rig order. Only NECK and HEAD change; everything above
// them follows, as everything above BACKdown follows vr_upright. Refused (nothing changes) on a
// rig that isn't laid out as FACE says, or whose neck or head the animation doesn't publish.
inline std::optional<Result> aim(std::span<const Matrix> locals,std::span<const int32_t> parents,std::span<const std::string> names,
    std::span<const int16_t> mapping,const Matrix& root,Vector gaze,const Limits& limits={}) {
    if(!limits.valid() || !vr_arm::finite(gaze) || vr_arm::length(gaze)<.5f || locals.size()!=parents.size() || locals.size()!=names.size() || mapping.size()!=locals.size()) return std::nullopt;
    const auto head=only(names,"HEAD"),neck=only(names,"NECK"),left=only(names,"eye_L"),right=only(names,"eye_R");
    if(!head || !neck || !left || !right) return std::nullopt;
    if(parents[*head]!=static_cast<int32_t>(*neck) || parents[*neck]<0 || parents[*left]!=static_cast<int32_t>(*head) || parents[*right]!=static_cast<int32_t>(*head) ||
       mapping[*head]<0 || mapping[*neck]<0) return std::nullopt;
    // The eyes sit in front of the head and to its left and right: FACE is the right axis for this rig.
    const auto l=vr_arm::position(locals[*left]),r=vr_arm::position(locals[*right]);
    if(!(l[1]>0 && r[1]>0 && l[2]>0 && r[2]<0)) return std::nullopt;
    std::vector<Matrix> world;
    if(!vr_arm::hierarchy(locals,parents,root,world)) return std::nullopt;
    const auto& chest=world[static_cast<size_t>(parents[*neck])];
    gaze=vr_arm::scale(gaze,1/vr_arm::length(gaze));
    // In the chest's axes: yaw left, pitch up.
    const auto wanted=vr_arm::rotate(gaze,vr_camera::inverse(chest));
    const float degrees=57.2957795f;
    const float yaw=std::atan2(wanted[2],wanted[1])*degrees,pitch=std::asin(std::clamp(wanted[0],-1.f,1.f))*degrees;
    const float held_yaw=std::clamp(yaw,-limits.yaw_degrees,limits.yaw_degrees),held_pitch=std::clamp(pitch,-limits.down_degrees,limits.up_degrees);
    Result out;out.locals.assign(locals.begin(),locals.end());
    out.yaw_degrees=held_yaw;out.pitch_degrees=held_pitch;out.limited=held_yaw!=yaw || held_pitch!=pitch;
    const float cos_pitch=std::cos(held_pitch/degrees);
    const auto target=vr_arm::rotate({std::sin(held_pitch/degrees),cos_pitch*std::cos(held_yaw/degrees),cos_pitch*std::sin(held_yaw/degrees)},chest);
    const auto face=vr_arm::rotate(FACE,world[*head]);
    const auto axis=vr_arm::cross(face,target);const float sine=vr_arm::length(axis);
    const float angle=std::atan2(sine,vr_arm::dot(face,target));
    if(!(sine>1e-5f)) return out; // Already there. (Directly behind can't be: the limits stop short of it.)
    out.turned_degrees=angle*degrees;
    // The neck takes its share about its own pivot, then the head finishes the turn from where
    // that left it, so the eyes end on the target exactly.
    auto turned_neck=vr_camera::multiply(world[*neck],vr_upright::rotation(vr_arm::scale(axis,1/sine),angle*limits.neck_share));
    vr_arm::position(turned_neck,vr_arm::position(world[*neck]));
    const auto carried=vr_camera::multiply(locals[*head],turned_neck);
    auto turned_head=vr_camera::multiply(carried,vr_arm::turn(vr_arm::rotate(FACE,carried),target));
    vr_arm::position(turned_head,vr_arm::position(carried));
    if(!vr_camera::rigid(turned_neck) || !vr_camera::rigid(turned_head)) return std::nullopt;
    out.locals[*neck]=vr_camera::multiply(turned_neck,vr_camera::inverse(world[static_cast<size_t>(parents[*neck])]));
    out.locals[*head]=vr_camera::multiply(turned_head,vr_camera::inverse(turned_neck));
    // Both turn about their own pivots. The world is hundreds of units from the origin, so take
    // the offsets from the animation rather than back from world positions, which round.
    vr_arm::position(out.locals[*neck],vr_arm::position(locals[*neck]));
    vr_arm::position(out.locals[*head],vr_arm::position(locals[*head]));
    return out;
}
} // namespace vr_head_aim
