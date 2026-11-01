#pragma once
#include "VrArmPose.h"
#include <charconv>
#include <chrono>
#include <string>

// Opt-in upright upper body (docs/BODY_CLIPPING.md). The run animation leans the spine
// forward about 10 degrees, which carries the head bone ~23 cm forward (playtest trace
// 18 Oct: HEAD +0.171 m standing vs +0.397 m running, root-forward). The tracked camera
// stays where the player's head is, so the helmet and pauldrons end up in front of the
// lenses. This rotates the lowest spine bone (BACKdown) about its own pivot so the
// spine line BACKdown->NECK points the way it does in the bind pose, in the character
// root's frame. Every child (head, clavicles, shoulders, arms) follows rigidly.
// Scope: published RENDER bones only (the animation publication buffer). The tracked VR
// camera, the character root/actor transform, the pelvis (CENTRE), the legs and the
// simulation are never changed.
//
// Twist (docs/PAULDRON.md): the native idle, sidearm and rifle stances also turn the upper
// body about the spine so the left shoulder leads (test room 21 Oct: 40-64 degrees; Joe's
// campaign 21 Oct: 49 degrees standing). The body faces the headset, so that put the left
// shoulder 16-25 cm in FRONT of the eyes and the pauldron across the lower view. After the
// lean, BACKdown also turns about the (corrected) spine line until the shoulder line
// ARM1L->ARM1R lies as in the bind pose: square to the body, like a real wearer's shoulders.
namespace vr_upright {
using Matrix=vr_arm::Matrix;
using Vector=vr_arm::Vector;
struct Settings {
    float strength{1};        // [0,1] share of the lean removed.
    // Physical sprint trace, 19 Oct: 55.87 degrees median, 78.75 peak. The old
    // 30-degree limit left the neck/shoulders leaning into the stationary tracked view.
    float max_degrees{90};    // [0,90] render-only correction; no inversion past horizontal. Also caps the twist.
    float deadband_degrees{2};// [0,10] leans (and twists) below this are left alone (idle breathing).
    float twist_strength{1};  // [0,1] share of the shoulder twist removed; 0 = lean only (the 21 Oct behaviour).
    bool valid() const { return std::isfinite(strength) && strength>=0 && strength<=1 && std::isfinite(max_degrees) && max_degrees>=0 && max_degrees<=90 &&
        std::isfinite(deadband_degrees) && deadband_degrees>=0 && deadband_degrees<=10 && std::isfinite(twist_strength) && twist_strength>=0 && twist_strength<=1; }
    bool operator==(const Settings&) const = default;
};
// Opt-in configuration from SM2VR_UPRIGHT_TORSO / SM2VR_UPRIGHT_STRENGTH / SM2VR_UPRIGHT_TWIST
// (nullopt = unset). Enabled only by exactly "1"; "0" or unset is off. Strength and twist must
// parse completely as numbers in [0,1]. Anything malformed leaves the correction off and says why.
struct Config { bool enabled{}; Settings settings; std::string error; };
inline bool parse_unit(const std::optional<std::string>& text,float& out) {
    if(!text) return true;
    float value{};const auto* first=text->data();const auto* last=first+text->size();
    const auto [end,code]=std::from_chars(first,last,value);
    if(text->empty() || code!=std::errc{} || end!=last || !std::isfinite(value) || value<0 || value>1) return false;
    out=value;return true;
}
inline Config parse_config(const std::optional<std::string>& enabled,const std::optional<std::string>& strength,
    const std::optional<std::string>& twist=std::nullopt) {
    Config c;
    if(enabled && *enabled!="0" && *enabled!="1") { c.error="SM2VR_UPRIGHT_TORSO must be 0 or 1";return c; }
    if(!parse_unit(strength,c.settings.strength)) { c.error="SM2VR_UPRIGHT_STRENGTH must be a number from 0 to 1";return c; }
    if(!parse_unit(twist,c.settings.twist_strength)) { c.error="SM2VR_UPRIGHT_TWIST must be a number from 0 to 1";return c; }
    c.enabled=enabled && *enabled=="1";
    return c;
}
struct Chain { size_t base{},top{}; };
struct Result { std::vector<Matrix> locals; float before_degrees{},after_degrees{}; float twist_before_degrees{},twist_after_degrees{}; bool corrected{}; };

inline std::optional<Chain> chain(std::span<const std::string> names,std::span<const int32_t> parents) {
    const auto find=[&](const char* name)->std::optional<size_t> {
        if(std::count(names.begin(),names.end(),name)!=1) return std::nullopt;
        return static_cast<size_t>(std::find(names.begin(),names.end(),name)-names.begin());
    };
    if(names.empty() || parents.size()!=names.size() || names.size()>4096) return std::nullopt;
    // Every parent must precede its child (the order vr_arm::hierarchy requires). That rules
    // out out-of-range parents and cycles before any traversal, so the walk below terminates.
    for(size_t i=0;i<parents.size();++i) if(parents[i]<-1 || parents[i]>=static_cast<int32_t>(i)) return std::nullopt;
    const auto base=find("BACKdown"),top=find("NECK");
    if(!base || !top || parents[*base]<0) return std::nullopt;
    // NECK must descend from BACKdown, so rotating the base moves the whole upper body.
    for(int32_t i=static_cast<int32_t>(*top);i>=0;i=parents[i]) if(static_cast<size_t>(i)==*base) return Chain{*base,*top};
    return std::nullopt;
}
inline Matrix rotation(Vector axis,float radians) {
    const float c=std::cos(radians),s=std::sin(radians),t=1-c;const auto [x,y,z]=axis;
    // Row-vector convention (v' = v*M), matching vr_camera::multiply and vr_arm::turn.
    return {t*x*x+c,t*x*y+s*z,t*x*z-s*y,0, t*x*y-s*z,t*y*y+c,t*y*z+s*x,0, t*x*z+s*y,t*y*z-s*x,t*z*z+c,0, 0,0,0,1};
}
inline float angle_degrees(Vector a,Vector b) {
    const float la=vr_arm::length(a),lb=vr_arm::length(b);
    if(!(la>1e-6f) || !(lb>1e-6f)) return 0;
    return std::acos(std::clamp(vr_arm::dot(a,b)/(la*lb),-1.0f,1.0f))*57.2957795f;
}
// Spine-line angle from its bind direction, in degrees, for diagnostics and the trace.
inline std::optional<float> lean(std::span<const Matrix> locals,std::span<const Matrix> bind,std::span<const int32_t> parents,
    std::span<const std::string> names,const Matrix& root) {
    const auto c=chain(names,parents);if(!c) return std::nullopt;
    std::vector<Matrix> world,reference;
    if(!vr_arm::hierarchy(locals,parents,root,world) || !vr_arm::hierarchy(bind,parents,root,reference)) return std::nullopt;
    return angle_degrees(vr_arm::sub(vr_arm::position(world[c->top]),vr_arm::position(world[c->base])),
        vr_arm::sub(vr_arm::position(reference[c->top]),vr_arm::position(reference[c->base])));
}
// The two arm roots, when both exist once and hang below BACKdown (so turning BACKdown moves
// them). A rig without them gets the lean correction only.
struct Shoulders { size_t left{},right{}; };
inline std::optional<Shoulders> shoulders(std::span<const std::string> names,std::span<const int32_t> parents,const Chain& c) {
    const auto below=[&](const char* name)->std::optional<size_t> {
        if(std::count(names.begin(),names.end(),name)!=1) return std::nullopt;
        const auto i=static_cast<size_t>(std::find(names.begin(),names.end(),name)-names.begin());
        for(int32_t j=parents[i];j>=0;j=parents[j]) if(static_cast<size_t>(j)==c.base) return i;
        return std::nullopt;
    };
    const auto l=below("ARM1L"),r=below("ARM1R");
    if(!l || !r) return std::nullopt;
    return Shoulders{*l,*r};
}
// Signed degrees that turn the posed shoulder line onto the bind shoulder line about the posed
// spine line (right-handed about BACKdown->NECK). Both lines are projected onto the plane
// across the spine first, so a lean never reads as a twist.
inline std::optional<float> twist_to_bind(std::span<const Matrix> world,std::span<const Matrix> reference,const Chain& c,const Shoulders& s) {
    auto axis=vr_arm::sub(vr_arm::position(world[c.top]),vr_arm::position(world[c.base]));
    const float axis_length=vr_arm::length(axis);if(!(axis_length>1e-6f)) return std::nullopt;
    axis=vr_arm::scale(axis,1/axis_length);
    const auto across=[&](Vector v){ return vr_arm::sub(v,vr_arm::scale(axis,vr_arm::dot(v,axis))); };
    const auto posed=across(vr_arm::sub(vr_arm::position(world[s.right]),vr_arm::position(world[s.left])));
    const auto bound=across(vr_arm::sub(vr_arm::position(reference[s.right]),vr_arm::position(reference[s.left])));
    if(!(vr_arm::length(posed)>1e-6f) || !(vr_arm::length(bound)>1e-6f)) return std::nullopt;
    return std::atan2(vr_arm::dot(vr_arm::cross(posed,bound),axis),vr_arm::dot(posed,bound))*57.2957795f;
}
// Shoulder twist away from the bind pose, signed degrees, for diagnostics and the trace.
// Positive: the posed line must turn right-handed about the spine (up) to square up.
inline std::optional<float> twist(std::span<const Matrix> locals,std::span<const Matrix> bind,std::span<const int32_t> parents,
    std::span<const std::string> names,const Matrix& root) {
    const auto c=chain(names,parents);if(!c || locals.size()!=bind.size()) return std::nullopt;
    const auto s=shoulders(names,parents,*c);if(!s) return std::nullopt;
    std::vector<Matrix> world,reference;
    if(!vr_arm::hierarchy(locals,parents,root,world) || !vr_arm::hierarchy(bind,parents,root,reference)) return std::nullopt;
    return twist_to_bind(world,reference,*c,*s);
}
// Turns BACKdown's world rotation by `degrees` about the unit `axis` through its own pivot.
inline bool turn_base(std::vector<Matrix>& locals,std::span<const Matrix> world,std::span<const int32_t> parents,size_t base,Vector axis,float degrees) {
    auto turned=vr_camera::multiply(world[base],rotation(axis,degrees/57.2957795f));
    vr_arm::position(turned,vr_arm::position(world[base]));
    if(!vr_camera::rigid(turned)) return false;
    locals[base]=vr_camera::multiply(turned,vr_camera::inverse(world[parents[base]]));
    return true;
}
inline std::optional<Result> correct(std::span<const Matrix> locals,std::span<const Matrix> bind,std::span<const int32_t> parents,
    std::span<const std::string> names,const Matrix& root,const Settings& settings) {
    const auto c=chain(names,parents);
    if(!c || !settings.valid() || locals.size()!=bind.size()) return std::nullopt;
    std::vector<Matrix> world,reference;
    if(!vr_arm::hierarchy(locals,parents,root,world) || !vr_arm::hierarchy(bind,parents,root,reference)) return std::nullopt;
    const auto current=vr_arm::sub(vr_arm::position(world[c->top]),vr_arm::position(world[c->base]));
    const auto target=vr_arm::sub(vr_arm::position(reference[c->top]),vr_arm::position(reference[c->base]));
    Result r;r.locals.assign(locals.begin(),locals.end());
    r.before_degrees=r.after_degrees=angle_degrees(current,target);
    // 1. Lean: swing the spine line back onto its bind direction.
    if(r.before_degrees>settings.deadband_degrees && settings.strength>0 && settings.max_degrees>0) {
        auto axis=vr_arm::cross(current,target);const float axis_length=vr_arm::length(axis);
        if(axis_length>1e-6f) { // Parallel or opposite: nothing sensible to do.
            const float apply=std::min(r.before_degrees*settings.strength,settings.max_degrees);
            if(!turn_base(r.locals,world,parents,c->base,vr_arm::scale(axis,1/axis_length),apply)) return std::nullopt;
            r.corrected=true;
        }
    }
    std::vector<Matrix> after;
    if(!vr_arm::hierarchy(r.locals,parents,root,after)) return std::nullopt;
    // 2. Twist: turn about the (now corrected) spine line so the shoulders square up. This
    // keeps the spine line, so the lean result is unchanged.
    if(const auto s=shoulders(names,parents,*c)) {
        const auto needed=twist_to_bind(after,reference,*c,*s);
        if(needed) {
            r.twist_before_degrees=r.twist_after_degrees=*needed;
            if(std::abs(*needed)>settings.deadband_degrees && settings.twist_strength>0 && settings.max_degrees>0) {
                const float apply=std::copysign(std::min(std::abs(*needed)*settings.twist_strength,settings.max_degrees),*needed);
                auto axis=vr_arm::sub(vr_arm::position(after[c->top]),vr_arm::position(after[c->base]));
                axis=vr_arm::scale(axis,1/vr_arm::length(axis));
                if(!turn_base(r.locals,after,parents,c->base,axis,apply)) return std::nullopt;
                if(!vr_arm::hierarchy(r.locals,parents,root,after)) return std::nullopt;
                r.twist_after_degrees=twist_to_bind(after,reference,*c,*s).value_or(0.0f);
                r.corrected=true;
            }
        }
    }
    r.after_degrees=angle_degrees(vr_arm::sub(vr_arm::position(after[c->top]),vr_arm::position(after[c->base])),target);
    return r;
}
// While the game shoots, its own aim (GunAimIK) turns the upper body toward the target, and it
// fires again only once the gun is within CtrlAim's angleAimedCheck (40 degrees). Squaring the
// shoulders every frame undid that turn, so a pistol fired once and then not again until about
// 2.4 s after the last pull (Joe, 22 Oct; test room at 1 Hz: 1 of 6 taps fired with the twist,
// 6 of 6 without). The twist stands down while a firearm trigger is held and for HOLD after it,
// as long as the native shoot state lasts, then comes back over RETURN. Lean is not affected.
struct FireHold {
    using Clock=std::chrono::steady_clock;
    static constexpr auto DOWN=std::chrono::milliseconds(100),HOLD=std::chrono::milliseconds(2600),RETURN=std::chrono::milliseconds(500);
    // Share of the configured twist to apply now, in [0,1].
    float update(bool trigger,Clock::time_point now) {
        if(trigger) m_last=now;
        const float dt=m_updated?std::clamp(std::chrono::duration<float>(now-*m_updated).count(),0.0f,1.0f):0.0f;m_updated=now;
        const bool holding=m_last && now>=*m_last && now-*m_last<HOLD;
        if(holding) m_scale=std::max(0.0f,m_scale-dt/std::chrono::duration<float>(DOWN).count());
        else m_scale=std::min(1.0f,m_scale+dt/std::chrono::duration<float>(RETURN).count());
        return m_scale;
    }
private:
    std::optional<Clock::time_point> m_last,m_updated;float m_scale{1};
};
} // namespace vr_upright
