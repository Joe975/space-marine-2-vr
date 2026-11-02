#pragma once
#include "VrWeaponAim.h"

// Experimental firearm constraint. Model lengths are native world units; Contact
// is in logical controller metres (0 support, 1 dominant), before body-zone scaling.
// Nothing here reads the game, invents a weapon id, changes a binding or emits input.
namespace vr_two_hand {
using Matrix=vr_arm::Matrix;
using Clock=VrWeaponAim::Clock;
struct Model {
    uintptr_t actor{};uint64_t epoch{},generation{};Clock::time_point captured{};
    size_t dominant{1};float units{1},tracking_units{}; // Marine geometry vs physical controller metres.
    Matrix gun_to_primary{},support_to_gun{},drawn_to_primary{};
};
struct Request {uint64_t epoch{},generation{};bool held{};uint64_t engagement{};};
struct Contact {
    uintptr_t actor{};uint64_t epoch{},generation{};Clock::time_point captured{};
    Matrix controller_to_primary{};
    std::optional<std::array<vr_arm::Vector,3>> region; // Grip/body proxy, not mesh bounds; primary-relative physical metres.
};
inline float region_distance(const std::array<vr_arm::Vector,3>& points,const vr_arm::Vector& point) {
    float result=INFINITY;
    for(size_t i=0;i<3;++i) {
        const auto a=points[i],b=points[(i+1)%3],ab=vr_arm::sub(b,a);
        const auto squared=vr_arm::dot(ab,ab);
        const auto fraction=squared>1e-8f?std::clamp(vr_arm::dot(vr_arm::sub(point,a),ab)/squared,0.f,1.f):0.f;
        result=std::min(result,vr_arm::length(vr_arm::sub(point,vr_arm::add(a,vr_arm::scale(ab,fraction)))));
    }
    return result;
}
// Native Marine metres, not physical controller reach. The 3 Nov Heavy trace
// seats the gun on GUN_R with a 1.05-1.08 m wrist span; the old .9 m cap rejected
// its support model before any grab/solve. Keep an upper bound for transition poses.
inline constexpr float MAX_NATIVE_SPAN=1.20f;
inline std::optional<Model> learn(std::span<const Matrix> native,std::span<const int32_t> parents,
    std::span<const std::string> names,const Matrix& root,const vr_weapon::Attachment& gun,
    float units,const vr_hand::Swap* swap=nullptr) {
    if(native.size()!=names.size() || parents.size()!=names.size() || !gun.grip || !vr_weapon::primary(*gun.grip) || !gun.seated || !std::isfinite(units) || units<.01f || units>100) return {};
    const auto index=[&](const char* name)->std::optional<size_t>{
        const auto it=std::find(names.begin(),names.end(),name);
        if(it==names.end() || std::count(names.begin(),names.end(),name)!=1)return {};
        return size_t(it-names.begin());
    };
    const auto l=index("HANDL"),r=index("HANDR"),ls=index("GUN_L"),rs=index("GUN_R");
    std::vector<Matrix> world;
    if(!l || !r || !ls || !rs || gun.index>=names.size() || !vr_arm::hierarchy(native,parents,root,world))return {};
    // A reload/draw with neither socket seated must not teach a support pose.
    if(vr_weapon::read_grip(world[gun.index],world[*ls],world[*rs],units)!=gun.grip)return {};
    const auto separation=vr_arm::length(vr_arm::sub(vr_arm::position(world[*l]),vr_arm::position(world[*r])))/units;
    if(separation<.12f || separation>MAX_NATIVE_SPAN)return {};
    const auto authored=world[vr_weapon::on_left_socket(*gun.grip)?*ls:*rs];
    auto from_support=vr_camera::multiply(authored,vr_camera::inverse(world[*l]));
    if(swap)from_support=swap->moved(from_support,0);
    Model out;out.dominant=swap?0:1;out.units=units;out.support_to_gun=vr_camera::inverse(from_support);
    // The caller supplies the calibrated committed gun-to-physical-primary fit.
    return out;
}
struct Solution {Matrix gun{};std::array<Matrix,2> wrists{};};
inline std::optional<Solution> solve(const Model& model,const std::array<std::optional<Matrix>,2>& wrists) {
    if(model.dominant>1 || !wrists[0] || !wrists[1] || !vr_camera::rigid(*wrists[0]) || !vr_camera::rigid(*wrists[1]) ||
        !vr_camera::rigid(model.gun_to_primary) || !vr_camera::rigid(model.support_to_gun) || !std::isfinite(model.units) || model.units<.01f || model.units>100)return {};
    const auto primary=model.dominant,other=1-primary;
    const auto gun=vr_camera::multiply(model.gun_to_primary,*wrists[primary]);
    const auto support=vr_camera::multiply(model.support_to_gun,gun);
    const auto pivot=vr_arm::position(*wrists[primary]);
    const auto from=vr_arm::sub(vr_arm::position(support),pivot),to=vr_arm::sub(vr_arm::position(*wrists[other]),pivot);
    const auto a=vr_arm::length(from),b=vr_arm::length(to);
    const float physical=model.tracking_units==0?model.units:model.tracking_units;
    if(!std::isfinite(physical) || physical<.01f || physical>100)return {};
    // Contact acquisition/release is measured in physical controller metres.
    // The Marine's authored scale is smaller in first person; using it here
    // rejected holds still inside the 12+8 cm release region (3 Nov playtest).
    if(a<.12f*physical || b<.12f*physical || std::abs(a-b)>.20f*physical ||
        vr_arm::dot(from,to)/(a*b)<.258819f)return {}; // >75 degrees: parted/crossed hands, not a grip.
    auto rotation=vr_arm::turn(from,to);
    vr_arm::position(rotation,vr_arm::sub(pivot,vr_arm::rotate(pivot,rotation)));
    Solution out;out.gun=vr_camera::multiply(gun,rotation);
    out.wrists[primary]=vr_camera::multiply(vr_camera::inverse(model.gun_to_primary),out.gun);
    out.wrists[other]=vr_camera::multiply(model.support_to_gun,out.gun);
    if(!vr_camera::rigid(out.gun) || !vr_camera::rigid(out.wrists[0]) || !vr_camera::rigid(out.wrists[1]))return {};
    return out;
}
// Convert the actual rendered gun's native support wrist into a controller-space
// contact. wrist_world performs the established native/XR axis and scale conversion.
inline std::optional<Contact> contact(const Model& model,const Matrix& primary_world,const Matrix& primary_controller,
    const Matrix& support_grip,const Matrix& camera_anchor,float tracking_units,bool left_handed) {
    if(!vr_camera::rigid(primary_world) || !vr_camera::rigid(primary_controller) || !vr_camera::rigid(support_grip) ||
        !vr_camera::rigid(model.drawn_to_primary) || !vr_camera::rigid(model.support_to_gun))return {};
    const auto anchor=vr_arm::wrist_world(camera_anchor,vr_camera::IDENTITY,vr_camera::IDENTITY,tracking_units);
    if(!anchor)return {};
    auto grip=support_grip;for(int i=12;i<15;++i)grip[i]*=tracking_units;
    const auto desired=vr_camera::multiply(model.support_to_gun,vr_camera::multiply(model.drawn_to_primary,primary_world));
    auto controller=vr_camera::multiply(vr_camera::multiply(vr_camera::inverse(grip),desired),vr_camera::inverse(*anchor));
    for(int i=12;i<15;++i)controller[i]/=tracking_units;
    auto relative=vr_camera::multiply(controller,vr_camera::inverse(primary_controller));
    if(left_handed)relative=vr_hand::mirror(relative);
    if(!vr_camera::rigid(relative))return {};
    const auto gun_world=vr_camera::multiply(model.drawn_to_primary,primary_world);
    auto centre=vr_camera::multiply(vr_camera::multiply(vr_camera::inverse(grip),gun_world),vr_camera::inverse(*anchor));
    for(int i=12;i<15;++i)centre[i]/=tracking_units;
    centre=vr_camera::multiply(centre,vr_camera::inverse(primary_controller));
    if(left_handed)centre=vr_hand::mirror(centre);
    if(!vr_camera::rigid(centre))return {};
    Contact result{model.actor,model.epoch,model.generation,model.captured,relative};
    result.region=std::array<vr_arm::Vector,3>{vr_arm::Vector{},vr_arm::position(relative),vr_arm::position(centre)};
    return result;
}
// Reserve at the start of the analogue squeeze, not only at the full grip
// threshold. An interrupted hold stays consumed until release: never a late LB
// or belt grab. A fresh belt grab still gets first refusal in VrSession.
class Grip {
public:
    struct State {bool held{},reserved{},in_contact{};uint64_t engagements{},generation{};};
    void reset(){const auto count=m_state.engagements;m_state={};m_state.engagements=count;m_released=false;}
    State state()const{return m_state;}
    void reserve(float grip){if(std::isfinite(grip) && grip>.35f)m_state.reserved=true;}
    static bool within_contact(const Contact& contact,const Matrix& primary,const Matrix& support,float margin=0) {
        if(!vr_camera::rigid(contact.controller_to_primary) || !vr_camera::rigid(primary) || !vr_camera::rigid(support))return false;
        if(contact.region) {
            const auto relative=vr_camera::multiply(support,vr_camera::inverse(primary));
            const auto point=vr_arm::position(relative);
            if(vr_arm::length(point)<.12f)return false;
            for(const auto& p:*contact.region)for(float x:p)if(!std::isfinite(x))return false;
            return region_distance(*contact.region,point)<=.12f+margin;
        }
        const auto expected=vr_camera::multiply(contact.controller_to_primary,primary);
        return vr_arm::length(vr_arm::sub(vr_arm::position(expected),vr_arm::position(support)))<=.12f+margin;
    }
    bool update(const std::optional<Contact>& contact,const std::optional<Matrix>& primary,const std::optional<Matrix>& support,
        float grip,bool allowed,uint64_t epoch,Clock::time_point now) {
        if(!std::isfinite(grip)){m_state.held=false;m_released=false;return false;}
        if(grip<=.35f){m_state.held=m_state.reserved=false;m_released=true;}
        const bool valid=allowed && contact && contact->epoch==epoch && VrWeaponAim::fresh(contact->captured,now) && primary && support;
        const bool changed=contact && m_state.generation && contact->generation!=m_state.generation;
        if(!valid || changed){m_state.held=m_state.in_contact=false;m_released=valid && grip<=.35f;if(valid)m_state.generation=contact->generation;return false;}
        m_state.generation=contact->generation;
        m_state.in_contact=within_contact(*contact,*primary,*support,m_state.held?.08f:0);
        if(m_state.in_contact && grip>.35f)m_state.reserved=true;
        if(!m_state.in_contact)m_state.held=false;
        else if(m_released && grip>=.65f){if(!m_state.held)++m_state.engagements;m_state.held=true;}
        return m_state.held;
    }
private:State m_state;bool m_released{};
};
}
