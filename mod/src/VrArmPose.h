#pragma once
#include "VrCameraPose.h"
#include <algorithm>
#include <optional>
#include <span>
#include <vector>
#include <string>

namespace vr_arm {
using Matrix=vr_camera::Matrix;
using Vector=std::array<float,3>;
inline Vector add(Vector a,Vector b) { for (int i=0;i<3;++i) { a[i]+=b[i]; } return a; }
inline Vector sub(Vector a,Vector b) { for (int i=0;i<3;++i) { a[i]-=b[i]; } return a; }
inline Vector scale(Vector a,float s) { for (auto& x:a) { x*=s; } return a; }
inline float dot(Vector a,Vector b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline Vector cross(Vector a,Vector b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
inline float length(Vector a) { return std::sqrt(dot(a,a)); }
inline bool finite(Vector a) { return std::all_of(a.begin(),a.end(),[](float v){return std::isfinite(v);}); }
inline Vector position(const Matrix& m) { return {m[12],m[13],m[14]}; }
inline void position(Matrix& m,Vector p) { for (int i=0;i<3;++i) { m[12+i]=p[i]; } }
inline Vector rotate(Vector p,const Matrix& m) {
    return {p[0]*m[0]+p[1]*m[4]+p[2]*m[8],p[0]*m[1]+p[1]*m[5]+p[2]*m[9],p[0]*m[2]+p[1]*m[6]+p[2]*m[10]};
}
// Native skeleton rotations are proper rotations. Camera compose deliberately flips the
// third output row for forward-facing camera storage; that flip must never reach a wrist.
inline std::optional<Matrix> wrist_world(const Matrix& camera_anchor,const Matrix& controller,
    const Matrix& grip_offset,float units_per_metre) {
    Matrix anchor{};
    if (!vr_camera::level_anchor(camera_anchor,anchor,false) || !vr_camera::rigid(controller) ||
        !vr_camera::rigid(grip_offset) || !std::isfinite(units_per_metre) || units_per_metre<0.01f || units_per_metre>100) { return std::nullopt; }
    for (int i=0;i<3;++i) { anchor[8+i]*=-1; }
    if (!vr_camera::rigid(anchor)) { return std::nullopt; }
    auto relative=vr_camera::multiply(grip_offset,controller);
    for (int i=0;i<3;++i) { relative[12+i]*=units_per_metre; }
    auto world=vr_camera::multiply(relative,anchor);
    if (!vr_camera::rigid(world)) { return std::nullopt; }
    return world;
}
inline Vector orthogonal(Vector unit) {
    Vector axis{}; const auto i=static_cast<size_t>(std::min_element(unit.begin(),unit.end(),[](float a,float b){return std::abs(a)<std::abs(b);})-unit.begin());
    axis[i]=1; auto result=cross(unit,axis); return scale(result,1/length(result));
}
inline Matrix turn(Vector from,Vector to) {
    from=scale(from,1/length(from)); to=scale(to,1/length(to));
    const auto cosine=std::clamp(dot(from,to),-1.0f,1.0f);
    auto out=vr_camera::IDENTITY;
    if (cosine<-0.99999f) {
        const auto axis=orthogonal(from);
        for (int i=0;i<3;++i) for (int j=0;j<3;++j) { out[i*4+j]=2*axis[i]*axis[j]-(i==j?1.0f:0.0f); }
        return out;
    }
    const auto v=cross(from,to);
    const float k[3][3]{{0,v[2],-v[1]},{-v[2],0,v[0]},{v[1],-v[0],0}};
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) {
        float k2{}; for (int a=0;a<3;++a) { k2+=k[i][a]*k[a][j]; }
        out[i*4+j]+=(k[i][j]+k2/(1+cosine));
    }
    return out;
}
inline bool hierarchy(std::span<const Matrix> locals,std::span<const int32_t> parents,const Matrix& root,std::vector<Matrix>& output) {
    if (locals.empty() || locals.size()!=parents.size() || locals.size()>1024 || !vr_camera::rigid(root)) { return false; }
    std::vector<Matrix> result; result.reserve(locals.size());
    for (size_t i=0;i<locals.size();++i) {
        if (!vr_camera::rigid(locals[i]) || parents[i]<-1 || parents[i]>=static_cast<int32_t>(i)) { return false; }
        auto world=vr_camera::multiply(locals[i],parents[i]<0?root:result[parents[i]]);
        if (!vr_camera::rigid(world)) { return false; }
        result.push_back(world);
    }
    output=std::move(result); return true;
}
// World position of one bone through its parent chain only, as hierarchy() would place it.
// local(i) gives bone i's local matrix (an animation publication over the bind pose).
template<class Local> std::optional<Vector> chain_position(std::span<const int32_t> parents,const Matrix& root,size_t index,Local local) {
    if (index>=parents.size() || parents.size()>1024 || !vr_camera::rigid(root)) { return std::nullopt; }
    std::vector<size_t> chain;
    for (int32_t i=static_cast<int32_t>(index);i>=0;i=parents[i]) {
        if (parents[i]<-1 || parents[i]>=i) { return std::nullopt; }
        chain.push_back(static_cast<size_t>(i));
    }
    auto world=root;
    for (auto i=chain.rbegin();i!=chain.rend();++i) {
        const Matrix& m=local(*i);
        if (!vr_camera::rigid(m)) { return std::nullopt; }
        world=vr_camera::multiply(m,world);
    }
    return position(world);
}
struct Chain { size_t shoulder{},elbow{},wrist{}; };
struct Companion {
    size_t bone{},follow{}; // e.g. the right glove's separate elbow/wrist branch.
    // Forearm armour is an elbow child at the wrist endpoint. Preserve its native
    // elbow-relative rotation, but carry it with any tracked wrist extension.
    bool endpoint_only{};
    // The Bulwark's storm shield hangs off shield_pos, an elbow child (playtest 26 Oct, rig
    // 116 under ARM2L). The solved forearm keeps the animation's roll, so turning the wrist
    // never turned the shield: "it's mounted to my forearm rather than my hand, hard to
    // control the angle". A twist companion turns about the forearm axis by however much the
    // tracked hand is rolled about that axis beyond the animation's own hand roll, the way a
    // strapped shield turns with the forearm's pronation. Wrist bend (swing) is not applied.
    bool twist_only{};
};
// The part of rotation `rotation` (translation ignored) that turns about unit `axis`, in the
// row-vector convention used here (v' = v*M): rotation = twist*swing, swing the smallest
// turn taking axis to axis*rotation.
inline Matrix twist_about(const Matrix& rotation,Vector axis) {
    auto r=rotation;position(r,{0,0,0});
    const auto moved=rotate(axis,r);
    if(!finite(moved) || length(moved)<.5f) return vr_camera::IDENTITY;
    auto twist=vr_camera::multiply(r,vr_camera::inverse(turn(axis,moved)));
    position(twist,{0,0,0});
    return twist;
}
struct Result { std::vector<Matrix> locals; float target_error{},upper_length{},lower_length{}; bool reach_limited{}; };
// Solve in native world space, then derive local rotations. Keep torso and every unrelated
// local transform byte-for-byte. Companion offsets retain native glove attachment geometry.
inline std::optional<Result> solve(std::span<const Matrix> locals,std::span<const int32_t> parents,
    const Matrix& root,Chain chain,const Matrix& target,Vector elbow_pole,std::span<const Companion> companions={},bool tracked_wrist=false) {
    std::vector<Matrix> world;
    if (!hierarchy(locals,parents,root,world) || !vr_camera::rigid(target) || !finite(elbow_pole) ||
        chain.shoulder>=locals.size() || chain.elbow>=locals.size() || chain.wrist>=locals.size() ||
        parents[chain.elbow]!=static_cast<int32_t>(chain.shoulder) || parents[chain.wrist]!=static_cast<int32_t>(chain.elbow)) { return std::nullopt; }
    const auto a=position(world[chain.shoulder]),b=position(world[chain.elbow]),c=position(world[chain.wrist]);
    const auto upper=length(sub(b,a)),lower=length(sub(c,b));
    if (!std::isfinite(upper+lower) || upper<1e-4f || lower<1e-4f) { return std::nullopt; }
    auto delta=sub(position(target),a); const auto requested=length(delta);
    auto direction=requested>1e-5f?scale(delta,1/requested):scale(sub(c,a),1/std::max(length(sub(c,a)),1e-5f));
    if (length(direction)<0.9f) { direction=scale(sub(b,a),1/upper); }
    const auto margin=std::min(upper,lower)*1e-4f;
    const auto distance=std::clamp(requested,std::abs(upper-lower)+margin,upper+lower-margin);
    auto pole=sub(elbow_pole,scale(direction,dot(elbow_pole,direction)));
    if (length(pole)<1e-5f) { pole=sub(sub(b,a),scale(direction,dot(sub(b,a),direction))); }
    if (length(pole)<1e-5f) { pole=orthogonal(direction); }
    pole=scale(pole,1/length(pole));
    const auto along=(upper*upper-lower*lower+distance*distance)/(2*distance);
    auto elbow=add(a,add(scale(direction,along),scale(pole,std::sqrt(std::max(0.0f,upper*upper-along*along)))));
    // Rendered VR hands must remain at the controller even when native animation
    // moves the shoulder out of reach. Retain a natural elbow solve and extend
    // both segments in proportion to the real wrist, so a long reach is a longer
    // arm, not a longer forearm (docs/BODY_SCALE.md); never clamp the held weapon
    // towards the face.
    if(tracked_wrist && requested>distance) elbow=add(elbow,scale(direction,(requested-distance)*upper/(upper+lower)));
    const auto wrist=tracked_wrist?position(target):add(a,scale(direction,distance));
    const auto upper_turn=turn(sub(b,a),sub(elbow,a));
    const auto lower_turn=turn(rotate(sub(c,b),upper_turn),sub(wrist,elbow));
    auto new_a=vr_camera::multiply(world[chain.shoulder],upper_turn); position(new_a,a);
    auto new_b=vr_camera::multiply(vr_camera::multiply(world[chain.elbow],upper_turn),lower_turn); position(new_b,elbow);
    auto new_c=target; position(new_c,wrist);
    auto changed=world; changed[chain.shoulder]=new_a; changed[chain.elbow]=new_b; changed[chain.wrist]=new_c;
    Result result; result.locals.assign(locals.begin(),locals.end());
    for (auto i:{chain.shoulder,chain.elbow,chain.wrist}) {
        result.locals[i]=vr_camera::multiply(changed[i],vr_camera::inverse(parents[i]<0?root:changed[parents[i]]));
    }
    for (const auto& companion:companions) {
        if (companion.bone>=locals.size() || companion.bone==chain.shoulder || companion.bone==chain.elbow || companion.bone==chain.wrist ||
            (companion.follow!=chain.elbow && companion.follow!=chain.wrist) || parents[companion.bone]<0) { return std::nullopt; }
        if(companion.twist_only) {
            if(companion.follow!=chain.wrist || parents[companion.bone]!=static_cast<int32_t>(chain.elbow)) return std::nullopt;
            // Elbow-local frame: the forearm axis is the wrist's native local translation (the
            // solve turns the forearm onto the tracked wrist, so it is the same axis after it).
            auto axis=position(locals[chain.wrist]);
            if(length(axis)<1e-5f) return std::nullopt;
            axis=scale(axis,1/length(axis));
            // How the tracked hand is rolled relative to the forearm, beyond the animation's.
            auto native=locals[chain.wrist],tracked=result.locals[chain.wrist];position(native,{0,0,0});position(tracked,{0,0,0});
            const auto twist=twist_about(vr_camera::multiply(vr_camera::inverse(native),tracked),axis);
            result.locals[companion.bone]=vr_camera::multiply(result.locals[companion.bone],twist);
            continue;
        }
        // Recompute parents after each companion; order is the actual hierarchy order.
        if (!hierarchy(result.locals,parents,root,changed)) { return std::nullopt; }
        auto followed=changed[companion.bone];
        if(companion.endpoint_only) {
            if(companion.follow!=chain.wrist || parents[companion.bone]!=static_cast<int32_t>(chain.elbow)) return std::nullopt;
            // HAND's original local translation describes the rigid forearm end
            // after the elbow turns. HAND itself may be farther away (or closer)
            // to honour tracking. Move the sibling armour end by that difference,
            // without making the bracer inherit independent palm rotation.
            const auto rigid_end=vr_camera::multiply(locals[chain.wrist],changed[chain.elbow]);
            const auto extension=sub(position(changed[chain.wrist]),position(rigid_end));
            if(length(extension)<=margin) continue;
            position(followed,add(position(followed),extension));
        } else {
            const auto offset=vr_camera::multiply(world[companion.bone],vr_camera::inverse(world[companion.follow]));
            followed=vr_camera::multiply(offset,changed[companion.follow]);
        }
        result.locals[companion.bone]=vr_camera::multiply(followed,vr_camera::inverse(changed[parents[companion.bone]]));
    }
    if (!hierarchy(result.locals,parents,root,changed)) { return std::nullopt; }
    result.upper_length=upper; result.lower_length=lower;
    result.target_error=length(sub(position(target),wrist)); result.reach_limited=std::abs(distance-requested)>margin;
    return result;
}
inline std::optional<Chain> chain(std::span<const std::string> names,std::span<const int32_t> parents,size_t hand) {
    if (hand>1 || names.size()!=parents.size()) { return {}; }
    const std::array<std::string,3> wanted=hand==0?std::array<std::string,3>{"ARM1L","ARM2L","HANDL"}:std::array<std::string,3>{"ARM1R","ARM2R","HANDR"};
    std::array<size_t,3> indices{};
    for (size_t i=0;i<3;++i) {
        auto found=std::find(names.begin(),names.end(),wanted[i]); if (found==names.end() || std::count(names.begin(),names.end(),wanted[i])!=1) { return {}; }
        indices[i]=static_cast<size_t>(found-names.begin());
    }
    if (parents[indices[1]]!=static_cast<int32_t>(indices[0]) || parents[indices[2]]!=static_cast<int32_t>(indices[1])) { return {}; }
    return Chain{indices[0],indices[1],indices[2]};
}
inline std::optional<Result> solve_named(std::span<const Matrix> locals,std::span<const int32_t> parents,
    std::span<const std::string> names,const Matrix& root,size_t hand,const Matrix& target,Vector pole,bool tracked_wrist=false,bool shield_twist=true) {
    const auto arm=chain(names,parents,hand); if (!arm) { return {}; }
    std::vector<Companion> companions;
    if (hand==1) {
        const auto elbow=std::find(names.begin(),names.end(),"ARM2R_glove"),wrist=std::find(names.begin(),names.end(),"HANDR_glove");
        if ((elbow==names.end())!=(wrist==names.end())) { return {}; }
        if (elbow!=names.end()) {
            const auto e=static_cast<size_t>(elbow-names.begin()),w=static_cast<size_t>(wrist-names.begin());
            if (std::count(names.begin(),names.end(),"ARM2R_glove")!=1 || std::count(names.begin(),names.end(),"HANDR_glove")!=1 ||
                parents[e]!=static_cast<int32_t>(arm->shoulder) || parents[w]!=static_cast<int32_t>(e) || e>=w) { return {}; }
            companions={{e,arm->elbow},{w,arm->wrist}};
        }
    }
    const std::string forearm=hand==0?"forearm_L":"forearm_R";
    if(const auto found=std::find(names.begin(),names.end(),forearm);found!=names.end()) {
        const auto i=static_cast<size_t>(found-names.begin());
        if(std::count(names.begin(),names.end(),forearm)!=1 || parents[i]!=static_cast<int32_t>(arm->elbow)) return {};
        companions.push_back({i,arm->wrist,true});
    }
    // The storm shield's mount (left forearm). Every marine rig has it; only the Bulwark hangs
    // a shield on it. Anywhere else (or twice) it is left to the animation: the arm still solves.
    if(hand==0 && shield_twist) if(const auto found=std::find(names.begin(),names.end(),"shield_pos");found!=names.end()) {
        const auto i=static_cast<size_t>(found-names.begin());
        if(std::count(names.begin(),names.end(),"shield_pos")==1 && parents[i]==static_cast<int32_t>(arm->elbow))
            companions.push_back({.bone=i,.follow=arm->wrist,.twist_only=true});
    }
    return solve(locals,parents,root,*arm,target,pole,companions,tracked_wrist);
}
} // namespace vr_arm
