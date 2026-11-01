#pragma once
#include "VrWeaponAim.h"
#include "GameBuild.h"
#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <cstring>
#include <vector>

// Retail 0x6ac3e716: only the script RigidBody.SetVelocity launch caller, a player
// throwable (HAND_THROWN), and its exact PropEquipment.user may be redirected.
// No nearest-body inference and no persistent physics/body writes.
class NativeGrenadeLaunch {
public:
    // The player's thrown equipment: each is a simple_grenade_base_client with a "left hand
    // item" prop_equipment_client in the game's scripts (ssl/weapons/throwable). Frag only
    // until 26 Oct: krak, shock and melta charges kept the game's aim along its gameplay
    // camera, which follows the gaze (Joe: "maybe it's just going off the direction my
    // head's facing"; GrenadeLaunch refused=7 redirected=0 over his krak/shock/melta throws).
    static constexpr std::array<std::string_view,4> HAND_THROWN{"grenade_frag_client","grenade_krak_client","grenade_shock_client","melta_charge_client"};
    static bool hand_thrown(std::string_view name){return std::ranges::find(HAND_THROWN,name)!=HAND_THROWN.end();}
    using Vector=std::array<float,3>;
    using Clock=std::chrono::steady_clock;
    using Reader=std::function<bool(uintptr_t,void*,size_t)>;
    // One entry per redirect, the most recent HISTORY kept, so a periodic log misses none.
    struct Redirect {uint64_t serial{};uintptr_t projectile{};Vector before{},after{};};
    static constexpr size_t HISTORY=16;
    // refused splits into: pose (no fresh unconsumed throw, so also other script launches
    // after a throw), type (a body that is not HAND_THROWN; last_refused_type names it) and
    // owner (a throwable that is not the player's).
    struct Snapshot {uint64_t redirected{},refused{};uintptr_t actor{},projectile{};Vector before{},after{};std::vector<Redirect> history;
        uint64_t refused_pose{},refused_type{},refused_owner{};std::string last_refused_type;};
    static NativeGrenadeLaunch& get(){static auto* value=new NativeGrenadeLaunch;return *value;}
    void bind(uintptr_t base,Reader reader,std::function<uintptr_t()> active){std::scoped_lock lock{m_mutex};m_base=base;m_read=std::move(reader);m_active=std::move(active);m_pose.reset();m_consumed={};m_state={};}
    void publish(std::optional<VrWeaponAim::Pose> pose,Clock::time_point now=Clock::now()){
        std::scoped_lock lock{m_mutex};m_pose=pose;m_received=now;
    }
    std::optional<Vector> rewrite(uintptr_t body,uintptr_t value,uintptr_t caller,Clock::time_point now=Clock::now()){
        std::scoped_lock lock{m_mutex};
        if(!m_base || caller!=m_base+game_build::rva(0x12aed3e) || !m_pose)return {};
        const auto& pose=*m_pose;const auto key=std::tuple{pose.actor,pose.epoch,pose.sequence};
        const auto [used_actor,used_epoch,used_sequence]=m_consumed;
        const bool replay=pose.epoch<used_epoch || (pose.epoch==used_epoch && pose.actor==used_actor && pose.sequence<=used_sequence);
        if(!pose.actor || !pose.epoch || !pose.sequence || replay || !m_active || m_active()!=pose.actor ||
            now<pose.captured || now-pose.captured>std::chrono::milliseconds(750) || now<m_received || now-m_received>std::chrono::milliseconds(100) || !vr_camera::rigid(pose.world)){++m_state.refused;++m_state.refused_pose;return {};}
        Vector native{};if(!read(value,native) || !std::ranges::all_of(native,[](float v){return std::isfinite(v) && std::abs(v)<10000;}))return {};
        const auto packed=q(body+0xa0);if((packed&15)!=2)return {};
        const auto props=q(q((packed>>4)+0x18));
        if(const auto type=name(props);!hand_thrown(type)){++m_state.refused;++m_state.refused_type;m_state.last_refused_type=type.substr(0,48);return {};}
        if(!owned(props,pose.actor)){++m_state.refused;++m_state.refused_owner;return {};}
        const auto speed=std::hypot(native[0],native[1],native[2]);
        if(!std::isfinite(speed) || speed<.01f)return {};
        Vector direction{pose.world[8],pose.world[9],pose.world[10]};
        const auto length=std::hypot(direction[0],direction[1],direction[2]);if(!std::isfinite(length) || length<.01f)return {};
        // The hand sets the speed as a fraction of the native throw (VrGrenadeAim clamps it);
        // anything outside that sane band falls back to the native speed.
        const float scale=std::isfinite(pose.speed_scale) && pose.speed_scale>=.1f && pose.speed_scale<=3?pose.speed_scale:1.0f;
        for(auto& v:direction)v*=speed*scale/length;
        m_consumed=key;++m_state.redirected;m_state.actor=pose.actor;m_state.projectile=props;m_state.before=native;m_state.after=direction;
        if(m_state.history.size()==HISTORY)m_state.history.erase(m_state.history.begin());
        m_state.history.push_back({m_state.redirected,props,native,direction});
        return direction;
    }
    Snapshot snapshot()const{std::scoped_lock lock{m_mutex};return m_state;}
private:
    template<class T>bool read(uintptr_t p,T& value)const{return p && m_read && m_read(p,&value,sizeof value);}
    uintptr_t q(uintptr_t p)const{uintptr_t v{};read(p,v);return v;}
    std::string text(uintptr_t p)const{
        std::array<unsigned char,16> b{};if(!read(p,b))return {};
        size_t n{};uintptr_t data=p;
        if(b[15]&1){uint32_t size{};std::memcpy(&data,b.data(),8);std::memcpy(&size,b.data()+8,4);n=size;}
        else if((b[15]>>1)<=15)n=15-(b[15]>>1);
        if(!n || n>64)return {};std::string s(n,'\0');return m_read(data,s.data(),n)?s:std::string{};
    }
    std::string name(uintptr_t p)const{return text(q(q(p+8)+0x10)+0x30);}
    bool owned(uintptr_t props,uintptr_t actor)const{
        const auto player_props=q(actor+0x20),player_vm=q(player_props+0x10);
        // The native/VM association must round-trip, excluding recycled references.
        const auto player_weak=q(player_props+0x30);
        if(!player_vm || !player_weak || q(player_vm+0x18)!=player_weak || q(player_weak)!=player_props)return false;
        uint32_t count{};if(!read(props+0x20,count) || !count || count>128)return false;
        const auto slots=q(props+0x18);uintptr_t equipment{};
        for(uint32_t i=0;i<count;++i){const auto p=q(slots+i*8);
            if(name(p)=="prop_equipment_client"){
                if(equipment || q(p+0x20)!=props)return false;equipment=p;
            }
        }
        const auto vm=q(equipment+0x18);
        const auto weak=q(equipment+0x10);
        if(!equipment || !vm || !weak || q(vm+0x18)!=weak || q(weak)!=equipment)return false;
        // Three header words and the native association precede 11 PropBase
        // fields. PropEquipment.user is its tenth field, at VM word 24. Verified against the
        // installed SSLB declarations, not a search for a nearby player pointer.
        const auto user=q(vm+0xc0);
        return (user&7)==6 && (user>>3)==player_vm;
    }
    mutable std::mutex m_mutex;uintptr_t m_base{};Reader m_read;std::function<uintptr_t()> m_active;
    std::optional<VrWeaponAim::Pose> m_pose;Clock::time_point m_received{};
    std::tuple<uintptr_t,uint64_t,uint64_t> m_consumed{};Snapshot m_state;
};
