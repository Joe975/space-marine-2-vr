// Guard guide: the ghost bar must agree with the guard detector it teaches (VrGuard), and
// its switches, colours and per-eye projection must behave. No GPU; see
// test_eye_texture_capture.cpp for the pixels.
#include "VrGuardGuide.h"
#include "VrNativeHud.h"
#include "VrProjection.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
using State=VrGuardGuide::State;
struct Vec { float x,y,z; };
Vec cross(Vec a,Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec unit(Vec v) { const float l=std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);return {v.x/l,v.y/l,v.z/l}; }
// OpenVR standing space (+Y up, gaze -Z), yaw turns right, pitch looks up.
vr_camera::Matrix head(float yaw,Vec at={0,1.7f,0},float pitch=0) {
    const Vec forward{std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)};
    const Vec z{-forward.x,-forward.y,-forward.z},x=unit(cross({0,1,0},z)),y=cross(z,x);
    return {x.x,x.y,x.z,0, y.x,y.y,y.z,0, z.x,z.y,z.z,0, at.x,at.y,at.z,1};
}
Vec world(float yaw,const vr_body::Offset& o,Vec at) {
    return {at.x+o.lateral*std::cos(yaw)+o.forward*std::sin(yaw),at.y+o.height,at.z+o.lateral*std::sin(yaw)-o.forward*std::cos(yaw)};
}
// A controller at o (head-yaw frame) whose -Z points along d (head-yaw frame, unit).
vr_camera::Matrix hand(float yaw,const vr_body::Offset& o,const vr_body::Offset& d,Vec at={0,1.7f,0}) {
    const Vec p{d.lateral*std::cos(yaw)+d.forward*std::sin(yaw),d.height,d.lateral*std::sin(yaw)-d.forward*std::cos(yaw)};
    const Vec z=unit({-p.x,-p.y,-p.z});
    const Vec helper=std::abs(z.y)>0.9f?Vec{std::sin(yaw),0,-std::cos(yaw)}:Vec{0,1,0};
    const Vec x=unit(cross(helper,z)),y=cross(z,x);const Vec w=world(yaw,o,at);
    return {x.x,x.y,x.z,0, y.x,y.y,y.z,0, z.x,z.y,z.z,0, w.x,w.y,w.z,1};
}
const vr_body::Offset SIDE{-0.8f,-0.3f,-0.2f},DOWN{-1,0,0},FORWARD{0,0,1},RIGHT=VrGuardGuide::AXIS;
const auto T0=Clock::time_point{100s};

struct Random { uint32_t s;float next(float lo,float hi) { s=s*1664525u+1013904223u;return lo+(hi-lo)*float(s>>8)/float(1u<<24); } };
// Valid guard settings anywhere in VrGuard's ranges, as a retune (or the melee agent) might set.
VrGuard::Settings random_settings(Random& r) {
    for(;;) {
        VrGuard::Settings s;
        for(size_t i=0;i<VrGuard::PARAMETERS.size();++i) s.values.values[i]=r.next(VrGuard::PARAMETERS[i].minimum,VrGuard::PARAMETERS[i].maximum);
        if(s.valid()) return s;
    }
}
// A guard that has seen the hand lowered, so it is not latched (VrGuard starts latched).
VrGuard lowered(const VrGuard::Settings& s,const vr_camera::Matrix& H,Clock::time_point& t,float yaw=0,Vec at={0,1.7f,0}) {
    VrGuard g;require(g.set_settings(s),"fixture settings valid");
    g.update(H,hand(yaw,SIDE,DOWN,at),0,true,t);t+=11ms;
    require(!g.snapshot().in_zone,"fixture: the lowered hand is outside the zone");return g;
}
vr_body::Offset direction(Random& r) {
    for(;;) { const vr_body::Offset d{r.next(-1,1),r.next(-1,1),r.next(-1,1)};if(d.height*d.height+d.lateral*d.lateral+d.forward*d.forward>.05f) return d; }
}

// The guide's entry test is VrGuard's: same zone, same entry limit, same angle, for any settings.
void entry_matches_detector() {
    Random r{12345};size_t poses=0,inside=0;
    for(int k=0;k<40;++k) {
        const auto settings=k==0?VrGuard::Settings{}:random_settings(r);
        for(int n=0;n<250;++n) {
            const float yaw=r.next(-3.1f,3.1f);const Vec at{r.next(-1,1),r.next(1.2f,1.9f),r.next(-1,1)};
            const auto H=head(yaw,at,r.next(-.6f,.6f));
            const vr_body::Offset o{r.next(-.7f,.4f),r.next(-.6f,.5f),r.next(-.1f,1.0f)};
            const auto L=hand(yaw,o,direction(r),at);
            auto t=T0;auto g=lowered(settings,head(yaw,at),t,yaw,at);
            g.update(H,L,0,true,t);
            const auto m=VrGuardGuide::measure(H,L);require(m.has_value(),"random poses measure");
            const auto e=VrGuardGuide::entry(settings,*m);const auto snap=g.snapshot();
            require(std::abs(snap.hand.height-m->hand.height)<1e-5f && std::abs(snap.hand.lateral-m->hand.lateral)<1e-5f && std::abs(snap.hand.forward-m->hand.forward)<1e-5f &&
                std::abs(snap.angle_degrees-m->angle_degrees)<1e-3f,"the guide measures the hand exactly as the detector does");
            require(snap.in_zone==e.in_zone,"guide and detector agree on the zone");
            require(snap.pose==e.pose(),"guide and detector agree on a guard start");
            ++poses;inside+=e.pose();
        }
    }
    require(poses==10000 && inside>200 && inside<poses-200,"the agreement sample covers both outcomes");
}

// The drawn target, held pointing along the drawn bar, is a guard: for the defaults, for
// random retunes, at any head position and yaw.
void target_is_a_guard() {
    Random r{777};
    for(int k=0;k<200;++k) {
        const auto settings=k==0?VrGuard::Settings{}:random_settings(r);
        const float yaw=r.next(-3.1f,3.1f);const Vec at{r.next(-2,2),r.next(1.2f,1.9f),r.next(-2,2)};
        const auto H=head(yaw,at);auto t=T0;auto g=lowered(settings,H,t,yaw,at);
        const auto target=VrGuardGuide::target(settings);
        require(g.update(H,hand(yaw,target,RIGHT,at),0,true,t),"a hand at the guide, pointing along its bar, raises the guard");
        const auto drawn=VrGuardGuide::draw(State::active,settings,{});
        require(drawn.has_value(),"active guide draws");
        const auto& grip=drawn->quads[0].corners;
        const float lateral=(grip[0].lateral+grip[2].lateral)/2,height=(grip[0].height+grip[2].height)/2;
        require(std::abs(lateral-target.lateral)<1e-6f && std::abs(height-target.height)<1e-6f && grip[0].forward==target.forward,"the grip marker is drawn on the target");
        const auto& right=drawn->quads[1].corners;const auto& left=drawn->quads[2].corners;
        require(std::abs(right[0].lateral-grip[2].lateral)<1e-6f && right[2].lateral>right[0].lateral && std::abs((right[0].height+right[1].height)/2-target.height)<1e-6f,"the bar runs right from the grip");
        require(std::abs(left[2].lateral-grip[0].lateral)<1e-6f && left[0].lateral<left[2].lateral && std::abs((left[2].lateral-target.lateral)+(right[0].lateral-target.lateral))<1e-6f &&
            std::abs((left[0].lateral-target.lateral)+(right[2].lateral-target.lateral))<1e-6f,"and mirrored to the left: the controller held across, either way");
        VrGuardGuide::Inputs in{true,true,true,true,g.snapshot(),VrGuardGuide::measure(H,hand(yaw,target,RIGHT,at))};
        require(VrGuardGuide::classify(settings,in)==State::active,"and the guide turns active");
        // Pointing forward at the target: right place, wrong angle (when min_angle needs turning).
        auto turned=lowered(settings,H,t,yaw,at);turned.update(H,hand(yaw,target,FORWARD,at),0,true,t);
        in.guard=turned.snapshot();in.left=VrGuardGuide::measure(H,hand(yaw,target,FORWARD,at));
        if(settings[VrGuard::MIN_ANGLE]>1) require(VrGuardGuide::classify(settings,in)==State::turn,"in the zone but pointing forward is 'turn'");
    }
}

void states_and_switches() {
    const VrGuard::Settings s;const auto H=head(0);const auto target=VrGuardGuide::target(s);
    auto t=T0;auto g=lowered(s,H,t);
    const auto at_side=VrGuardGuide::measure(H,hand(0,SIDE,DOWN));
    VrGuardGuide::Inputs in{true,true,true,true,g.snapshot(),at_side};
    require(VrGuardGuide::classify(s,in)==State::idle,"sword out, hand at the side: idle guide");
    for(int gate=0;gate<4;++gate) {
        auto off=in;(gate==0?off.guide_enabled:gate==1?off.guard_enabled:gate==2?off.sword:off.allowed)=false;
        require(VrGuardGuide::classify(s,off)==State::hidden,"guide, guard, sword and input ownership each hide the guide");
    }
    in.left.reset();require(VrGuardGuide::classify(s,in)==State::idle,"no fresh left hand: idle, not a guess");
    // In the pose but the detector refuses: trigger pulled in the zone (aim latch).
    g.update(H,hand(0,target,RIGHT),0.5f,true,t);t+=11ms;
    require(!g.snapshot().held && g.snapshot().aim_latched,"fixture: aim latch holds the guard off");
    in.guard=g.snapshot();in.left=VrGuardGuide::measure(H,hand(0,target,RIGHT));
    require(VrGuardGuide::classify(s,in)==State::blocked,"pose right but no guard: blocked");
    // Both hands on the sword: VrSession passes allowed=false to the guard, so it never holds.
    auto two=lowered(s,H,t);two.update(H,hand(0,target,RIGHT),0,false,t);
    in.guard=two.snapshot();require(VrGuardGuide::classify(s,in)==State::blocked,"two-handed hold in the pose: blocked");
    // Held drifting out after the start (exit margin): still the detector's held, so active.
    auto held=lowered(s,H,t);held.update(H,hand(0,target,RIGHT),0,true,t);t+=11ms;
    const vr_body::Offset drift{target.height,target.lateral,s[VrGuard::MAX_FORWARD]};
    require(held.update(H,hand(0,drift,RIGHT),0,true,t),"fixture: a held guard drifts out");
    in.guard=held.snapshot();in.left=VrGuardGuide::measure(H,hand(0,drift,RIGHT));
    require(VrGuardGuide::classify(s,in)==State::active,"a held guard stays active wherever the detector keeps it");

    require(std::atan2(-target.height,target.forward)<30*3.14159265f/180 && target.height<0,"the default target is below the eyes but in view (under 30 degrees down)");
    const VrGuardGuide::Settings look;
    require(!VrGuardGuide::draw(State::hidden,s,look),"hidden draws nothing");
    auto off=look;off.values.values[VrGuardGuide::ENABLED]=0;
    require(!VrGuardGuide::draw(State::active,s,off),"config enabled=0 draws nothing");
    std::array<std::array<float,4>,4> colours{};
    for(size_t i=1;i<VrGuardGuide::STATES;++i) colours[i-1]=VrGuardGuide::draw(State(i),s,look)->colour;
    for(size_t a=0;a<4;++a) for(size_t b=a+1;b<4;++b) require(colours[a]!=colours[b],"each visible state has its own colour");
    require(colours[0][3]<colours[3][3] && colours[3][3]<=1,"idle is faintest, active brightest");
    auto longer=look;longer.values.values[VrGuardGuide::LENGTH]=.4f;
    require(VrGuardGuide::draw(State::idle,s,longer)->quads[1].corners[2].lateral>VrGuardGuide::draw(State::idle,s,look)->quads[1].corners[2].lateral,"bar length knob changes the drawn bar");
    auto tuned=s;tuned.values.values[VrGuard::MAX_ENTRY_FORWARD]=.4f;
    require(VrGuardGuide::draw(State::idle,tuned,look)->quads[0].corners[0].forward>VrGuardGuide::draw(State::idle,s,look)->quads[0].corners[0].forward,"a retuned guard zone moves the guide");
}

void tracker() {
    const VrGuard::Settings s;const auto H=head(0);auto t=T0;auto g=lowered(s,H,t);
    VrGuardGuide::Tracker tr;
    const VrGuardGuide::Inputs in{false,true,true,true,g.snapshot(),VrGuardGuide::measure(H,hand(0,SIDE,DOWN))};
    require(tr.update(s,in,t)==State::hidden && !tr.current(t),"disabled by default (launcher flag off)");
    tr.enable(true);
    require(tr.update(s,in,t)==State::idle && tr.current(t) && tr.current(t)->state==State::idle,"enabled: idle guide while the sword is out");
    require(tr.current(t+150ms) && !tr.current(t+151ms) && !tr.current(t-1ms),"a guide not refreshed by a poll for 150 ms disappears");
    auto noguide=in;noguide.sword=false;
    require(tr.update(s,noguide,t)==State::hidden && !tr.current(t),"no sword, no guide");
    auto look=tr.settings();look.values.values[VrGuardGuide::ENABLED]=0;require(tr.set_settings(look),"config switch accepted");
    require(tr.update(s,in,t)==State::hidden,"config enabled=0 hides it with the launcher flag on");
    look.values.values[VrGuardGuide::ENABLED]=.5f;require(!tr.set_settings(look),"enabled must be 0 or 1");
    const auto st=tr.status();require(st.polls[0]==3 && st.polls[1]==1 && st.changes==2 && st.state==State::hidden,"status counts polls per state and changes");
}

void settings_file(const char* repo_config) {
    std::stringstream text;const VrGuardGuide::Settings defaults;require(defaults.write(text),"defaults write");
    const auto back=VrGuardGuide::Settings::read(text);require(back && *back==defaults,"settings round-trip");
    std::stringstream partial{"enabled=1\n"};require(!VrGuardGuide::Settings::read(partial),"incomplete settings are rejected");
    std::stringstream wild{"enabled=1\nbar_length_metres=0.24\nbar_thickness_metres=0.018\ngrip_metres=0.05\nopacity=3\n"};require(!VrGuardGuide::Settings::read(wild),"out-of-range opacity is rejected");
    const auto shipped=VrGuardGuide::Settings::load(repo_config);
    require(shipped && *shipped==defaults && shipped->enabled(),"config/index_guard_guide.calibration ships the defaults, enabled");
    require(!defaults.save("relative.calibration"),"saves need an absolute path");
}

// world() is locate()'s inverse; clip() lands where the HUD plane maths (plane_transform)
// puts the same head-relative point, and clips behind the eye.
void projection() {
    Random r{99};
    for(int n=0;n<200;++n) {
        const float yaw=r.next(-3.1f,3.1f);const auto H=head(yaw,{r.next(-1,1),r.next(1,2),r.next(-1,1)},r.next(-.5f,.5f));
        const vr_body::Offset o{r.next(-.5f,.5f),r.next(-.5f,.5f),r.next(-.5f,.5f)};
        auto L=vr_camera::IDENTITY;const auto w=VrGuardGuide::world(H,o);require(w.has_value(),"world point");
        L[12]=(*w)[0];L[13]=(*w)[1];L[14]=(*w)[2];
        const auto back=vr_body::locate(H,L);
        require(back && std::abs(back->height-o.height)<1e-5f && std::abs(back->lateral-o.lateral)<1e-5f && std::abs(back->forward-o.forward)<1e-5f,"world() inverts locate()");
    }
    vr_camera::Matrix P{};require(vr_projection::build({-1.2f,.9f,-1.1f,1},.1f,1000,P),"asymmetric projection");
    auto eye=vr_camera::IDENTITY;eye[12]=-.032f;eye[13]=.004f;
    const auto H=head(0,{.3f,1.6f,-.2f});
    for(const auto& [lateral,height,forward]:std::array<std::array<float,3>,3>{{{-.1f,-.175f,.225f},{.2f,.1f,.6f},{-.3f,-.4f,.3f}}}) {
        const float size=.04f;
        const auto q=VrGuardGuide::rectangle(lateral-size/2,lateral+size/2,height-size/2,height+size/2,forward);
        const auto c=VrGuardGuide::clip(q,H,eye,P);require(c.has_value(),"clip corners");
        float x=0,y=0;for(int k=0;k<4;++k){x+=(*c)[k*4]/(*c)[k*4+3]/4;y+=(*c)[k*4+1]/(*c)[k*4+3]/4;}
        const auto plane=VrNativeHud::plane_transform(size,size,lateral,height,forward,P,eye);require(plane.has_value(),"plane transform");
        const float u=(.5f-(*plane)[2])/(*plane)[0],v=(.5f-(*plane)[3])/(*plane)[1];
        require(std::abs(x-(2*u-1))<1e-3f && std::abs(y-(1-2*v))<1e-3f,"guide quads project where the HUD plane maths puts the same point");
        for(int k=0;k<4;++k) require((*c)[k*4+2]>0 && (*c)[k*4+2]<(*c)[k*4+3],"in front of the eye: inside the depth clip range");
    }
    const auto behind=VrGuardGuide::clip(VrGuardGuide::rectangle(-.1f,.1f,-.1f,.1f,-.3f),H,eye,P);
    require(behind && (*behind)[2]<0,"behind the eye: the rasterizer clips it");
    {   // Snapshot evidence: the drawn state and the grip marker's pixel, from the same projection.
        const VrGuard::Settings g;const auto d=VrGuardGuide::draw(State::turn,g,{});
        require(VrGuardGuide::json(std::nullopt,H,eye,P,100,100)==",\"guard_guide\":\"hidden\"","no guide: hidden, no pixel");
        const auto text=VrGuardGuide::json(d,H,eye,P,1000,800);const auto px=VrGuardGuide::pixel(*d,H,eye,P,1000,800);
        require(px && text.starts_with(",\"guard_guide\":\"turn\",\"guard_guide_pixel\":[") && (*px)[1]>400 && (*px)[1]<800 && (*px)[0]>0 && (*px)[0]<1000,"drawn guide: state and an in-image pixel below the centre");
    }
    auto bad=H;bad[0]=2;require(!VrGuardGuide::clip(VrGuardGuide::rectangle(0,.1f,0,.1f,.3f),bad,eye,P),"non-rigid head refuses");
}
} // namespace

int main(int argc,char** argv) {
    try {
        if(argc<2) throw std::runtime_error("usage: vr_guard_guide_tests CONFIG/index_guard_guide.calibration");
        entry_matches_detector();target_is_a_guard();states_and_switches();tracker();settings_file(argv[1]);projection();
        std::cout<<"guard guide: detector agreement, target, states, switches, tracker, settings and projection passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
