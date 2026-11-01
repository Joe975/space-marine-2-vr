#pragma once
#include "VrBodyFrame.h"
#include <chrono>
#include <deque>
#include <optional>

// Left-arm push: knockback. Joe, 21 Oct evening (20:39): "I don't have knockback ...
// knockback should just be a push with the left arm".
//
// The game has no separate shove/knockback command for this loadout: the gameplay
// controller's melee commands are MeleeLight, the VR held strike (VrSwordPower, D-pad up,
// native LIGHT_HOLD), MeleeParry/Block, CounterAttack (finisher), GunStrike (contextual,
// after a perfect parry/dodge) and Finisher (ssl/actions/action_controller_project.sso,
// ssl/input/keymap_layer_game.sso). The chainsword's knockback attack is the held strike,
// so a push sends that: the caller pulses D-pad up, like a power swing.
//
// A push is a left hand thrust straight out, open hand or closed fist (Joe, 22 Oct: a left
// punch is a push too; this replaces the 20 Oct "a left punch does nothing"). The wrist moves
// at least TRAVEL forward (gaze yaw, measured in room space so a head turn is not a thrust)
// within WINDOW, at least SPEED forward between samples, and ends with the arm out
// (END_FORWARD) at chest height in front of the body. Not a push: a grenade held (the
// grenade owns the grip), the left trigger pulled now or within TRIGGER_QUIET (the pistol),
// the guard raised, or the right wrist moving faster than RIGHT_STILL (both arms flailing; the
// right controller twists along with a left punch, so its virtual blade tip is not used).
// After a push the arm must come back REARM_BACK before another, and COOLDOWN applies.
//
// Joe's evening trace has one clean push, right after the remark (1447.0 s: 0.54 m forward in
// 0.17 s, peak 3.7 m/s, ending 0.63 m out, 0.35 m below the eyes, grip open, right hand still).
class VrLeftPush {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr float SPEED=2.0f,TRAVEL=0.30f,END_FORWARD=0.40f,MIN_HEIGHT=-0.75f,MAX_HEIGHT=-0.05f,MIN_LATERAL=-0.6f,MAX_LATERAL=0.3f;
    static constexpr float TRIGGER=0.05f,RIGHT_STILL=1.5f,REARM_BACK=0.15f;
    static constexpr auto WINDOW=std::chrono::milliseconds(300),TRIGGER_QUIET=std::chrono::milliseconds(300),
        PULSE=std::chrono::milliseconds(140),COOLDOWN=std::chrono::milliseconds(800);
    struct Snapshot { uint64_t pushes{}; bool pressed{},ready{}; float forward{},height{},lateral{},travel{},peak{}; };
    struct Input {
        std::optional<vr_camera::Matrix> head,hand; // Raw standing-space matrices.
        float trigger{};                            // Left trigger.
        bool grenade{};                             // A grenade owns the left grip (VrGrenadeThrow).
        float right_speed{};                        // Right wrist speed (VrSwingMelee::Snapshot::wrist_speed).
        bool guard{};                               // VrGuard pose raised.
        bool allowed{};                             // Sword loadout, focus, armed, gestures on.
    };
    void reset() { m_points.clear();m_state.pressed=false;m_state.ready=false;m_pulse_until={}; }
    Snapshot snapshot() const { return m_state; }
    bool update(const Input& in,Clock::time_point now) {
        if(!in.allowed || !in.head || !in.hand || !vr_camera::rigid(*in.head) || !vr_camera::rigid(*in.hand) || !std::isfinite(in.trigger)) { reset();return false; }
        const auto forward=vr_body::yaw_forward(*in.head);
        if(!forward) { reset();return false; }
        if(in.trigger>TRIGGER) m_trigger_at=now;
        const std::array<float,3> d{(*in.hand)[12]-(*in.head)[12],(*in.hand)[13]-(*in.head)[13],(*in.hand)[14]-(*in.head)[14]};
        if(!m_points.empty() && (now<=m_points.back().time || now-m_points.back().time>std::chrono::milliseconds(100))) m_points.clear();
        m_points.push_back({d,now});
        while(now-m_points.front().time>WINDOW) m_points.pop_front();
        const auto here=vr_body::direction(*forward,d[0],d[1],d[2]);
        m_state.forward=here.forward;m_state.height=here.height;m_state.lateral=here.lateral;
        const auto along=[&](const std::array<float,3>& a,const std::array<float,3>& b){return (a[0]-b[0])*(*forward)[0]+(a[2]-b[2])*(*forward)[1];};
        float travel{},peak{};
        for(size_t i=0;i<m_points.size();++i) {
            travel=(std::max)(travel,along(d,m_points[i].wrist));
            if(i+1<m_points.size()) {
                const float dt=std::chrono::duration<float>(m_points[i+1].time-m_points[i].time).count();
                if(dt>0) peak=(std::max)(peak,along(m_points[i+1].wrist,m_points[i].wrist)/dt);
            }
        }
        m_state.travel=travel;m_state.peak=peak;
        if(here.forward<END_FORWARD-REARM_BACK) m_state.ready=true;
        m_state.pressed=now<m_pulse_until;
        const bool quiet_trigger=m_trigger_at==Clock::time_point{} || now-m_trigger_at>=TRIGGER_QUIET;
        const bool pose=here.forward>=END_FORWARD && here.height>=MIN_HEIGHT && here.height<=MAX_HEIGHT && here.lateral>=MIN_LATERAL && here.lateral<=MAX_LATERAL;
        if(m_state.ready && now>=m_cooldown_until && pose && travel>=TRAVEL && peak>=SPEED && !in.grenade && quiet_trigger &&
           in.right_speed<=RIGHT_STILL && !in.guard) {
            m_state.ready=false;++m_state.pushes;m_state.pressed=true;
            m_pulse_until=now+PULSE;m_cooldown_until=now+COOLDOWN;
        }
        return m_state.pressed;
    }
private:
    struct Point { std::array<float,3> wrist; Clock::time_point time; };
    std::deque<Point> m_points;
    Snapshot m_state;
    Clock::time_point m_pulse_until{},m_cooldown_until{},m_trigger_at{};
};
