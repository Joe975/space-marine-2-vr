#include "VrGazeCamera.h"
#include "SceneMode.h"
#include "VrControllerInput.h"
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
bool close_to(float a,float b,float tolerance=.05f) { return std::abs(a-b)<=tolerance; }
using vr_gaze::Matrix;using vr_gaze::Vector;
constexpr float RAD=0.0174532925f;
// A native camera (rows right, up, forward) turned by yaw about +Y and pitched, in a
// right-handed Y-up world like the game's (title camera: right +X, forward -Z).
Matrix camera(float yaw_degrees,float pitch_degrees,bool mirrored=false) {
    const float y=yaw_degrees*RAD,p=pitch_degrees*RAD;
    const Vector forward{-std::sin(y)*std::cos(p),std::sin(p),-std::cos(y)*std::cos(p)};
    Vector right{std::cos(y),0,-std::sin(y)};
    if(mirrored) for(auto& v:right) v=-v;
    return {right[0],right[1],right[2],0, 0,1,0,0, forward[0],forward[1],forward[2],0, 0,0,0,1};
}
Vector gaze(float yaw_degrees,float pitch_degrees) { const auto m=camera(yaw_degrees,pitch_degrees);return {m[8],m[9],m[10]}; }
}

int main() { try {
    // Alignment: the camera's own right axis decides the sign, so handedness is never assumed.
    const auto title=camera(0,-6.5f);
    auto a=vr_gaze::alignment(title,Vector{1,0,0});
    require(a && close_to(a->yaw_degrees,90) && close_to(a->pitch_degrees,6.5f),"gaze along the native camera's right axis is +90 degrees and above its pitch");
    a=vr_gaze::alignment(title,Vector{-1,0,0});
    require(a && close_to(a->yaw_degrees,-90),"gaze to the camera's left is negative");
    a=vr_gaze::alignment(camera(0,0,true),Vector{1,0,0});
    require(a && close_to(a->yaw_degrees,-90),"a camera whose right axis points the other way reverses the sign");
    // Session numbers (docs/INTERACTION.md): native camera at -60 degrees heading, pitch -6.5;
    // Joe looking 130 degrees away and 30 degrees down at the elevator lever.
    a=vr_gaze::alignment(camera(-60,-6.5f),gaze(-60-130,-30));
    require(a && close_to(std::abs(a->yaw_degrees),130,.1f) && close_to(a->pitch_degrees,-23.5f,.1f),"large session misalignment is measured exactly");
    a=vr_gaze::alignment(camera(170,-7),gaze(-170,-7));
    require(a && close_to(std::abs(a->yaw_degrees),20,.1f) && close_to(a->pitch_degrees,0,.1f),"headings wrap across +/-180 degrees");
    require(!vr_gaze::alignment(camera(0,-6.5f),Vector{0,-1,0}),"gaze straight down has no heading");
    require(!vr_gaze::alignment(camera(0,90),Vector{0,0,-1}),"camera straight up has no heading");
    require(!vr_gaze::alignment(title,Vector{0,std::numeric_limits<float>::quiet_NaN(),-1}),"non-finite gaze is refused");
    require(!vr_gaze::alignment(title,Vector{0,0,0}),"zero gaze is refused");

    // Steering law against the measured native response (2026-10-21): a requested turn rate
    // proportional to the error, inverted through the measured curve; full stick beyond
    // boost_degrees; deadband 2 with a 5 degree re-engage band.
    using vr_gaze::Steering;
    Steering::Settings s;require(s.valid(),"defaults valid");
    for(const auto* curve:{&Steering::YAW_RESPONSE,&Steering::PITCH_RESPONSE})
        for(size_t i=1;i<curve->size();++i) require((*curve)[i].stick>(*curve)[i-1].stick && (*curve)[i].rate>=(*curve)[i-1].rate,"measured curves are monotonic");
    require(close_to(Steering::stick_for(Steering::YAW_RESPONSE,15.1f),.60f,1e-4f) && close_to(Steering::stick_for(Steering::YAW_RESPONSE,9),.5265f,.001f),
        "inversion is exact on the table and linear between points");
    require(Steering::stick_for(Steering::YAW_RESPONSE,500)==.80f && Steering::stick_for(Steering::YAW_RESPONSE,0)==.30f,"inversion stays below the edge boost and starts at the native deadzone");
    std::array<bool,2> hold{};
    auto stick=Steering::steer({4,0},s,hold);
    require(close_to(stick.x,Steering::stick_for(Steering::YAW_RESPONSE,6*3),.001f) && stick.y==0,"4 degrees asks for 18 deg/s: about 0.64, far above the 0.3-0.4 dead zone");
    require(stick.x>.6f,"near the target the stick still turns the native camera at a useful rate (the first law sent 0.3-0.4)");
    stick=Steering::steer({1.5f,-1},s,hold);
    require(stick.x==0 && stick.y==0 && hold[0] && hold[1],"inside the deadband an axis stops and holds");
    stick=Steering::steer({4.5f,-4},s,hold);
    require(stick.x==0 && stick.y==0,"a small drift after settling does not restart (no hunting)");
    stick=Steering::steer({6,-4},s,hold);
    require(stick.x>.6f && stick.y==0 && !hold[0] && hold[1],"beyond re-engage only that axis restarts");
    hold={};stick=Steering::steer({-12,8},s,hold);
    require(close_to(stick.x,-Steering::stick_for(Steering::YAW_RESPONSE,66),.001f) && close_to(stick.y,Steering::stick_for(Steering::PITCH_RESPONSE,42),.001f),
        "error sign sets direction; pitch uses its own (slower) curve");
    stick=Steering::steer({16,-16},s,hold);
    require(stick.x==1 && stick.y==-1,"beyond boost_degrees: full stick (the game's edge boost, about 120 deg/s)");
    auto slower=s;slower.values.values[Steering::RATE_SCALE]=2;
    {std::array<bool,2> h1{},h2{};require(Steering::steer({4,0},slower,h1).x<Steering::steer({4,0},s,h2).x,"a faster native look (rate_scale 2) asks for less stick");}
    auto inverted=s;inverted.values.values[Steering::PITCH_SIGN]=-1;hold={};
    require(inverted.valid() && Steering::steer({0,30},inverted,hold).y<0,"inverted-Y look reverses pitch only");
    {std::array<bool,2> h{};const auto nan=Steering::steer({std::numeric_limits<float>::quiet_NaN(),3},s,h);require(nan.x==0 && !h[0],"a non-finite error never moves the stick");}
    auto bad=s;bad.values.values[Steering::PITCH_SIGN]=.5f;require(!bad.valid(),"pitch_sign must be exactly +/-1");
    bad=s;bad.values.values[Steering::REENGAGE]=1;require(!bad.valid(),"re-engage below the deadband rejected");
    bad=s;bad.values.values[Steering::BOOST]=5;bad.values.values[Steering::REENGAGE]=6;require(!bad.valid(),"boost inside the re-engage band rejected");
    {
        std::istringstream good{"response_per_second=5\ndeadband_degrees=2\nreengage_degrees=5\nboost_degrees=20\nmax_stick=1\nrate_scale=1\npitch_sign=1\n"};
        const auto loaded=Steering::Settings::read(good);
        require(loaded && close_to(loaded->values.values[Steering::RESPONSE],5,1e-6f),"tuning file reads");
        std::istringstream old{"yaw_stick_per_degree=0.02\npitch_stick_per_degree=0.02\ndeadband_degrees=3\nreengage_degrees=6\nmin_stick=0.3\nmax_stick=1\npitch_sign=1\n"};
        require(!Steering::Settings::read(old),"a first-version tuning file is refused (defaults apply)");
        const auto path=std::filesystem::temp_directory_path()/("sm2vr-gaze-"+std::to_string(GetCurrentProcessId())+".calibration");
        require(loaded->save(path) && Steering::Settings::load(path)==loaded,"tuning file round trips");std::filesystem::remove(path);
    }
    {   // Log line: never "nan"; stale and missing alignments are named; sent comes from the pad.
        const auto none=vr_gaze::status_line(7,std::nullopt,std::nullopt,std::nullopt,0,0);
        const auto stale=vr_gaze::status_line(7,std::nullopt,1234.4,std::nullopt,0,0);
        const auto live=vr_gaze::status_line(9,vr_gaze::Alignment{-3.04f,2.5f},5.0,Steering::Stick{.7f,0},0,16384);
        require(none.find("alignment=none")!=std::string::npos && stale.find("alignment=stale age_ms=1234")!=std::string::npos,"missing/stale alignment is named");
        require(live.find("yaw_error=-3.0 pitch_error=2.5")!=std::string::npos && live.find("computed=[0.70,0.00] sent=[0.00,0.50]")!=std::string::npos,"computed and sent sticks are reported apart");
        for(const auto* text:{&none,&stale,&live}) require(text->find("nan")==std::string::npos,"no nan in the log line");
    }

    // Gating: disabled, disallowed, stale or future alignments steer nothing.
    using namespace std::chrono_literals;
    const auto t0=Steering::Clock::time_point{100s};
    Steering steering;
    steering.observe(camera(0,-6.5f),gaze(-90,-6.5f),t0);
    require(!steering.stick(true,t0+10ms),"the candidate is off unless enabled");
    steering.enable(true);steering.observe(camera(0,-6.5f),gaze(-90,-6.5f),t0);
    require(!steering.stick(false,t0+10ms) && !steering.last(),"menus, viewer, cinematics or other views do not steer");
    const auto right=steering.stick(true,t0+10ms);
    require(right && right->x>.9f && right->y==0,"gaze 90 degrees to the right turns the native camera right");
    require(!steering.stick(true,t0+150ms),"an alignment older than 100 ms steers nothing (camera stops)");
    require(!steering.stick(true,t0-1ms),"a future-dated alignment is refused");
    steering.observe(camera(0,90),gaze(0,0),t0);require(!steering.stick(true,t0+1ms),"an undefined alignment clears the previous one");
    steering.enable(false);steering.observe(camera(0,0),gaze(90,0),t0);require(!steering.latest(t0),"disabling drops alignment");
    {   // The hangar, 31 Oct 18:44:17: mask 0x800, the game camera still at its spawn heading of 23.5 and pitch
        // -7.1, Joe facing the battle simulator 100.9 degrees to its left and 21.3 below. No prompt: the marker
        // was off the game camera's frustum. The stick now turns it round, as it does at 0x0 in the same hangar.
        const auto scene=[](uint32_t mask,bool menu=false){return scene_mode::classify({true,mask,mask,false,menu});};
        Steering hangar;hangar.enable(true);hangar.observe(camera(23.5f,-7.1f),gaze(23.5f+100.9f,-7.1f-21.3f),t0);
        const auto look=hangar.stick(scene(0x800).gaze_camera(),t0+1ms);
        require(look && look->x==-1 && look->y==-1,"the hub steers: full stick left and down");
        require(!hangar.stick(scene(0xc00).gaze_camera(),t0+2ms),"the hub paused coming out of a load does not");
        require(!hangar.stick(scene(0x800,true).gaze_camera(),t0+3ms),"mission select over the hub does not");
    }
    {   // Class ability held (native Y, precise): the camera closes to the deadband instead of
        // parking inside the re-engage band, so a grapnel or auspex aims at the gaze centre.
        Steering held;held.enable(true);
        held.observe(camera(0,0),gaze(-1.5f,0),t0);require(held.stick(true,t0+1ms)->x==0,"settled inside the deadband");
        held.observe(camera(0,0),gaze(-4.5f,-4.5f),t0+20ms);
        require(held.stick(true,t0+21ms)->x==0,"plain play: a 4.5 degree drift stays parked (no hunting)");
        const auto aim=held.stick(true,t0+22ms,true);
        require(aim && aim->x>.6f && aim->y<-.5f,"ability held: the same drift is closed, both axes");
        held.observe(camera(0,0),gaze(-1.8f,1),t0+40ms);
        require(held.stick(true,t0+41ms,true)->x==0 && held.stick(true,t0+42ms,true)->y==0,"ability held: it still stops inside the deadband");
        held.observe(camera(0,0),gaze(-3,0),t0+60ms);
        require(held.stick(true,t0+61ms)->x==0 && held.stick(true,t0+62ms,true)->x>.5f,"released: back to parking; held again: closes");
        require(held.settings().values.values[Steering::REENGAGE]==5,"the tuning itself is unchanged");
    }

    // Closed loop against the measured native response: the yaw/pitch curves, an edge boost
    // from 0.85 to 100 or 140 deg/s yaw (55% for pitch) that is instant or ramps over 0.5 s,
    // 30 Hz polls and 1-3 polls of latency. Each look settles within 3 degrees inside
    // 0.6 s + yaw / 80 or pitch / 45 deg/s (the check_interaction_alignment.py --follow limit), never
    // overshoots past the re-engage band, and afterwards stays inside it with the stick at rest.
    {
        const auto curve=[](const auto& table,float a){
            if(a<=table.front().stick) return 0.f;
            for(size_t i=1;i<table.size();++i) if(a<=table[i].stick) {const auto& p=table[i-1];const auto& q=table[i];return p.rate+(q.rate-p.rate)*(a-p.stick)/(q.stick-p.stick);}
            return table.back().rate;};
        struct Look { float yaw,pitch; };bool all_ok=true;
        for(const float boost_rate:{100.f,140.f}) for(const float ramp:{0.f,.5f}) for(const int latency:{1,2,3})
        for(const Look look:{Look{179,0},Look{130,-23.5f},Look{90,0},Look{-35,-35},Look{12,5}}) {
            Steering loop;loop.enable(true);
            float yaw=0,pitch=0;std::array<float,2> boosted{};
            const float target_yaw=-look.yaw,target_pitch=look.pitch; // Gaze to the right: heading decreases.
            auto t=t0;float worst_overshoot=0,worst_after=0;std::optional<int> settled_frame;bool rested=true;
            std::vector<Steering::Stick> pipeline(size_t(latency),Steering::Stick{});
            const float limit=.6f+std::max(std::abs(look.yaw)/80,std::abs(look.pitch)/45); // Pitch turns at about 55% of yaw.
            for(int frame=0;frame<150;++frame,t+=33ms) {
                const auto applied=pipeline.front();pipeline.erase(pipeline.begin());
                const auto rate=[&](float stick,const auto& table,float top,float& b){const float a=std::abs(stick);
                    if(a>=.85f){b=ramp>0?std::min(1.f,b+.033f/ramp):1.f;return std::copysign(table.back().rate+(top-table.back().rate)*b,stick);}
                    b=0;return std::copysign(curve(table,a),stick);};
                yaw-=rate(applied.x,Steering::YAW_RESPONSE,boost_rate,boosted[0])*.033f;
                pitch+=rate(applied.y,Steering::PITCH_RESPONSE,.55f*boost_rate,boosted[1])*.033f;
                loop.observe(camera(yaw,pitch),gaze(target_yaw,target_pitch),t);
                const auto now=loop.latest(t);require(now.has_value(),"loop alignment available");
                // Overshoot: the error changes sign and grows on the far side.
                worst_overshoot=std::max({worst_overshoot,look.yaw>0?-now->yaw_degrees:now->yaw_degrees,look.pitch<0?now->pitch_degrees:look.pitch>0?-now->pitch_degrees:0.f});
                const auto sent=loop.stick(true,t).value_or(Steering::Stick{});pipeline.push_back(sent);
                const float error=std::max(std::abs(now->yaw_degrees),std::abs(now->pitch_degrees));
                if(error<=3 && !settled_frame) settled_frame=frame;
                if(settled_frame && frame>*settled_frame+8) {worst_after=std::max(worst_after,error);rested=rested && sent.x==0 && sent.y==0;}
            }
            if(!(settled_frame && *settled_frame*.033f<=limit) || worst_overshoot>5 || worst_after>5 || !rested)
                std::cerr<<"look "<<look.yaw<<","<<look.pitch<<" boost "<<boost_rate<<" ramp "<<ramp<<" latency "<<latency<<": settle frame "<<(settled_frame?*settled_frame:-1)
                    <<" overshoot "<<worst_overshoot<<" after "<<worst_after<<" rested "<<rested<<'\n';
            all_ok=all_ok && settled_frame && *settled_frame*.033f<=limit && worst_overshoot<=5 && worst_after<=5 && rested;
        }
        require(all_ok,"every look settles inside the --follow limit, never overshoots past the re-engage band, then rests (failures listed above)");
    }

    // The look reaches the native right stick only on armed gameplay pads.
    {
        VrControllerInput input;input.configure(true,true,false);
        const auto now=std::chrono::steady_clock::now();
        input.update(VrControllerSample{},0,true,now,std::array<float,2>{.5f,-.25f});
        require(input.snapshot(now).pad.Gamepad.sThumbRX==0,"disarmed (awaiting neutral) pads carry no look");
        input.update(VrControllerSample{},0,true,now,std::array<float,2>{.5f,-.25f});
        const auto pad=input.snapshot(now).pad.Gamepad;
        require(input.snapshot(now).armed && pad.sThumbRX==16384 && pad.sThumbRY==-8192,"armed gameplay pad carries the steering deflection");
        input.update(VrControllerSample{},0,true,now);
        require(input.snapshot(now).pad.Gamepad.sThumbRX==0,"no look, right stick centred");
        input.update(VrControllerSample{},0,true,now,std::array<float,2>{std::numeric_limits<float>::infinity(),0});
        require(input.snapshot(now).pad.Gamepad.sThumbRX==0,"non-finite look is ignored");
    }
    std::cout<<"gaze alignment, steering law, gating, closed loop and right-stick routing PASS\n";return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
