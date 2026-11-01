#include "VrGuard.h"
#include "VrGrenadeThrow.h"
#include "VrLeftPush.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Vec { float x,y,z; };
Vec cross(Vec a,Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec unit(Vec v) { const float l=std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);return {v.x/l,v.y/l,v.z/l}; }
// OpenVR standing space: +Y up, gaze along -Z. Yaw turns right.
vr_camera::Matrix head(float yaw,Vec at={0,1.7f,0}) {
    const Vec forward{std::sin(yaw),0,-std::cos(yaw)};
    const Vec z{-forward.x,-forward.y,-forward.z},x=unit(cross({0,1,0},z)),y=cross(z,x);
    return {x.x,x.y,x.z,0, y.x,y.y,y.z,0, z.x,z.y,z.z,0, at.x,at.y,at.z,1};
}
// Head-yaw frame (lateral right, height, forward) to standing space.
Vec world(float yaw,float lateral,float height,float forward,Vec at) {
    return {at.x+lateral*std::cos(yaw)+forward*std::sin(yaw),at.y+height,at.z+lateral*std::sin(yaw)-forward*std::cos(yaw)};
}
enum class Point { forward,right,up,down };
// A controller at (lateral,height,forward) whose -Z pointing axis is along `point` in the head-yaw frame.
vr_camera::Matrix hand(float yaw,float lateral,float height,float forward,Point point=Point::right,Vec at={0,1.7f,0}) {
    const Vec p=point==Point::forward?Vec{std::sin(yaw),0,-std::cos(yaw)}:point==Point::right?Vec{std::cos(yaw),0,std::sin(yaw)}:
        point==Point::up?Vec{0,1,0}:Vec{0,-1,0};
    const Vec z{-p.x,-p.y,-p.z};
    const Vec helper=std::abs(z.y)>0.9f?Vec{std::sin(yaw),0,-std::cos(yaw)}:Vec{0,1,0};
    const Vec x=unit(cross(helper,z)),y=cross(z,x);
    const Vec w=world(yaw,lateral,height,forward,at);
    return {x.x,x.y,x.z,0, y.x,y.y,y.z,0, z.x,z.y,z.z,0, w.x,w.y,w.z,1};
}
const auto T0=Clock::time_point{100s};

void guard_pose() {
    VrGuard g;auto t=T0;
    const auto H=head(0);
    require(!g.update(H,hand(0,-0.2f,-0.1f,0.25f),0,true,t),"a guard already raised at enable must be lowered first");
    t+=11ms;require(!g.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t),"left hand at the side is not a guard");
    t+=11ms;require(g.update(H,hand(0,-0.2f,-0.1f,0.25f),0,true,t),"forearm raised across the face holds parry/block");
    require(g.snapshot().guards==1 && g.owns_hand(),"one raise is one guard and owns the left hand");
    t+=500ms;require(g.update(H,hand(0,-0.2f,-0.1f,0.25f),0,true,t),"holding the guard keeps the native button held (block)");
    t+=11ms;require(!g.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t),"lowering the hand releases");
    require(g.snapshot().last_duration_ms>500 && g.snapshot().last_duration_ms<520,"guard duration is recorded for the trace");
    t+=11ms;require(g.update(H,hand(0,-0.1f,0.05f,0.25f,Point::up),0,true,t),"a vertical (pointing up) guard counts");
    t+=100ms;g.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t); // Past min_hold.
    const auto rejected=g.snapshot().angle_rejections;
    t+=11ms;require(!g.update(H,hand(0,-0.15f,-0.3f,0.25f,Point::forward),0,true,t),"a left hand pointing forward (supporting a gun) is not a guard");
    require(g.snapshot().angle_rejections==rejected+1,"a forward-pointing hand in the zone is counted as an angle rejection");
    t+=11ms;require(!g.update(H,hand(0,-0.15f,-0.3f,0.25f,Point::forward),0,true,t) && g.snapshot().angle_rejections==rejected+1,"one zone visit counts once");
    t+=11ms;g.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t);
    t+=11ms;require(!g.update(H,hand(0,-0.2f,-0.1f,0.25f),0.5f,true,t),"aiming down sights (left trigger) never parries");
    t+=11ms;require(!g.update(H,hand(0,-0.2f,-0.1f,1.0f),0,true,t),"an outstretched arm beyond reach is not a guard");
    t+=11ms;require(!g.update(H,hand(0,0.5f,-0.1f,0.25f),0,true,t),"a hand far to the right is outside the guard");
}
void guard_timing() {
    VrGuard g;auto t=T0;const auto H=head(0);
    g.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t);
    t+=11ms;require(g.update(H,hand(0,-0.2f,-0.1f,0.25f),0,true,t),"raise");
    t+=11ms;require(g.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t),"a flick shorter than min_hold still reaches the game");
    t+=80ms;require(!g.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t),"min_hold expires");
    // Hysteresis: once raised, drifting just past an edge holds; well past releases.
    t+=11ms;g.update(H,hand(0,-0.2f,-0.1f,0.25f),0,true,t);
    t+=11ms;require(g.update(H,hand(0,-0.2f,0.33f,0.25f),0,true,t),"drift within exit margin keeps the guard");
    t+=200ms;require(!g.update(H,hand(0,-0.2f,0.4f,0.25f),0,true,t),"leaving by more than the margin releases");
    t+=11ms;g.update(H,hand(0,-0.2f,-0.1f,0.25f),0,true,t);
    t+=11ms;require(!g.update(H,hand(0,-0.2f,-0.1f,0.25f),0,false,t),"disallowed input releases immediately");
    t+=11ms;require(!g.update(H,hand(0,-0.2f,-0.1f,0.25f),0,true,t),"after a gate drop the held guard must be lowered first");
    t+=11ms;require(!g.update(std::nullopt,hand(0,-0.2f,-0.1f,0.25f),0,true,t),"lost head tracking releases");
}
// Playtest 20 Oct: the left pistol held out in the guard zone parried between every
// shot, and a guard held through a sword swing re-pressed LB (a new native parry that also
// aborts the attack) when the attack pulse ended.
void guard_aim_latch_and_yield() {
    VrGuard g;auto t=T0;const auto H=head(0);
    const auto down=hand(0,-0.3f,-0.8f,0,Point::down),held_out=hand(0,-0.2f,-0.35f,0.25f),raised=hand(0,-0.2f,-0.1f,0.25f);
    g.update(H,down,0,true,t);
    t+=11ms;require(!g.update(H,held_out,1,true,t),"firing the pistol never parries");
    t+=11ms;require(!g.update(H,held_out,0,true,t),"releasing the trigger between shots with the pistol still held out must not parry");
    t+=300ms;require(!g.update(H,held_out,0,true,t) && g.snapshot().aim_latched,"the aim latch holds while the hand stays in the zone");
    const auto rejected=g.snapshot().angle_rejections;
    t+=11ms;g.update(H,held_out,1,true,t);t+=11ms;g.update(H,held_out,0,true,t);
    require(g.snapshot().angle_rejections==rejected && g.snapshot().guards==0,"aim-latched visits are neither guards nor angle rejections");
    t+=11ms;require(!g.update(H,down,0,true,t) && !g.snapshot().aim_latched,"leaving the zone clears the aim latch");
    t+=11ms;require(g.update(H,raised,0,true,t) && g.snapshot().guards==1,"a fresh raise after lowering guards again");
    t+=11ms;g.update(H,down,0,true,t);t+=100ms;g.update(H,down,0,true,t);
    t+=11ms;require(!g.update(H,raised,0.5f,true,t),"aiming as the hand enters the zone never parries");
    t+=11ms;require(!g.update(H,raised,0,true,t),"releasing the trigger inside the zone still waits for the hand to leave");
    t+=11ms;g.update(H,down,0,true,t);

    t+=11ms;require(g.update(H,raised,0,true,t),"raise");
    const auto guards=g.snapshot().guards;
    g.yield(t);
    require(g.snapshot().yields==1 && !g.snapshot().held && !g.owns_hand(),"a sword attack releases the held guard");
    t+=200ms;require(!g.update(H,raised,0,true,t),"the still-raised guard does not press LB again after the attack");
    t+=500ms;require(!g.update(H,raised,0,true,t) && g.snapshot().guards==guards,"no new guard while it stays raised");
    t+=11ms;g.update(H,down,0,true,t);
    t+=11ms;require(g.update(H,raised,0,true,t) && g.snapshot().guards==guards+1,"lowering and raising guards again");
    t+=11ms;g.update(H,down,0,true,t);t+=100ms;g.update(H,down,0,true,t);
    g.yield(t);
    require(g.snapshot().yields==1,"an attack with the guard lowered is not a yield");
    t+=11ms;require(g.update(H,raised,0,true,t),"and leaves the next raise immediate (timing-critical parry)");
}
// Joe, 20 Oct: a guard starts with the hand near the face; a held block may drift out.
void guard_entry_near_face() {
    VrGuard g;auto t=T0;const auto H=head(0);const auto down=hand(0,-0.3f,-0.8f,0,Point::down);
    g.update(H,down,0,true,t);
    t+=11ms;require(!g.update(H,hand(0,-0.2f,-0.3f,0.55f),0,true,t),"an arm already out (a punch or a held pistol) never starts a guard, even across");
    t+=11ms;require(!g.update(H,hand(0,-0.2f,-0.2f,0.4f,Point::up),0,true,t) && !g.snapshot().guards,"nor does pulling it back to 0.4 m");
    t+=11ms;g.update(H,down,0,true,t);
    t+=11ms;require(g.update(H,hand(0,-0.2f,-0.1f,0.2f),0,true,t),"raised close to the face it guards");
    for(float forward:{0.3f,0.4f,0.5f,0.6f}) { t+=50ms;require(g.update(H,hand(0,-0.2f,-0.1f,forward),0,true,t),"a held block may drift out"); }
    require(g.snapshot().guards==1,"one block stays one guard while it drifts (no extra parries)");
    VrGuard tight;auto s=tight.settings();s.values.values[VrGuard::MAX_FORWARD]=0.4f;require(tight.set_settings(s),"max_forward accepts 0.4");
    tight.update(H,down,0,true,t);t+=11ms;require(tight.update(H,hand(0,-0.2f,-0.1f,0.2f),0,true,t),"tight: raise");
    t+=100ms;require(!tight.update(H,hand(0,-0.2f,-0.1f,0.5f),0,true,t),"max_forward_metres still ends a block that drifts beyond it");
    auto bad=s;bad.values.values[VrGuard::MAX_ENTRY_FORWARD]=0.5f;require(!bad.valid(),"the entry limit cannot exceed the zone");
    VrGuard steep;t+=11ms;steep.update(H,down,0,true,t);
    auto tilted=hand(0,-0.2f,-0.1f,0.2f,Point::up);
    // 20 Oct: the left pistol held while firing read 47.7 degrees (median), 56.1 (p90).
    const auto across=[&](float degrees){auto m=hand(0,-0.2f,-0.1f,0.2f,Point::up);const float a=(90-degrees)*0.0174533f;const Vec p{0,std::cos(a),-std::sin(a)};
        const Vec z{-p.x,-p.y,-p.z},x=unit(cross({0,0,-1},z)),y=cross(z,x);return vr_camera::Matrix{x.x,x.y,x.z,0,y.x,y.y,y.z,0,z.x,z.y,z.z,0,m[12],m[13],m[14],1};};
    tilted=across(55);
    t+=11ms;require(!steep.update(H,tilted,0,true,t),"a controller only 55 degrees across (the pistol-holding angle) is no guard by default");
    // Joe, 21 Oct evening: raises held at 58-61.6 degrees by the face were meant as guards.
    VrGuard near;t+=11ms;near.update(H,down,0,true,t);t+=11ms;require(near.update(H,across(60),0,true,t),"a raise 60 degrees across by the face guards (21 Oct evening near-misses)");
    // Joe, 23 Oct: the zone reaches further from the face and higher.
    VrGuard far;t+=11ms;far.update(H,down,0,true,t);t+=11ms;require(far.update(H,hand(0,-0.2f,-0.1f,0.36f),0,true,t),"a guard can start 0.36 m from the face");
    VrGuard high;t+=11ms;high.update(H,down,0,true,t);t+=11ms;require(high.update(H,hand(0,-0.1f,0.27f,0.25f,Point::up),0,true,t),"a guard can start 0.27 m above the eyes");
}
// Playtest 26 Oct (docs/PARRY_TIMING_2026-10-26.md): a hand resting in the grenade belt zone pauses
// the guard without latching it; a grenade transaction (allowed=false) still latches.
void guard_belt_rest() {
    const auto H=head(0);const auto across=hand(0,-0.2f,-0.1f,0.25f),low=hand(0,-0.25f,-0.7f,0.1f,Point::down);
    VrGuard g;auto t=T0;g.update(H,low,0,true,t);
    t+=11ms;require(!g.poll(H,low,0,true,true,false,t),"a hand resting at the belt is no guard");
    t+=11ms;require(g.poll(H,across,0,true,false,false,t),"raised straight out of the belt it guards on the first poll");
    t+=11ms;require(g.poll(H,hand(0,-0.2f,-0.45f,0.25f),0,true,true,false,t),"a raised guard dipping into the belt's top keeps the hand");
    t+=100ms;require(!g.poll(H,low,0,true,true,false,t),"lowered into the belt it lets go (after the minimum hold)");
    require(g.snapshot().guards==1,"one raise, one guard (no flicker at the boundary)");
    VrGuard old;t+=11ms;old.update(H,low,0,true,t);
    t+=11ms;old.poll(H,low,0,true,true,true,t);
    t+=11ms;require(!old.poll(H,across,0,true,false,true,t),"belt_latch: the raise out of the belt is latched (the rule before 26 Oct)");
    VrGuard busy;t+=11ms;busy.update(H,low,0,true,t);
    t+=11ms;busy.poll(H,low,0,false,false,false,t); // A grenade is held: not allowed.
    t+=11ms;require(!busy.poll(H,across,0,true,false,false,t),"after a grenade transaction the raised hand must still be lowered first");
    t+=11ms;busy.poll(H,low,0,true,true,false,t);
    t+=11ms;require(busy.poll(H,across,0,true,false,false,t),"once lowered it guards again");
}
void guard_yaw_invariance() {
    for(float yaw:{0.7f,-2.1f,3.0f}) {
        VrGuard g;auto t=T0;const Vec at{1.3f,1.6f,-0.4f};const auto H=head(yaw,at);
        g.update(H,hand(yaw,-0.3f,-0.8f,0,Point::down,at),0,true,t);
        t+=11ms;require(g.update(H,hand(yaw,-0.2f,-0.1f,0.25f,Point::right,at),0,true,t),"guard follows head yaw and room position");
        t+=200ms;g.update(H,hand(yaw,-0.3f,-0.8f,0,Point::down,at),0,true,t);
        t+=11ms;require(!g.update(H,hand(yaw,-0.15f,-0.3f,0.25f,Point::forward,at),0,true,t),"forward pointing rejection follows head yaw");
    }
}
void guard_settings() {
    std::stringstream text;VrGuard::Settings defaults;require(defaults.valid() && defaults.write(text),"defaults are valid and writable");
    const auto parsed=VrGuard::Settings::read(text);require(parsed && *parsed==defaults,"settings round-trip");
    std::stringstream partial{"min_height_metres=-0.5\n"};require(!VrGuard::Settings::read(partial),"incomplete settings are rejected");
    auto inverted=defaults;inverted.values.values[VrGuard::MIN_HEIGHT]=-0.1f;inverted.values.values[VrGuard::MAX_HEIGHT]=-0.2f;
    require(!inverted.valid(),"inverted zone bounds are rejected");
    // Each zone knob changes classification.
    struct Case { VrGuard::Index index; float value; float lateral,height,forward; Point point; };
    const std::array<Case,7> cases{{{VrGuard::MIN_HEIGHT,-0.2f,-0.2f,-0.3f,0.25f,Point::right},{VrGuard::MAX_HEIGHT,-0.2f,-0.2f,-0.1f,0.25f,Point::right},
        {VrGuard::MIN_FORWARD,0.22f,-0.2f,-0.1f,0.2f,Point::right},{VrGuard::MAX_ENTRY_FORWARD,0.22f,-0.2f,-0.1f,0.25f,Point::right},
        {VrGuard::MIN_LATERAL,-0.2f,-0.4f,-0.1f,0.25f,Point::right},{VrGuard::MAX_LATERAL,0.0f,0.2f,-0.1f,0.25f,Point::right},
        {VrGuard::MIN_ANGLE,90,-0.2f,-0.1f,0.25f,Point::up}}};
    for(const auto& c:cases) {
        VrGuard baseline;auto t=T0;const auto H=head(0);
        baseline.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t);
        require(baseline.update(H,hand(0,c.lateral,c.height,c.forward,c.point),0,true,t+11ms),"case is a guard with defaults");
        VrGuard tuned;auto s=defaults;s.values.values[c.index]=c.value;
        if(c.index==VrGuard::MIN_ANGLE) { auto tilted=hand(0,c.lateral,c.height,c.forward,Point::up);
            // Tilt the vertical guard 20 degrees towards forward: 70 degrees from gaze.
            const float a=0.3491f;const Vec p{0,std::cos(a),-std::sin(a)};const Vec z{-p.x,-p.y,-p.z},x=unit(cross({0,0,-1},z)),y=cross(z,x);
            tilted={x.x,x.y,x.z,0,y.x,y.y,y.z,0,z.x,z.y,z.z,0,tilted[12],tilted[13],tilted[14],1};
            VrGuard loose;loose.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t);
            require(loose.update(H,tilted,0,true,t+11ms),"a 70 degree guard passes the default 58 degree angle");
            require(tuned.set_settings(s),"angle setting accepted");tuned.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t);
            require(!tuned.update(H,tilted,0,true,t+11ms),"min_angle_degrees rejects a guard tilted towards forward");
            continue; }
        require(tuned.set_settings(s),"tuned setting accepted");
        tuned.update(H,hand(0,-0.3f,-0.8f,0,Point::down),0,true,t);
        require(!tuned.update(H,hand(0,c.lateral,c.height,c.forward,c.point),0,true,t+11ms),"each zone knob changes classification");
    }
}

// Drive the left hand along a straight line (head-yaw frame) at 90 Hz.
struct Hand { float lateral,height,forward; };
void move(VrGrenadeThrow& g,Clock::time_point& t,Hand from,Hand to,std::chrono::milliseconds duration,float grip,float yaw=0,Vec at={0,1.7f,0},Vec drift={}) {
    const int steps=std::max<int>(1,int(duration.count()/11));
    for(int i=1;i<=steps;++i) {
        const float f=float(i)/steps;t+=11ms;
        const Vec offset{drift.x*f,drift.y*f,drift.z*f};
        const Vec a{at.x+offset.x,at.y+offset.y,at.z+offset.z};
        g.update(head(yaw,a),hand(yaw,from.lateral+(to.lateral-from.lateral)*f,from.height+(to.height-from.height)*f,from.forward+(to.forward-from.forward)*f,Point::down,a),grip,true,t);
    }
}
bool step(VrGrenadeThrow& g,Clock::time_point& t,Hand h,float grip,std::chrono::milliseconds dt=11ms) { t+=dt;return g.update(head(0),hand(0,h.lateral,h.height,h.forward,Point::down),grip,true,t); }
const Hand HIP{-0.25f,-0.7f,0},FRONT{-0.1f,-0.1f,0.55f},CHEST{-0.15f,-0.4f,0.3f};

void grenade_throw() {
    VrGrenadeThrow g;auto t=T0;
    require(!step(g,t,HIP,1),"a squeeze held at enable must be released first");
    require(g.snapshot().grabs==0,"no grab from a latched squeeze");
    step(g,t,HIP,0);require(g.owns_grip(),"the belt zone owns the left grip (no parry at the hip)");
    require(!step(g,t,HIP,1) && g.snapshot().holding && g.snapshot().grabs==1,"squeeze at the left hip takes a grenade");
    move(g,t,HIP,FRONT,200ms,1);
    require(g.snapshot().holding && g.snapshot().speed>2,"throw motion measured while holding");
    require(step(g,t,FRONT,0,1ms),"a fast forward release throws (native equipment pulse)");
    require(g.snapshot().throws==1 && g.snapshot().last==VrGrenadeThrow::Outcome::thrown,"throw counted");
    require(g.snapshot().last_forward_fraction>0.5f,"release velocity is mostly forward");
    require(step(g,t,FRONT,0,100ms),"pulse lasts pulse_ms");
    require(!step(g,t,FRONT,0,30ms),"pulse ends");
    require(g.owns_grip(),"the raised follow-through remains grenade-owned after its native pulse");
    step(g,t,HIP,0);step(g,t,FRONT,0);
    require(!g.owns_grip(),"lowering the released hand returns it to ordinary gestures");

    // Slow release puts it back.
    step(g,t,HIP,0);step(g,t,HIP,1);
    move(g,t,HIP,CHEST,600ms,1);
    require(!step(g,t,CHEST,0,30ms) && g.snapshot().cancels==1 && g.snapshot().last==VrGrenadeThrow::Outcome::cancelled,"a slow release cancels without a native press");
    // Backwards flick is not a throw.
    step(g,t,CHEST,0);move(g,t,CHEST,HIP,100ms,0);step(g,t,HIP,1);
    move(g,t,HIP,{-0.3f,-0.6f,-0.4f},150ms,1);
    require(!step(g,t,{-0.3f,-0.6f,-0.4f},0,1ms) && g.snapshot().cancels==2,"a fast backward release is not a throw");
    // Squeezing outside the belt never takes a grenade.
    step(g,t,FRONT,0);require(!step(g,t,FRONT,1) && !g.snapshot().holding && g.snapshot().grabs==3,"squeeze in front is parry, not a grenade");
    step(g,t,FRONT,0);
}
void grenade_walking_and_gaps() {
    VrGrenadeThrow g;auto t=T0;
    step(g,t,HIP,0);step(g,t,HIP,1);
    // Body and hand move together quickly (room-scale step or lean): no relative motion.
    move(g,t,HIP,HIP,200ms,1,0,{0,1.7f,0},{0,0,-0.6f});
    t+=1ms;g.update(head(0,{0,1.7f,-0.6f}),hand(0,HIP.lateral,HIP.height,HIP.forward,Point::down,{0,1.7f,-0.6f}),0,true,t);
    require(g.snapshot().throws==0 && g.snapshot().cancels==1,"moving the whole body is not a throw");
    // A tracking jump during the throw cannot create speed.
    t+=11ms;g.update(head(0,{0,1.7f,-0.6f}),hand(0,HIP.lateral,HIP.height,HIP.forward,Point::down,{0,1.7f,-0.6f}),1,true,t);
    t+=11ms;g.update(head(0,{0,1.7f,-0.6f}),hand(0,FRONT.lateral,FRONT.height,FRONT.forward+2,Point::down,{0,1.7f,-0.6f}),1,true,t);
    t+=1ms;g.update(head(0,{0,1.7f,-0.6f}),hand(0,FRONT.lateral,FRONT.height,FRONT.forward+2,Point::down,{0,1.7f,-0.6f}),0,true,t);
    require(g.snapshot().throws==0,"a tracking jump is never a throw");
    // Yaw invariance: the same throw facing elsewhere.
    for(float yaw:{1.2f,-2.5f}) {
        VrGrenadeThrow r;auto u=T0;const Vec at{2,1.6f,1};
        const auto at_hand=[&](Hand h,float grip){u+=11ms;return r.update(head(yaw,at),hand(yaw,h.lateral,h.height,h.forward,Point::down,at),grip,true,u);};
        at_hand(HIP,0);at_hand(HIP,1);move(r,u,HIP,FRONT,200ms,1,yaw,at);u-=10ms;
        require(at_hand(FRONT,0) && r.snapshot().throws==1,"throw detection follows head yaw and room position");
    }
}
// Input polls can repeat one cached tracking sample (the stereo path issues poses per native
// camera job). Motion must be timed by capture, so extra polls neither dilute nor fake speed.
void grenade_cached_samples() {
    VrGrenadeThrow g;auto t=T0;auto captured=T0;
    const auto poll=[&](Hand h,float grip,int repeats){
        captured+=11ms;bool pressed{};
        for(int r=0;r<repeats;++r) { t+=4ms;pressed|=g.update(head(0),hand(0,h.lateral,h.height,h.forward,Point::down),grip,true,t,captured); }
        return pressed;
    };
    poll(HIP,0,1);poll(HIP,1,1);
    for(int i=1;i<=18;++i) { const float f=i/18.0f;poll({HIP.lateral+(FRONT.lateral-HIP.lateral)*f,HIP.height+(FRONT.height-HIP.height)*f,HIP.forward+(FRONT.forward-HIP.forward)*f},1,3); }
    const float speed=g.snapshot().speed;
    require(speed>2,"throw speed from capture times");
    // Five more polls of the same capture, 20 ms of poll time later, before the release sample.
    for(int r=0;r<5;++r) { t+=4ms;g.update(head(0),hand(0,FRONT.lateral,FRONT.height,FRONT.forward,Point::down),1,true,t,captured); }
    require(std::abs(g.snapshot().speed-speed)<1e-4f,"repeated polls of one capture do not change speed");
    t+=4ms;require(g.update(head(0),hand(0,FRONT.lateral,FRONT.height,FRONT.forward,Point::down),0,true,t,captured),"release on a repeated sample still throws");
    require(g.snapshot().throws==1,"one throw");
}
void grenade_modes_and_settings() {
    VrGrenadeThrow g;auto s=g.settings();s.values.values[VrGrenadeThrow::HOLD_BUTTON]=1;require(g.set_settings(s),"hold mode accepted");
    auto t=T0;step(g,t,HIP,0);
    require(step(g,t,HIP,1),"hold mode presses the native button from the grab");
    move(g,t,HIP,FRONT,200ms,1);require(g.snapshot().pressed,"held while aiming");
    require(!step(g,t,FRONT,0,1ms) && g.snapshot().throws==1,"hold mode releases on throw (the game throws on release)");
    // Timeout.
    VrGrenadeThrow h;t=T0;step(h,t,HIP,0);step(h,t,HIP,1);
    require(h.snapshot().holding,"holding");
    step(h,t,HIP,1,9s);require(!h.snapshot().holding && h.snapshot().last==VrGrenadeThrow::Outcome::timeout,"held too long puts the grenade back");
    require(!step(h,t,HIP,0) && h.snapshot().throws==0,"release after a timeout throws nothing");
    // Speed and forward knobs change the outcome of the same throw.
    for(auto [index,value]:{std::pair{VrGrenadeThrow::THROW_SPEED,5.0f},std::pair{VrGrenadeThrow::FORWARD_FRACTION,1.0f}}) {
        VrGrenadeThrow k;auto ks=k.settings();ks.values.values[index]=value;require(k.set_settings(ks),"knob accepted");
        auto u=T0;step(k,u,HIP,0);step(k,u,HIP,1);move(k,u,HIP,FRONT,200ms,1);
        require(!step(k,u,FRONT,0,1ms) && k.snapshot().throws==0,"stricter speed/direction knobs reject the default throw");
    }
    VrGrenadeThrow z;auto zs=z.settings();zs.values.values[VrGrenadeThrow::BELT_RADIUS]=0.2f;require(z.set_settings(zs),"zone knob accepted");
    t=T0;step(z,t,HIP,0);step(z,t,HIP,1);require(!z.snapshot().holding,"belt radius changes acquisition reach");
    std::stringstream text;require(VrGrenadeThrow::Settings{}.write(text),"defaults writable");
    require(VrGrenadeThrow::Settings::read(text)==VrGrenadeThrow::Settings{},"settings round-trip");
    std::stringstream bad{"max_height_metres=nan\n"};require(!VrGrenadeThrow::Settings::read(bad),"malformed settings rejected");
    VrGrenadeThrow d;t=T0;step(d,t,HIP,0);step(d,t,HIP,1);
    t+=11ms;d.update(head(0),hand(0,HIP.lateral,HIP.height,HIP.forward,Point::down),1,false,t);
    require(!d.snapshot().holding && d.snapshot().last==VrGrenadeThrow::Outcome::lost,"gate loss drops a held grenade");
}
void grenade_release_ownership() {
    VrGrenadeThrow g;auto t=T0;
    step(g,t,HIP,0);step(g,t,HIP,1);
    move(g,t,HIP,FRONT,600ms,1);
    require(!step(g,t,FRONT,0,30ms) && g.snapshot().last==VrGrenadeThrow::Outcome::cancelled,"slow raised release cancels");
    require(g.owns_grip(),"cancelled raised hand remains grenade follow-through, never instant parry");
    step(g,t,FRONT,1);require(g.owns_grip() && !g.snapshot().holding,"re-squeeze during raised follow-through cannot leak to parry or grab");
    step(g,t,HIP,1);require(g.owns_grip(),"lowering while still squeezed does not hand the grip to parry");
    step(g,t,HIP,0);step(g,t,FRONT,0);
    require(!g.owns_grip(),"release then lower re-arms ordinary guard/support");

    step(g,t,HIP,0);step(g,t,HIP,1);step(g,t,FRONT,1,9s);
    require(g.snapshot().last==VrGrenadeThrow::Outcome::timeout && !g.snapshot().holding && g.owns_grip(),"timeout still owns the squeezed grip until release and lowering");
    step(g,t,FRONT,0);require(g.owns_grip(),"timeout release in guard zone remains quiet");
    step(g,t,HIP,0);step(g,t,FRONT,0);require(!g.owns_grip(),"timeout recovery rearms after release and lowering");
    step(g,t,HIP,0);step(g,t,HIP,1);
    t+=11ms;require(!g.update(std::nullopt,std::nullopt,1,true,t) && !g.owns_grip() && !g.snapshot().pressed,"tracking loss releases button and transaction ownership immediately");
    step(g,t,FRONT,1);require(!g.snapshot().holding && !g.snapshot().pressed,"held squeeze at tracking recovery cannot grab or throw");
}
void grenade_belt_head_looks() {
    // A controller near the physical waist does not move when the player looks
    // sideways. There is no torso tracker; handedness plus bounded radius supplies
    // belt ownership rather than pretending the player's gaze is their torso.
    for(float body_yaw:{0.0f,1.5707963f,3.1415926f,-1.5707963f}) {
        for(float look:{0.0f,1.5707963f,3.1415926f,-1.5707963f}) {
            for(float pitch:{-1.3962634f,0.0f,1.3962634f}) {
                VrGrenadeThrow g;auto t=T0;const Vec at{1.1f,1.65f,-.8f};
                auto H=head(body_yaw+look,at);
                const auto yaw=H;const float c=std::cos(pitch),s=std::sin(pitch);
                for(int j=0;j<3;++j){H[4+j]=yaw[4+j]*c-yaw[8+j]*s;H[8+j]=yaw[4+j]*s+yaw[8+j]*c;}
                const auto L=hand(body_yaw,-.25f,-.65f,.03f,Point::down,at);
                g.update(H,L,0,true,t);g.update(H,L,1,true,t+11ms);
                require(g.snapshot().holding && g.owns_grip(),"belt acquisition survives whole-body turns and independent head yaw/pitch");
            }
        }
    }
    // Exact missed squeeze classifications from the physical trace: previously
    // only the gaze-relative +lateral sign sent these close waist squeezes to LB.
    for(Hand h:std::array<Hand,3>{{{.038f,-.647f,.075f},{.040f,-.608f,-.022f},{.108f,-.648f,-.035f}}}) {
        VrGrenadeThrow g;auto t=T0;step(g,t,h,0);step(g,t,h,1);
        require(g.snapshot().holding,"measured near-centre belt miss becomes grenade acquisition");
    }
    VrGrenadeThrow far;auto t=T0;const Hand outside{-1,-.7f,0};
    step(far,t,outside,0);step(far,t,outside,1);
    require(!far.snapshot().holding && !far.owns_grip(),"waist acquisition is bounded horizontally, including the previously unbounded left side");
    // A front squeeze cannot later acquire by passing through the belt.
    VrGrenadeThrow held;t=T0;step(held,t,FRONT,0);step(held,t,FRONT,1);step(held,t,HIP,1);
    require(!held.snapshot().holding && held.snapshot().grabs==0,"a fresh squeeze must still start in the belt");
    step(held,t,HIP,0);step(held,t,HIP,1);require(held.snapshot().holding,"release and fresh belt squeeze acquires");
}
void grenade_settings_migration() {
    const std::string legacy="max_height_metres=-0.45\nmin_height_metres=-1.2\nmax_lateral_metres=-0.3\nmax_abs_forward_metres=0.4\nthrow_speed_metres_per_second=2.1\nforward_fraction=0.6\npulse_ms=150\nhold_button=1\nmax_hold_seconds=5\n";
    std::stringstream old{legacy};const auto migrated=VrGrenadeThrow::Settings::read(old);
    require(migrated && (*migrated)[VrGrenadeThrow::BELT_RADIUS]==.4f && (*migrated)[VrGrenadeThrow::MAX_HEIGHT]==-.45f &&
        (*migrated)[VrGrenadeThrow::MIN_HEIGHT]==-1.2f && (*migrated)[VrGrenadeThrow::THROW_SPEED]==2.1f &&
        (*migrated)[VrGrenadeThrow::FORWARD_FRACTION]==.6f && (*migrated)[VrGrenadeThrow::PULSE]==150 &&
        migrated->hold_mode() && (*migrated)[VrGrenadeThrow::MAX_HOLD]==5,"complete legacy file retains all meaningful tuning and maps reach to radius");
    std::stringstream output;require(migrated->write(output) && output.str().find("max_lateral")==std::string::npos && output.str().find("belt_radius_metres=")!=std::string::npos,"writes only the new eight-key schema");
    require(VrGrenadeThrow::Settings::read(output)==migrated,"migrated settings round-trip");
    for(const auto& text:std::array<std::string,6>{legacy+"belt_radius_metres=0.35\n",legacy+"max_lateral_metres=-0.3\n",
        legacy.substr(0,legacy.find("max_hold_seconds")),legacy+"unknown=1\n",
        std::string{"max_height_metres=-0.4\nmin_height_metres=-1.1\nbelt_radius_metres=.35\nthrow_speed_metres_per_second=1.5\nforward_fraction=.3\npulse_ms=120\nhold_button=0\n"},
        std::string{"max_height_metres=-0.4\nmin_height_metres=-1.1\nbelt_radius_metres=.35\nthrow_speed_metres_per_second=1.5\nforward_fraction=.3\npulse_ms=120\nhold_button=0\nmax_hold_seconds=8\nbelt_radius_metres=.4\n"}}) {
        std::stringstream invalid{text};require(!VrGrenadeThrow::Settings::read(invalid),"mixed, duplicate, unknown and incomplete schemas rejected");
    }
    for(auto [index,value,h]:std::array<std::tuple<VrGrenadeThrow::Index,float,Hand>,2>{{{VrGrenadeThrow::MAX_HEIGHT,-.8f,HIP},{VrGrenadeThrow::MIN_HEIGHT,-.6f,HIP}}}) {
        VrGrenadeThrow g;auto s=g.settings();s.values.values[index]=value;require(g.set_settings(s),"height tuning accepted");auto t=T0;
        step(g,t,h,0);step(g,t,h,1);require(!g.snapshot().holding,"each height tuning bound changes acquisition");
    }
    VrGrenadeThrow pulse;auto ps=pulse.settings();ps.values.values[VrGrenadeThrow::PULSE]=250;require(pulse.set_settings(ps),"pulse tuning accepted");auto t=T0;
    step(pulse,t,HIP,0);step(pulse,t,HIP,1);move(pulse,t,HIP,FRONT,200ms,1);step(pulse,t,FRONT,0,1ms);
    require(step(pulse,t,FRONT,0,200ms) && !step(pulse,t,FRONT,0,51ms),"pulse tuning changes native button lifetime");
    VrGrenadeThrow timeout;auto ts=timeout.settings();ts.values.values[VrGrenadeThrow::MAX_HOLD]=2;require(timeout.set_settings(ts),"timeout tuning accepted");t=T0;
    step(timeout,t,HIP,0);step(timeout,t,HIP,1);step(timeout,t,HIP,1,3s);
    require(timeout.snapshot().last==VrGrenadeThrow::Outcome::timeout && !timeout.snapshot().holding,"timeout tuning changes held lifetime");
}
} // namespace

// Launch direction math (VrGrenadeAim): head-yaw frame, so snap turns and head yaw are
// handled by the caller's frame conversion; tested in test_vr_session.cpp.
bool close_to(float a,float b,float tolerance=0.01f) { return std::abs(a-b)<=tolerance; }
void grenade_aim_direction() {
    using A=VrGrenadeAim;
    A::Settings s;
    const auto set=[&](A::Index i,float v){s.values.values[i]=v;require(s.valid(),"setting valid");};
    // Defaults (Joe, 21 Oct): exactly the hand's release direction.
    auto a=A::aim({0,0,4},s);
    require(close_to(a.yaw_degrees,0) && close_to(a.pitch_degrees,0) && a.hand_yaw_valid,"straight forward level throw: level, along the gaze");
    a=A::aim({4*std::tan(0.5236f),-4*std::tan(0.6981f),4},s); // 40 degrees left, rising (about 24 degrees)
    require(close_to(a.hand_yaw_degrees,-40,.1f) && close_to(a.yaw_degrees,-40,.1f),"default yaw is the hand's");
    require(a.hand_pitch_degrees>20 && close_to(a.pitch_degrees,a.hand_pitch_degrees,.1f),"default pitch is the hand's");
    const float length=std::sqrt(a.direction.height*a.direction.height+a.direction.lateral*a.direction.lateral+a.direction.forward*a.direction.forward);
    require(close_to(length,1,1e-4f) && a.direction.lateral<0 && a.direction.height>0,"direction is a unit vector, left and up");
    a=A::aim({0,3,1},s);require(close_to(a.yaw_degrees,a.hand_yaw_degrees,.1f) && a.hand_yaw_degrees>70,"no gaze limit by default: a sideways throw goes sideways");
    a=A::aim({0,-1,-1},s);require(close_to(std::abs(a.yaw_degrees),135,.1f),"even behind the gaze (the gesture rejects those; the math does not clamp)");
    a=A::aim({5,0,1},s);require(close_to(a.pitch_degrees,60),"pitch clamped to max_pitch");
    a=A::aim({-5,0,1},s);require(close_to(a.pitch_degrees,-30),"pitch clamped to min_pitch");
    a=A::aim({3,0.2f,0.1f},s);require(!a.hand_yaw_valid && close_to(a.yaw_degrees,0),"an almost vertical release has no hand yaw: the gaze decides");
    a=A::aim({std::nanf(""),0,1},s);require(!a.hand_yaw_valid && std::isfinite(a.direction.forward) && close_to(a.yaw_degrees,0) && close_to(a.speed_scale,.35f),"invalid velocity falls back to gaze and the slowest launch");
    // Gaze blend still available.
    set(A::HAND_YAW_WEIGHT,0);set(A::HAND_PITCH_WEIGHT,0);
    a=A::aim({3,2,1},s);require(close_to(a.yaw_degrees,0) && close_to(a.pitch_degrees,10),"weights 0: where the player looks, with the native lob");
    set(A::HAND_YAW_WEIGHT,.5f);set(A::HAND_PITCH_WEIGHT,.5f);set(A::MAX_YAW,15);
    a=A::aim({0,-1,1},s);require(close_to(a.yaw_degrees,-15,.1f) && close_to(a.pitch_degrees,5,.1f),"half weights blend; max_yaw_from_gaze limits the blend");
    // Speed: proportional to the hand, clamped to a band of the native speed.
    const A::Settings d;
    require(close_to(A::aim({0,0,4},d).speed_scale,1),"a 4 m/s release is the native throw");
    require(close_to(A::aim({0,0,2},d).speed_scale,.5f) && close_to(A::aim({1.2f,0,1.6f},d).speed_scale,.5f),"speed is the full 3D release speed over the reference");
    require(close_to(A::aim({0,0,6},d).speed_scale,1.5f) && close_to(A::aim({0,0,20},d).speed_scale,1.5f),"hard throws are capped at max_speed_fraction");
    require(close_to(A::aim({0,0,.5f},d).speed_scale,.35f),"soft throws keep min_speed_fraction");
    require(close_to(A::aim({0,0,3},d).hand_speed,3),"hand speed recorded");
    // Head-yaw frame to tracking space round trip at several gaze yaws.
    for(float yaw:{0.0f,0.7f,-2.2f,3.1f}) {
        const std::array<float,2> f{std::sin(yaw),-std::cos(yaw)};
        const vr_body::Offset o{.3f,-.5f,.81f};const auto t=A::tracking(f,o);const auto back=vr_body::direction(f,t[0],t[1],t[2]);
        require(close_to(back.height,o.height,1e-4f) && close_to(back.lateral,o.lateral,1e-4f) && close_to(back.forward,o.forward,1e-4f),"tracking() inverts vr_body::direction");
    }
    // Settings file: round trip, strict schema, ordering rule.
    std::stringstream file;require(s.write(file),"aim settings write");
    const auto read=A::Settings::read(file);require(read && *read==s,"aim settings round trip");
    std::istringstream partial{"hand_yaw_weight=0.5\n"};require(!A::Settings::read(partial),"partial aim file rejected");
    std::istringstream old{"hand_yaw_weight=0.5\nmax_yaw_from_gaze_degrees=45\nhand_pitch_weight=0.5\nbase_pitch_degrees=10\nmin_pitch_degrees=-10\nmax_pitch_degrees=45\n"};
    require(!A::Settings::read(old),"a 20 October file (no speed keys) is rejected, so the new hand defaults apply");
    A::Settings inverted;inverted.values.values[A::MIN_PITCH]=30;inverted.values.values[A::MAX_PITCH]=20;require(!inverted.valid(),"min pitch above max rejected");
    VrGrenadeThrow g;require(!g.set_aim_settings(inverted) && g.aim_settings()==A::Settings{},"invalid aim settings refused");
}
// Joe's six OpenXR belt throws (run 20261020_180150_xr_fit_joe), release velocity in the
// head-yaw frame {height, lateral, forward} and the release speed the trace recorded.
void grenade_aim_measured_throws() {
    struct Release { vr_body::Offset velocity; float traced_speed; };
    const std::array<Release,6> releases{{{{.374f,-.664f,2.309f},2.431f},{{1.328f,-2.423f,3.193f},4.222f},{{1.895f,-2.852f,2.359f},4.158f},
        {{1.123f,-1.303f,2.413f},2.963f},{{2.197f,-2.07f,2.916f},4.197f},{{1.907f,-2.111f,2.608f},3.859f}}};
    const VrGrenadeAim::Settings defaults;float slowest=10,fastest=0;
    for(const auto& r:releases) {
        const auto a=VrGrenadeAim::aim(r.velocity,defaults);
        require(close_to(a.hand_speed,r.traced_speed,.01f),"fixture: hand speed matches the traced release speed");
        require(a.hand_yaw_degrees<-15 && a.hand_yaw_degrees>-51 && close_to(a.yaw_degrees,a.hand_yaw_degrees,.01f),"each measured throw flies where that hand went (16-50 degrees left of the gaze)");
        require(a.pitch_degrees>8 && a.pitch_degrees<33 && close_to(a.pitch_degrees,a.hand_pitch_degrees,.01f),"each measured throw keeps its own release angle (9-32 degrees up)");
        require(a.speed_scale>=.6f && a.speed_scale<=1.06f,"measured throws stay inside the native speed band, unclamped");
        require(close_to(a.speed_scale,r.traced_speed/4,.01f),"launch speed is proportional to the hand");
        slowest=std::min(slowest,a.speed_scale);fastest=std::max(fastest,a.speed_scale);
    }
    require(fastest/slowest>1.7f,"the softest measured throw launches much slower than the hardest");
}
// The gesture records the aim of the throw it detected, in its own head-yaw frame.
void grenade_aim_recorded_on_throw() {
    for(float yaw:{0.0f,1.2f,-2.5f}) {
        VrGrenadeThrow g;auto t=T0;const Vec at{1,1.6f,-2};
        const auto at_hand=[&](Hand h,float grip){t+=11ms;return g.update(head(yaw,at),hand(yaw,h.lateral,h.height,h.forward,Point::down,at),grip,true,t);};
        at_hand(HIP,0);at_hand(HIP,1);
        move(g,t,HIP,{HIP.lateral+.45f,HIP.height+.2f,HIP.forward+.45f},150ms,1,yaw,at); // Forward and right (45 degrees), rising.
        t-=10ms;require(at_hand({HIP.lateral+.45f,HIP.height+.2f,HIP.forward+.45f},0) && g.snapshot().throws==1,"throw detected");
        const auto a=g.snapshot().last_aim;
        require(close_to(a.hand_yaw_degrees,45,1) && close_to(a.yaw_degrees,45,1) && a.pitch_degrees>10,"recorded aim follows the hand relative to the gaze at any head yaw");
        require(close_to(a.hand_speed,g.snapshot().last_release_speed,.01f) && close_to(a.speed_scale,std::clamp(a.hand_speed/4,.35f,1.5f),.01f),"recorded speed is the release speed");
    }
}
// Left-arm push (knockback). Thrust from the chest (lateral -0.1, height -0.45, forward 0.10)
// to `reach` forward over `ms`, sampled every 11 ms (90 Hz).
struct PushRun { uint64_t pushes{};bool pressed{}; };
PushRun thrust(VrLeftPush& p,Clock::time_point& t,float reach,int ms,VrLeftPush::Input base={},float yaw=0,Vec at={0,1.7f,0},float turn=0) {
    PushRun run;const int steps=std::max(1,ms/11);
    for(int i=0;i<=steps+6;++i) {
        const float f=std::min(1.f,float(i)/steps);
        auto in=base;in.head=head(yaw+turn*f,at);in.hand=hand(yaw,-.1f,-.45f,.10f+(reach-.10f)*f,Point::up,at);
        run.pressed|=p.update(in,t);t+=11ms;
    }
    run.pushes=p.snapshot().pushes;return run;
}
void rest(VrLeftPush& p,Clock::time_point& t,VrLeftPush::Input base={}) {
    for(int i=0;i<30;++i) { auto in=base;in.head=head(0);in.hand=hand(0,-.1f,-.45f,.10f,Point::up);p.update(in,t);t+=11ms; }
}
void left_push() {
    VrLeftPush::Input on{};on.allowed=true;
    { VrLeftPush p;auto t=T0;rest(p,t,on);
      const auto r=thrust(p,t,.62f,170,on); // Joe, 1447.0 s: 0.52 m in 0.17 s.
      require(r.pressed && r.pushes==1,"an open left hand thrust straight out pushes once");
      for(int i=0;i<100;++i) { auto in=on;in.head=head(0);in.hand=hand(0,-.1f,-.45f,.62f+(i%2)*.04f,Point::up);p.update(in,t);t+=11ms; }
      require(p.snapshot().pushes==1,"held out (shaking), the arm must come back before another push");
      t+=1s;rest(p,t,on);require(thrust(p,t,.62f,170,on).pushes==2,"after bringing it back, a second push"); }
    { VrLeftPush p;auto t=T0;rest(p,t,on);require(!thrust(p,t,.62f,600,on).pushes,"a slow reach (0.9 m/s) is not a push"); }
    { VrLeftPush p;auto t=T0;rest(p,t,on);require(!thrust(p,t,.30f,100,on).pushes,"a short jab that ends close to the body is not a push"); }
    { VrLeftPush p;auto t=T0;auto in=on;in.grenade=true;rest(p,t,in);require(!thrust(p,t,.62f,170,in).pushes,"a held grenade is not a push"); }
    // Joe, 22 Oct: a left punch is a push too, so a fist, a squeezed grip or lost finger
    // tracking does not matter (the push no longer reads the grip or finger curls).
    { VrLeftPush p;auto t=T0;auto in=on;in.right_speed=1.2f;rest(p,t,in);
      require(thrust(p,t,.62f,170,in).pushes==1,"a left punch twisting the right controller (right wrist 1.2 m/s, 20 Oct 561.7 s) pushes"); }
    { VrLeftPush p;auto t=T0;auto in=on;in.trigger=.6f;rest(p,t,in);in.trigger=0;
      require(!thrust(p,t,.62f,170,in).pushes,"just after the pistol trigger (300 ms) is not a push");
      t+=1s;rest(p,t,in);require(thrust(p,t,.62f,170,in).pushes==1,"once the trigger has been released a while, it is"); }
    { VrLeftPush p;auto t=T0;auto in=on;in.right_speed=4;rest(p,t,in);require(!thrust(p,t,.62f,170,in).pushes,"both arms flailing is not a push"); }
    { VrLeftPush p;auto t=T0;auto in=on;in.guard=true;rest(p,t,in);require(!thrust(p,t,.62f,170,in).pushes,"a raised guard is not a push"); }
    { VrLeftPush p;auto t=T0;auto in=on;in.allowed=false;rest(p,t,in);require(!thrust(p,t,.62f,170,in).pushes,"not allowed (bolter out, two-handed, menus): no push"); }
    { VrLeftPush p;auto t=T0;
      for(int i=0;i<30;++i) { auto in=on;in.head=head(0);in.hand=hand(0,-.45f,-.45f,.05f,Point::up);p.update(in,t);t+=11ms; }
      // The hand held 0.45 m to the left, the head turning 80 degrees towards it: in the gaze
      // frame it swings round to the front, but it did not move.
      for(int i=0;i<=20;++i) { auto in=on;in.head=head(-1.4f*i/20);in.hand=hand(0,-.45f,-.45f,.05f,Point::up);p.update(in,t);t+=11ms; }
      require(!p.snapshot().pushes,"a head turn towards a still hand is not a thrust"); }
    for(float yaw:{.8f,-2.4f}) { VrLeftPush p;auto t=T0;const Vec at{1.2f,1.6f,-.5f};
      for(int i=0;i<30;++i) { auto in=on;in.head=head(yaw,at);in.hand=hand(yaw,-.1f,-.45f,.10f,Point::up,at);p.update(in,t);t+=11ms; }
      require(thrust(p,t,.62f,170,on,yaw,at).pushes==1,"the push follows head yaw and room position"); }
}
int main() {
    try {
        left_push();
        grenade_aim_direction();grenade_aim_measured_throws();grenade_aim_recorded_on_throw();
        guard_pose();guard_timing();guard_aim_latch_and_yield();guard_entry_near_face();guard_belt_rest();guard_yaw_invariance();guard_settings();
        grenade_throw();grenade_walking_and_gaps();grenade_cached_samples();grenade_modes_and_settings();grenade_release_ownership();grenade_belt_head_looks();grenade_settings_migration();
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
    std::cout<<"vr_gestures: all checks passed\n";return 0;
}
