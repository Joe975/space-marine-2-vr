#include "VrHolster.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
using Slot=VrHolster::Slot;
using Clock=VrHolster::Clock;
struct Vec { float x,y,z; };
Vec cross(Vec a,Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec unit(Vec v) { const float l=std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);return {v.x/l,v.y/l,v.z/l}; }
// OpenVR standing space: +Y up, gaze along -Z. Yaw turns right, pitch looks up.
vr_camera::Matrix head(float yaw,float pitch,Vec at={0,1.7f,0}) {
    const Vec forward{std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)};
    const Vec z{-forward.x,-forward.y,-forward.z},x=unit(cross({0,1,0},z)),y=cross(z,x);
    return {x.x,x.y,x.z,0, y.x,y.y,y.z,0, z.x,z.y,z.z,0, at.x,at.y,at.z,1};
}
// Hand at (lateral right, height, forward along gaze yaw) from a yawed head position.
vr_camera::Matrix hand(float yaw,float lateral,float height,float forward,Vec at={0,1.7f,0}) {
    auto m=vr_camera::IDENTITY;
    m[12]=at.x+lateral*std::cos(yaw)+forward*std::sin(yaw);
    m[13]=at.y+height;
    m[14]=at.z+lateral*std::sin(yaw)-forward*std::cos(yaw);
    return m;
}

void zones() {
    VrHolster h;
    require(h.classify(head(0,0),hand(0,0.1f,-0.1f,-0.1f))==Slot::primary,"hand behind the right ear selects the shoulder holster");
    require(h.classify(head(0,0),hand(0,0.25f,-0.7f,0.05f))==Slot::secondary,"hand at the right hip selects the hip holster");
    require(h.classify(head(0,0),hand(0,.273f,-.462f,-.064f))==Slot::secondary,"20:51 physical waist squeeze is inside the hip zone");
    require(h.classify(head(0,0),hand(0,.9f,-.7f,0))==Slot::none,"outstretched hand at hip height cannot select sidearm");
    require(h.classify(head(0,0),hand(0,.2f,-.2f,-.8f))==Slot::none,"unbounded reach behind the head cannot select primary");
    require(h.classify(head(0,0),hand(0,0.15f,-0.35f,0.05f))==Slot::primary,"a hand resting on top of the right shoulder selects the primary (playtest 17 Oct)");
    // Every recorded shoulder draw (19-21 Oct, lateral/height/forward) selects the primary.
    for(const auto& d:{Vec{.20f,-.10f,-.24f},Vec{.15f,-.19f,-.22f},Vec{.28f,-.03f,-.14f},Vec{.31f,-.04f,-.10f},Vec{.17f,-.08f,-.29f},
                       Vec{.19f,-.09f,-.26f},Vec{.18f,-.09f,-.25f},Vec{.30f,-.13f,-.10f},Vec{.19f,-.09f,-.20f}})
        require(h.classify(head(0,0),hand(0,d.x,d.y,d.z))==Slot::primary,"a recorded shoulder draw selects the primary");
    // Joe, 22 Oct: the 21 Oct night resize is undone (he meant the pauldron in view). The zone
    // again reaches 5 cm above the eyes, 15 cm left of centre and 10 cm in front.
    for(const auto& d:{Vec{.2f,.03f,-.1f},Vec{0,-.1f,-.1f},Vec{.2f,-.1f,.08f},Vec{.15f,-.38f,-.05f}})
        require(h.classify(head(0,0),hand(0,d.x,d.y,d.z))==Slot::primary,"the shoulder zone keeps its 17-21 Oct size");
    require(h.classify(head(0,0),hand(0,0.15f,-0.35f,0.2f))==Slot::none,"a hand held in front of the shoulder is not a holster");
    require(h.classify(head(0,0),hand(0,0.15f,-0.25f,0.4f))==Slot::none,"a raised aiming hand in front is not a holster");
    require(h.classify(head(0,0),hand(0,0.2f,-0.4f,0.3f))==Slot::none,"a lowered hand in front of the chest is not a holster");
    require(h.classify(head(0,0),hand(0,-0.3f,-0.7f,0))==Slot::none,"the left hip is not the right-hip holster");
    require(h.classify(head(0,0),hand(0,0.25f,-1.4f,0))==Slot::none,"a hand near the floor is below the hip holster");
    require(h.classify(head(0,0),hand(0,0.25f,-0.7f,0.5f))==Slot::none,"hip holster has a bounded forward reach");
    for(float yaw:{0.7f,-2.1f,3.0f}) {
        const Vec at{1.3f,1.6f,-0.4f};
        require(h.classify(head(yaw,0,at),hand(yaw,0.1f,-0.1f,-0.1f,at))==Slot::primary,"shoulder zone follows head yaw and room position");
        require(h.classify(head(yaw,0,at),hand(yaw,0.25f,-0.7f,0.05f,at))==Slot::secondary,"hip zone follows head yaw and room position");
        require(h.classify(head(yaw,0,at),hand(yaw,0.15f,-0.25f,0.4f,at))==Slot::none,"aiming stays outside zones at any yaw");
    }
    for(float pitch:{-1.48f,-0.9f,0.6f,1.2f}) {
        require(h.classify(head(0.5f,pitch),hand(0.5f,0.25f,-0.7f,0.05f))==Slot::secondary,"looking down at the hip or up must not move the hip zone");
        require(h.classify(head(0.5f,pitch),hand(0.5f,0.1f,-0.1f,-0.1f))==Slot::primary,"pitch must not move the shoulder zone");
    }
    require(std::abs(h.snapshot().lateral-0.1f)<1e-4f && std::abs(h.snapshot().height+0.1f)<1e-4f && std::abs(h.snapshot().forward+0.1f)<1e-4f,
        "snapshot reports the hand's head-relative position for tuning");
}

void selection() {
    using std::chrono::milliseconds;
    const auto t0=Clock::time_point{std::chrono::seconds(5)};
    auto h0=head(0,0);const auto hip=hand(0,.25f,-.7f,0),up=hand(0,.25f,.45f,0),front=hand(0,.25f,-.2f,.5f);
    VrHolster h;
    auto step=[&](int ms,const auto& pose,float grip,bool allowed=true,std::optional<bool> equipped=std::nullopt){return h.update(h0,pose,grip,allowed,t0+milliseconds(ms),equipped);};
    require(step(0,up,1)==Slot::none,"held fist at enable must not salute");
    step(10,up,0);require(step(20,up,1)==Slot::none,"release/regrip overhead cannot arm a salute");
    step(30,front,0);step(40,front,1);
    require(step(60,up,1)==Slot::salute && h.snapshot().salutes==1,"lifting an already closed fist salutes, never selects primary");
    require(step(140,up,1)==Slot::salute && step(161,up,1)==Slot::none,"emote is a bounded native pulse");
    step(500,up,0);require(step(510,up,1)==Slot::none && h.snapshot().salutes==1,"no repeats while overhead");
    step(600,hip,1);require(step(700,up,1)==Slot::none,"lowering without releasing cannot repeat");
    step(800,hip,0);require(step(820,up,1)==Slot::salute && h.snapshot().salutes==2,"lower and release re-arms");
    require(step(830,up,1,false)==Slot::none && !h.owns_grip(),"focus loss releases gesture");
    require(step(850,up,1)==Slot::none,"recovery with held fist cannot fire");
    require(h.update(std::nullopt,up,1,true,t0+milliseconds(900))==Slot::none,"tracking loss releases gesture");
    step(1000,front,0);step(1020,front,1);require(!h.owns_grip(),"forward weapon grip is not reserved");
    auto invalid=up;invalid[0]=2;require(step(1040,invalid,1)==Slot::none,"non-rigid tracking is refused");
    // 31 Oct (Joe): weapon swap is the right hip grip only. A squeeze at the hip always swaps, so its
    // draw quiets swings until the grip is released (the 21 Oct grip-and-swing out of the hip is gone).
    step(1500,hip,0);require(step(1520,hip,1)==Slot::secondary && h.owns_grip(),"a hip squeeze swaps and consumes the grip");
    step(1530,hand(0,.25f,-.45f,.35f),1);require(h.suppresses_swing(t0+milliseconds(1530)),"the swap's draw quiets swings while the grip is held");
    step(1540,hand(0,.25f,.22f,0),1);require(h.suppresses_swing(t0+milliseconds(1540)),"near the emote height the rising fist owns the hand until released");
    step(2600,front,0);require(step(2620,hand(0,.1f,-.1f,-.1f),1,true,true)==Slot::none && h.owns_grip(),"a squeeze over the shoulder draws nothing (31 Oct) but still owns the grip");
    require(h.snapshot().primary==0 && h.snapshot().secondary==1,"only the hip swapped");
    for(float yaw:{0.7f,-2.1f,3.0f}) for(float pitch:{-1.2f,1.2f}) {
        VrHolster rotated;auto view=head(yaw,pitch);
        rotated.update(view,hand(yaw,.25f,-.7f,0),0,true,t0);
        require(rotated.update(view,hand(yaw,.25f,.45f,0),1,true,t0+milliseconds(20))==Slot::salute,"salute follows yaw and ignores gaze pitch");
    }
}

void sidearm_selection() {
    using std::chrono::milliseconds;
    const auto t0=Clock::time_point{std::chrono::seconds(10)};
    const auto view=head(0,0),hip=hand(0,.25f,-.7f,0);
    VrHolster h;
    const auto step=[&](int ms,float grip,std::optional<bool> equipped){return h.update(view,hip,grip,true,t0+milliseconds(ms),equipped);};
    step(0,0,false);
    require(step(20,1,false)==Slot::secondary && h.snapshot().secondary==1,"hip selects sidearm when primary is observed");
    require(step(80,1,true)==Slot::secondary,"new sidearm observation must not truncate the native pulse");
    require(step(121,1,true)==Slot::none,"sidearm request is a bounded pulse");
    step(500,0,true);require(step(520,1,true)==Slot::primary && h.snapshot().primary==1,"with the sidearm out the hip swaps back to the primary (31 Oct)");
    step(1000,0,std::nullopt);require(step(1020,1,std::nullopt)==Slot::secondary && h.owns_grip(),"with the weapon unknown the hip still cycles");
    {   // 25 Oct: a loadout unknown for a while (the battle simulator: the gun in neither hand) cycles.
        VrHolster u;const auto at=[&](int ms,const vr_camera::Matrix& pose,float grip){return u.update(view,pose,grip,true,t0+milliseconds(ms),std::nullopt,true);};
        at(0,hip,0);require(at(20,hip,1)==Slot::secondary && u.snapshot().secondary==1,"hip cycles while the loadout stays unknown");
        const auto shoulder_pose=hand(0,.1f,-.1f,-.1f);
        at(500,shoulder_pose,0);require(at(520,shoulder_pose,1)==Slot::none && u.snapshot().primary==0,"the shoulder never draws (31 Oct)");}
    step(1500,0,false);require(step(1520,1,false)==Slot::secondary && h.snapshot().secondary==3,"hip selects again after primary returns");
    const auto shoulder=hand(0,.1f,-.1f,-.1f);
    for(const bool sidearm:{true,false}){const int at=sidearm?2000:2500;h.update(view,shoulder,0,true,t0+milliseconds(at),sidearm);
        require(h.update(view,shoulder,1,true,t0+milliseconds(at+20),sidearm)==Slot::none,"the shoulder draws nothing with either weapon out (31 Oct)");}
}

void settings() {
    VrHolster::Settings s;std::stringstream text;
    require(s.valid() && s.write(text),"default zones are valid");
    auto read=VrHolster::Settings::read(text);require(read && *read==s,"settings round-trip");
    std::istringstream partial{"shoulder_min_height=-0.2\n"};require(!VrHolster::Settings::read(partial),"partial settings are refused");
    auto inverted=s;inverted.values[4]=-0.3f;require(!inverted.valid(),"hip bottom above hip top is refused");
    VrHolster h;auto low=s;low.values[3]=-0.8f;low.values[4]=-1.3f;
    require(h.set_settings(low) && h.classify(head(0,0),hand(0,0.25f,-0.7f,0.05f))==Slot::none &&
        h.classify(head(0,0),hand(0,0.25f,-1.0f,0.05f))==Slot::secondary,"a live hip-height change moves the actual zone");
    auto high=s;high.values[6]=.5f;require(h.set_settings(high) && h.classify(head(0,0),hand(0,.25f,.3f,0))==Slot::none &&
        h.classify(head(0,0),hand(0,.25f,.6f,0))==Slot::salute,"live fist threshold changes actual detection");
    require(h.set_settings(low),"restore active zones");
    require(!h.set_settings(inverted) && h.settings()==low,"invalid live settings keep the active zones");
}
void inhibited_acquisition() {
    using std::chrono::milliseconds;const auto t0=Clock::time_point{std::chrono::seconds(20)};
    const auto view=head(0,0),hip=hand(0,.273f,-.462f,-.064f);VrHolster h;
    h.update(view,hip,0,true,t0,false);
    require(h.update(view,hip,1,true,t0+milliseconds(20),false,false)==Slot::none && h.owns_grip(),"aim/support consumes a hip squeeze without drawing");
    require(h.update(view,hip,1,true,t0+milliseconds(500),false,true)==Slot::none,"releasing trigger/support cannot turn held grip into a delayed draw");
    h.update(view,hip,0,true,t0+milliseconds(520),false);
    require(h.update(view,hip,1,true,t0+milliseconds(540),false)==Slot::secondary,"release and deliberate fresh hip squeeze still draws");
    require(h.update(view,hip,1,true,t0+milliseconds(560),true,false)==Slot::secondary,"trigger/support cannot truncate an existing bounded draw pulse");
    require(h.update(view,hip,1,false,t0+milliseconds(580),true)==Slot::none,"focus loss cancels even committed draws");
    require(h.update(view,hip,1,true,t0+milliseconds(600),false)==Slot::none,"focus recovery requires right grip release");
    const auto up=hand(0,.25f,.45f,0);
    h.update(view,hip,0,true,t0+milliseconds(1000),false);
    require(h.update(view,up,1,true,t0+milliseconds(1020),false,false)==Slot::none,"trigger use inhibits a new raised-fist emote too");
    require(h.update(view,up,1,true,t0+milliseconds(1040),false,true)==Slot::none,"releasing trigger overhead cannot produce a delayed emote");
    h.update(view,hip,0,true,t0+milliseconds(1500),false);
    require(h.update(view,up,1,true,t0+milliseconds(1520),false,true)==Slot::salute,"lowering and releasing restores deliberate salute");
}
}

// Joe, 21 Oct: the sword rests at the hip and is raised by the shoulder. Being in a zone
// only stops a swing starting there; a selection and its draw stop swings until settled.
void swing_gates() {
    using std::chrono::milliseconds;
    const auto t0=Clock::time_point{std::chrono::seconds(20)};
    const auto view=head(0,0),hip=hand(0,.25f,-.7f,0),front=hand(0,.2f,-.4f,.4f);
    VrHolster h;
    const auto step=[&](int ms,const vr_camera::Matrix& at,float grip,std::optional<bool> equipped){h.update(view,at,grip,true,t0+milliseconds(ms),equipped);return t0+milliseconds(ms);};
    auto now=step(0,hip,0,true);
    require(h.blocks_swing() && !h.quiets_swing(now),"resting at the hip with the sword out: no swing starts here, nothing to settle");
    now=step(20,front,0,true);require(!h.blocks_swing() && !h.quiets_swing(now),"out of the zone a swing may start");
    // 31 Oct (Joe): the hip swaps both ways, so a squeeze there with the sidearm out draws the primary;
    // the 21 Oct grip-and-swing out of the hip with the sword out is gone.
    now=step(40,hip,1,true);now=step(60,front,1,true);
    require(h.snapshot().primary==1 && h.quiets_swing(now),"a squeeze at the hip with the sidearm out swaps to the primary, and its draw quiets swings");
    require(!h.blocks_swing(),"out of the zone nothing blocks a swing starting there");
    now=step(700,front,0,true);require(!h.blocks_swing() && !h.quiets_swing(now),"released and past the quiet window, swings start again");
    step(790,front,0,false);now=step(800,hip,0,false);now=step(820,hip,1,false);
    require(h.snapshot().secondary==1 && h.quiets_swing(now),"an actual selection quiets swings");
    now=step(1100,front,1,false);require(h.quiets_swing(now),"while drawing it back with the grip held");
    now=step(1600,front,0,false);require(!h.quiets_swing(now),"released and past the quiet window, swings return");
    now=step(1700,hand(0,.25f,.45f,0),0,true);
    require(h.snapshot().zone==Slot::salute && h.quiets_swing(now),"the overhead emote zone stays a hard gate: lowering the arm from it is not a swing (20 Oct replay, 678.9 s)");
}

// Joe, 21 Oct: the emote fires higher (0.35 m above the eyes; his real raises peaked at
// 0.49-0.72 m) and pumping a raised fist does not repeat it.
void emote_height() {
    using std::chrono::milliseconds;
    const auto t0=Clock::time_point{std::chrono::seconds(30)};
    const auto view=head(0,0),low=hand(0,.25f,-.1f,.3f),beside=hand(0,.25f,.25f,.3f),mid=hand(0,.25f,.15f,.3f),up=hand(0,.25f,.45f,0);
    VrHolster h;
    const auto step=[&](int ms,const vr_camera::Matrix& at,float grip){return h.update(view,at,grip,true,t0+milliseconds(ms),true);};
    step(0,low,0);step(20,low,1);
    require(step(40,beside,1)==Slot::none,"a fist raised beside the head (a sword guard) is not the emote any more");
    require(step(60,up,1)==Slot::salute,"raised 0.45 m above the eyes it is");
    step(600,up,0);step(620,mid,0);step(640,mid,1);
    require(step(660,up,1)==Slot::none,"pumping the fist without lowering it to eye level does not repeat (past the cooldown)");
    step(700,beside,0);step(720,low,0);step(740,low,1);
    require(step(760,up,1)==Slot::salute && h.snapshot().salutes==2,"lowered and raised again, it repeats");
}

// 30 Oct playtest: the thumb on a right button squeezes the Index grip; such a squeeze is never a grab.
void thumb_squeeze() {
    require(VrHolster::thumb_squeeze(true,false,false,0),"right A (dodge) is a thumb squeeze");
    require(VrHolster::thumb_squeeze(false,true,false,0),"right B (interact) is a thumb squeeze");
    require(VrHolster::thumb_squeeze(false,false,true,0),"the stick click (finisher) is a thumb squeeze");
    require(VrHolster::thumb_squeeze(false,false,false,.5f),"a trackpad press (grenade) is a thumb squeeze");
    require(!VrHolster::thumb_squeeze(false,false,false,VrHolster::RELEASE),"a thumb resting on the trackpad is not");
    require(!VrHolster::thumb_squeeze(false,false,false,0),"no thumb button, a real squeeze");
    // The 20:35:56 grab replayed: the grip crossed PRESS while A was down, so it is consumed and
    // the grip must be released before the next squeeze draws.
    using std::chrono::milliseconds;const auto t0=Clock::time_point{std::chrono::seconds(40)};
    VrHolster h;const auto view=head(0,0),hip=hand(0,.13f,-.58f,.26f);
    const auto step=[&](int ms,float grip,bool a){return h.update(view,hip,grip,true,t0+milliseconds(ms),false,!VrHolster::thumb_squeeze(a,false,false,0));};
    step(0,.18f,false);step(30,.21f,true);step(60,.47f,true);
    require(step(95,.68f,true)==Slot::none && h.snapshot().secondary==0,"the recorded dodge squeeze at the hip draws nothing");
    require(step(300,.45f,false)==Slot::none,"lifting the thumb above RELEASE is no draw");
    step(400,.2f,false);
    require(step(600,.9f,false)==Slot::secondary,"a deliberate squeeze afterwards draws the pistol");
}

int main() {
    try {emote_height(); zones();selection();sidearm_selection();settings();inhibited_acquisition();swing_gates();thumb_squeeze();std::cout<<"holster zones, selection pulses and settings passed\n";return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
