#include "VrSprintCamera.h"
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok,const std::string& message) { if(!ok) throw std::runtime_error(message); }
using Clock=VrSprintCamera::Clock;
const Clock::time_point T0{std::chrono::seconds(100)};
Clock::time_point at(int ms) { return T0+std::chrono::milliseconds(ms); }
// Speeds from the scripted driver (docs/SPRINT_CAMERA.md): native sprint 8.36-8.74 m/s,
// full-stick running 5.78-5.81 m/s.
constexpr float RUN=5.8f,SPRINT=8.6f;
// The smoothstep ramp's steepest slope is 1.5/blend; 0.20 m over 0.25 s moves at most
// 0.9 mm per millisecond. Anything larger between two 1 ms samples is a snap.
constexpr float FULL=0.20f,HALF=FULL/2,NEAR_FULL=FULL-0.001f;
constexpr float MAX_STEP_PER_MS=FULL*1.5f/0.25f/1000+1e-6f;

// Polls at `poll_ms` intervals over [from,to) and samples the eye offset every millisecond in
// between (eyes are composed at render rate, not poll rate). Fails on any discontinuity.
struct Run {
    VrSprintCamera camera;
    float last{};int last_ms{-1};std::map<int,float> seen;
    void poll(int ms,bool requested,std::optional<float> speed) { sample(ms);camera.update(requested,speed,at(ms));sample(ms); }
    void sample(int ms) {
        const float value=camera.offset_metres(at(ms));
        require(std::isfinite(value) && value>=0 && value<=camera.settings().offset_metres+1e-6f,"offset stays within [0, configured]");
        if(last_ms>=0 && ms>=last_ms) require(std::abs(value-last)<=MAX_STEP_PER_MS*std::max(1,ms-last_ms),"offset is continuous at "+std::to_string(ms)+" ms");
        last=value;last_ms=ms;seen[ms]=value;
    }
    // What an eye composed at `ms` received (queried later, the ramp has moved on).
    float seen_at(int ms) const { const auto found=seen.find(ms); require(found!=seen.end(),"no eye sampled at "+std::to_string(ms)+" ms"); return found->second; }
    void hold(int from,int to,bool requested,std::optional<float> speed,int poll_ms=11) {
        for(int ms=from;ms<to;++ms) { if((ms-from)%poll_ms==0) poll(ms,requested,speed);else sample(ms); }
    }
};

void defaults() {
    const VrSprintCamera::Settings settings;
    require(settings.valid() && settings.offset_metres==FULL && settings.blend_seconds==0.25f,"defaults: 0.20 m over 0.25 s (Joe, 21 Oct)");
    VrSprintCamera camera;
    for(int ms=0;ms<2000;ms+=50) { camera.update(false,RUN,at(ms));require(camera.offset_metres(at(ms))==0,"no sprint request, no offset (running is not sprinting)"); }
    require(camera.snapshot(at(2000)).engagements==0,"never engaged");
}

void ramp_in_hold_and_ramp_out() {
    Run run;
    run.hold(0,1000,false,RUN);
    require(run.seen_at(999)==0,"walking/running before the press has no offset");
    // Press: the game is still accelerating through running speed for ~0.15 s.
    run.poll(1000,true,RUN);
    require(run.camera.snapshot(at(1000)).active && !run.camera.snapshot(at(1000)).confirmed,"ramp starts at the press, inside the onset grace");
    require(run.seen_at(1000)==0,"ramp starts from zero, no snap");
    run.hold(1001,1150,true,RUN);
    const float half=run.seen_at(1125); // Between polls (1122, 1133), as an eye would be.
    require(std::abs(half-HALF)<1e-4f,"halfway through the blend the smoothstep gives half the offset");
    run.hold(1150,3000,true,SPRINT);
    require(run.camera.snapshot(at(2999)).confirmed,"native sprint speed confirms");
    require(std::abs(run.seen_at(1250)-FULL)<1e-6f && std::abs(run.seen_at(2999)-FULL)<1e-6f,"full 0.20 m after 0.25 s and while sprinting");
    // Release: hold through the native stop lean (0.7 s), then ramp out over 0.25 s.
    run.hold(3000,3600,false,RUN);
    require(std::abs(run.seen_at(3599)-FULL)<1e-6f,"offset holds through the native stop animation");
    run.hold(3600,4300,false,0.f);
    require(run.seen_at(3700)>NEAR_FULL,"still held at 0.7 s");
    require(std::abs(run.seen_at(3700+125)-HALF)<1e-4f,"ramping out after the hold");
    require(run.seen_at(4299)==0,"fully out 0.25 s after the hold ends");
    require(run.camera.snapshot(at(4299)).engagements==1,"one engagement for one sprint");
}

void poll_rate_independent() {
    // The same sprint polled at 90 Hz and at 20 Hz gives the same eye offset at any instant.
    const auto script=[](int poll_ms) {
        Run run;run.hold(0,1500,true,SPRINT,poll_ms);run.hold(1500,2600,false,RUN,poll_ms);return run;
    };
    auto fast=script(11),slow=script(50);
    for(int ms:{1500,2100,2200,2250,2300,2400}) require(std::abs(fast.seen_at(ms)-slow.seen_at(ms))<1e-5f,
        "hold expiry and ramp-out do not depend on the poll rate ("+std::to_string(ms)+" ms)");
    require(std::abs(fast.seen_at(2200+125)-HALF)<1e-4f,"ramp-out starts exactly 0.7 s after release");
}

void refused_sprint_retracts() {
    // The mod asked for sprint but the game never reached sprint speed (blocked, slope, takeover).
    Run run;
    run.hold(0,2000,true,RUN);
    const auto state=run.camera.snapshot(at(1999));
    require(!state.active && !state.confirmed && state.refusals==1,"no native confirmation after the grace: refused once");
    require(run.seen_at(1999)==0,"refused sprint retracts to no offset");
    run.hold(2000,3000,false,RUN);
    require(run.seen_at(2999)==0,"no hold after a refused sprint");
}

void short_unconfirmed_press_has_no_hold() {
    Run run;
    run.hold(0,100,true,RUN);
    const float at_release=run.seen_at(99);
    require(at_release>0,"ramp began");
    run.hold(100,500,false,RUN);
    require(run.seen_at(100+260)==0,"unconfirmed tap ramps straight out (no 0.7 s hold)");
}

void hysteresis_and_wall() {
    Run run;
    run.hold(0,1000,true,SPRINT);
    run.hold(1000,1500,true,6.5f);
    require(run.camera.snapshot(at(1499)).active && run.seen_at(1499)>NEAR_FULL,"speed dips between release (6.2) and confirm (7.0) keep the offset");
    run.hold(1500,2000,true,2.0f);
    require(!run.camera.snapshot(at(1999)).active && run.seen_at(1999)==0,"stopped against a wall while still requesting: offset ramps out");
    run.hold(2000,2500,true,SPRINT);
    require(run.camera.snapshot(at(2499)).active && run.seen_at(2499)>NEAR_FULL,"sprint speed again re-engages");
}

void unobserved_native_uses_mod_state() {
    Run run;
    run.hold(0,2000,true,std::nullopt);
    const auto state=run.camera.snapshot(at(1999));
    require(state.active && !state.native_known && run.seen_at(1999)>NEAR_FULL,"without a native body the mod's sprint state decides");
    run.hold(2000,2500,false,std::nullopt);
    require(run.seen_at(2499)>NEAR_FULL,"and still holds through the stop");
    VrSprintCamera nan;nan.update(true,std::nanf(""),at(0));
    require(!nan.snapshot(at(0)).native_known,"a non-finite speed is unobserved, not a refusal");
}

void reversal_mid_ramp_is_continuous() {
    Run run;
    run.hold(0,100,true,RUN);        // Grace ramp-in.
    run.hold(100,180,false,RUN);     // Unconfirmed: ramping out.
    run.hold(180,600,true,SPRINT);   // Pressed again: back up from the current value.
    require(run.seen_at(599)>NEAR_FULL,"re-press reaches full offset");
}

void settings_changes() {
    VrSprintCamera camera;
    VrSprintCamera::Settings off;off.offset_metres=0;
    require(camera.set_settings(off,at(0)),"0 m is valid and disables");
    for(int ms=0;ms<2000;ms+=10) { camera.update(true,SPRINT,at(ms));require(camera.offset_metres(at(ms))==0,"disabled offset is never applied"); }
    VrSprintCamera live;
    live.update(true,RUN,at(0));
    const float before=live.offset_metres(at(100));
    VrSprintCamera::Settings slow;slow.blend_seconds=1;
    require(live.set_settings(slow,at(100)) && std::abs(live.offset_metres(at(100))-before)<1e-6f,"a new blend time mid-ramp does not jump the offset");
    for(auto broken:{VrSprintCamera::Settings{-0.1f},VrSprintCamera::Settings{0.5f},VrSprintCamera::Settings{0.15f,0},VrSprintCamera::Settings{0.15f,0.25f,6,7},
        VrSprintCamera::Settings{std::nanf("")}})
        require(!live.set_settings(broken,at(100)) && live.settings()==slow,"invalid settings are refused and keep the active ones");
    live.reset(at(200));
    require(live.offset_metres(at(200))==0 && !live.snapshot(at(200)).active,"reset drops the offset (recenter is already discontinuous)");
}
} // namespace

int main() {
    try { defaults();ramp_in_hold_and_ramp_out();poll_rate_independent();refused_sprint_retracts();short_unconfirmed_press_has_no_hold();hysteresis_and_wall();
          unobserved_native_uses_mod_state();reversal_mid_ramp_is_continuous();settings_changes(); }
    catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
    std::cout<<"vr_sprint_camera: all checks passed\n";return 0;
}
