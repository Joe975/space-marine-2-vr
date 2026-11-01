#include "synthetic_code.h"
#include "EngineBodyFollowing.h"
#include "VrControllerInput.h"
#include "VrGazeCamera.h"
#include <iostream>
#include <cstring>
#include <stdexcept>
namespace {
void require(bool value,const char* message) { if (!value) { throw std::runtime_error{message}; } }
void require(bool value,const std::string& message) { if (!value) { throw std::runtime_error{message}; } }
bool close(float a,float b) { return std::abs(a-b)<0.0001f; }
template<class T> void write(void* object,size_t offset,const T& value) { std::memcpy(static_cast<uint8_t*>(object)+offset,&value,sizeof(T)); }
uint64_t native_context_calls{},yaw_calls{},native_body_calls{},native_world_calls{};
void* yaw_actor{}; float yaw_degrees{};
__declspec(noinline) void native_update(void*,void*) { ++native_body_calls; }
__declspec(noinline) uint8_t native_world(void*,float*) { ++native_world_calls;return 1; }
__declspec(noinline) void native_context(void*,void*,float* out) { ++native_context_calls; out[0]=0; out[1]=-0.1f; out[2]=0; out[3]=123; out[4]=321; }
void set_yaw(void* actor,float yaw) { ++yaw_calls; yaw_actor=actor; yaw_degrees=yaw; }
void math() {
    {   // The head reaches body following in the Marine's metres, its rotation untouched.
        vr_camera::Matrix turned={0,0,1,0, 0,1,0,0, -1,0,0,0, .1f,.2f,.3f,1};
        const auto marine=VrBodyFollowing::marine_head(turned,1.5f);
        require(close(marine[12],.15f) && close(marine[13],.3f) && close(marine[14],.45f) && std::equal(turned.begin(),turned.begin()+12,marine.begin()),"marine_head scales the translation only");
    }
    const auto now=VrBodyFollowing::Clock::now();
    VrBodyFollowing core; core.identify(1); core.allow(true); core.body(1,2,3,{10,0,20},now);
    auto game=vr_camera::IDENTITY; game[10]=-1; game[12]=10; game[13]=2; game[14]=20;
    auto head=vr_camera::IDENTITY;
    auto anchor=core.camera(game,head,1,1,now);
    require(anchor==game && close(std::abs(*core.yaw(1,now)),180),"initial head must preserve game camera and native +Z yaw convention");
    head={0,0,1,0, 0,1,0,0, -1,0,0,0, 0,0,0,1};
    anchor=core.camera(game,head,1,1,now);
    require(close(*core.yaw(1,now),90),"physical/snap right yaw must face the native character along world +X");
    auto turned_game=game; turned_game[0]=0; turned_game[2]=1; turned_game[8]=1; turned_game[10]=0;
    core.camera(turned_game,head,1,1,now);
    require(close(*core.yaw(1,now),90),"native camera yaw must not feed the head turn back in and rotate body/view twice");
    require(close(core.movement_heading_offset(now),-1.57079632679f),"stick mapping compensates native camera rotation relative to stable VR basis");
    require(core.movement_heading_offset(now+std::chrono::milliseconds(101))==0,"stale camera basis never supplies movement correction");
    require(!core.yaw(99,now) && !core.yaw(1,now+std::chrono::milliseconds(101)),"wrong actors and stale tracking must never receive facing");
    core.allow(false); require(!core.yaw(1,now),"loss of allowed input must release body facing"); core.allow(true);
    auto settings=core.settings(); settings.room_enabled=true; settings.gain=5; settings.deadzone=0;
    require(core.set_settings(settings),"validated live settings must apply");
    head=vr_camera::IDENTITY; head[12]=0.2f;
    core.camera(game,head,1,1,now);
    auto requested=core.velocity(3,{0,0,0},0.02f,now);
    require(close(requested[0],1) && requested[1]==0 && requested[2]==0,"room movement must use physical error and live catch-up gain without vertical velocity");
    core.body(1,2,3,{10.01f,0,20},now);
    core.velocity(3,{0,0,0},0.02f,now); // Solver accepted half of the requested 0.02 displacement.
    anchor=core.camera(game,head,1,1,now);
    vr_camera::Matrix world{},view{};
    require(vr_camera::compose(anchor,head,1,world,view) && close(world[12],10.2f) && close(core.snapshot().consumed[0],0.01f),
        "camera must consume only observed accepted body movement and retain the HMD position once");
    core.velocity(3,{0,0,0},0.02f,now);
    require(close(core.snapshot().consumed[0],0.01f),"blocked solver with no body displacement must not consume requested motion");
    require(core.velocity(3,{1,0,0},0.02f,now)==VrBodyFollowing::Vector{},"first prototype must defer room catch-up while native horizontal locomotion is active");
    settings.gain=10; core.set_settings(settings);
    requested=core.velocity(3,{0,0,0},0.02f,now);
    require(close(requested[0],1.5f),"live response and speed cap must change actual submitted velocity");
    settings.max_speed=0.5f; core.set_settings(settings);
    require(close(core.velocity(3,{0,0,0},0.02f,now)[0],0.5f),"live speed knob must affect actual output");
    core.camera(game,vr_camera::IDENTITY,2,1,now);
    require(core.snapshot().consumed==VrBodyFollowing::Vector{} && core.snapshot().room_error==0,"recenter must discard old consumed motion and pending requests");
    core.identify(44); require(!core.snapshot().anchored && !core.yaw(44,now),"load/owner changes must clear old actor pose and anchor");
    settings.gain=0; require(!core.set_settings(settings),"invalid response settings must reject atomically");
}
// The pre-24-Oct cutscene rule, still the cutscene_hold=false switch: the native cinematic camera.
template<class T> void native_cutscenes(T& core){auto settings=core.settings();settings.cutscene_hold=false;require(core.set_settings(settings),"cutscene hold switch");}
void dodge_facing_authority() {
    const auto start=VrBodyFollowing::Clock::now();
    VrBodyFollowing core;core.identify(1);core.allow(true);native_cutscenes(core);
    auto game=vr_camera::IDENTITY;game[10]=-1;game[13]=2;
    const auto refresh=[&](auto now){core.body(1,2,3,{0,0,0},now);return core.camera(game,vr_camera::IDENTITY,1,1,now);};
    const auto anchor=refresh(start);require(core.yaw(1,start).has_value(),"normal locomotion follows headset facing");
    core.dodge(true,start);
    require(!core.yaw(1,start),"dodge must yield native yaw before native root motion starts");
    auto trailing=game;trailing[12]+=1.9f;
    require(core.camera(trailing,vr_camera::IDENTITY,1,1,start)==anchor && core.snapshot().anchored,"known dodge camera lag retains its established stable view");
    auto remote=game;remote[12]+=3;
    require(core.camera(remote,vr_camera::IDENTITY,1,1,start)!=remote && !core.snapshot().anchored && core.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover,
        "dodge cannot override a genuinely remote camera: an animation takeover holds the view");
    refresh(start);
    require(core.camera(trailing,vr_camera::IDENTITY,1,1,start,true)==trailing && !core.snapshot().anchored,"scripted authority wins even during a requested dodge");
    refresh(start);
    for(int ms:{100,500,1300,1799}) {
        const auto now=start+std::chrono::milliseconds(ms);core.dodge(true,now);
        require(refresh(now)==anchor && core.snapshot().anchored && !core.yaw(1,now),"held dodge preserves camera anchor while native rotation owns root motion");
        require(core.velocity(3,{4,0,0},.02f,now)==VrBodyFollowing::Vector{},"dodge handover must not redirect native velocity");
    }
    auto now=start+VrBodyFollowing::DODGE_FACING_HOLD;refresh(now);core.dodge(true,now);
    require(core.yaw(1,now).has_value(),"held button cannot extend native facing handover forever");
    core.dodge(false,now);core.dodge(true,now);require(!core.yaw(1,now),"a new dodge press starts its own bounded handover");
    core.allow(false);core.allow(true);refresh(now);require(core.yaw(1,now).has_value(),"focus reset clears old dodge authority");
    core.dodge(false,now);core.dodge(true,now);core.identify(2);core.body(2,4,5,{0,0,0},now);core.camera(game,vr_camera::IDENTITY,1,1,now);
    require(core.yaw(2,now).has_value(),"actor replacement never inherits an old dodge hold");
}
void movement_camera_frames() {
    const auto now=VrBodyFollowing::Clock::now();constexpr float radians=.01745329252f;
    for(float native_degrees:{-175.f,-90.f,0.f,90.f,175.f})for(float head_degrees:{-90.f,0.f,30.f,90.f})for(float side:{-1.f,0.f,1.f,2.f}) {
        VrBodyFollowing core;core.identify(1);core.allow(true);core.body(1,2,3,{0,0,0},now);
        auto game=vr_camera::IDENTITY;game[10]=-1;game[13]=2;
        core.camera(game,vr_camera::IDENTITY,1,1,now);
        const float n=native_degrees*radians,h=head_degrees*radians;
        game[0]=-std::cos(n);game[2]=std::sin(n);game[8]=std::sin(n);game[10]=std::cos(n);
        auto head=vr_camera::IDENTITY;head[0]=head[10]=std::cos(h);head[2]=std::sin(h);head[8]=-std::sin(h);
        core.camera(game,head,1,1,now);
        VrControllerInput input;input.enable(true);input.update(VrControllerSample{},0,true,now);
        const bool neutral_dodge=side==2;
        VrControllerSample move;move.move_x=neutral_dodge?0:side;move.move_y=side==0?1.f:0.f;move.confirm=neutral_dodge;
        input.update(move,h+core.movement_heading_offset(now),true,now);
        const auto pad=input.snapshot(now).pad.Gamepad;const float x=pad.sThumbLX/32767.f,y=pad.sThumbLY/32767.f;
        const float world_x=-x*std::cos(n)+y*std::sin(n),world_z=x*std::sin(n)+y*std::cos(n);
        const float target=core.snapshot().yaw*radians;
        const float expected_x=neutral_dodge?-std::sin(target):side==0?std::sin(target):-side*std::cos(target);
        const float expected_z=neutral_dodge?-std::cos(target):side==0?std::cos(target):side*std::sin(target);
        require(std::abs(world_x-expected_x)<.0001f && std::abs(world_z-expected_z)<.0001f,
            "real pad output must retain view-relative left/right/forward across native camera and tracked-head headings");
    }
}
// Gaze camera on (default with -Controls): the native camera is steered toward the
// composed eye through VrGazeCamera while body following keeps the view on its stable
// reference. Once it settles the native camera faces the gaze, so walking forward is plain
// native forward, a neutral dodge is plain native backward, and the movement mapping still
// matches the view at every step on the way (sign conventions of both modules agree).
void gaze_steered_native_camera() {
    constexpr float radians=.01745329252f;
    for(float start_degrees:{-150.f,0.f,120.f})for(float head_degrees:{-100.f,35.f,170.f}) {
        auto now=VrBodyFollowing::Clock::now();
        VrBodyFollowing core;core.identify(1);core.allow(true);core.body(1,2,3,{0,0,0},now);
        // The game's camera family (as movement_camera_frames): right (-cos n,0,sin n), forward (sin n,0,cos n).
        const auto native=[](float n){auto g=vr_camera::IDENTITY;g[0]=-std::cos(n);g[2]=std::sin(n);g[8]=std::sin(n);g[10]=std::cos(n);g[13]=2;return g;};
        float n=start_degrees*radians;
        core.camera(native(n),vr_camera::IDENTITY,1,1,now);
        const float h=head_degrees*radians;
        auto head=vr_camera::IDENTITY;head[0]=head[10]=std::cos(h);head[2]=std::sin(h);head[8]=-std::sin(h);
        vr_gaze::Steering steering;steering.enable(true);
        std::array<vr_gaze::Steering::Stick,2> pipeline{};
        for(int frame=0;frame<90;++frame,now+=std::chrono::milliseconds(33)) {
            const auto applied=pipeline[0];pipeline[0]=pipeline[1];
            // The native response measured live (VrGazeCamera YAW_RESPONSE, ~120 deg/s edge boost).
            const auto& curve=vr_gaze::Steering::YAW_RESPONSE;const float a=std::abs(applied.x);float rate=0;
            if(a>=.85f) rate=120;
            else if(a>curve.front().stick) {rate=curve.back().rate;for(size_t i=1;i<curve.size();++i) if(a<=curve[i].stick){rate=curve[i-1].rate+(curve[i].rate-curve[i-1].rate)*(a-curve[i-1].stick)/(curve[i].stick-curve[i-1].stick);break;}}
            n-=std::copysign(rate,applied.x)*radians*.033f; // Increasing n turns this camera left; +x turns right.
            core.body(1,2,3,{0,0,0},now);
            const auto anchor=core.camera(native(n),head,1,1,now);
            vr_camera::Matrix world{},view{};require(vr_camera::compose(anchor,head,1,world,view),"eye composes");
            steering.observe(native(n),{world[8],world[9],world[10]},now);
            pipeline[1]=steering.stick(true,now).value_or(vr_gaze::Steering::Stick{});
            // Walking forward stays along the view while the native camera turns underneath.
            VrControllerInput input;input.enable(true);input.update(VrControllerSample{},0,true,now);
            VrControllerSample forward;forward.move_y=1;input.update(forward,h+core.movement_heading_offset(now),true,now);
            const auto pad=input.snapshot(now).pad.Gamepad;const float x=pad.sThumbLX/32767.f,y=pad.sThumbLY/32767.f;
            const float world_x=-x*std::cos(n)+y*std::sin(n),world_z=x*std::sin(n)+y*std::cos(n);
            const float target=core.snapshot().yaw*radians;
            require(std::abs(world_x-std::sin(target))<.001f && std::abs(world_z-std::cos(target))<.001f,"walking stays view-relative while the native camera is steered");
        }
        const auto settled=steering.latest(now-std::chrono::milliseconds(33));
        require(settled && std::abs(settled->yaw_degrees)<=6,"the steered native camera ends on the gaze");
        VrControllerInput input;input.enable(true);input.update(VrControllerSample{},0,true,now);
        VrControllerSample forward;forward.move_y=1;input.update(forward,h+core.movement_heading_offset(now),true,now);
        auto pad=input.snapshot(now).pad.Gamepad;
        require(pad.sThumbLY>32767*.99f && std::abs(pad.sThumbLX)<32767*.11f,"on the gaze, view forward is native forward");
        VrControllerSample dodge;dodge.confirm=true;input.update(VrControllerSample{},0,true,now);input.update(dodge,h+core.movement_heading_offset(now),true,now);
        pad=input.snapshot(now).pad.Gamepad;
        require(pad.sThumbLY<-32767*.99f && std::abs(pad.sThumbLX)<32767*.11f,"on the gaze, a neutral dodge is native backward");
    }
}
void head_placement_turns() {
    const auto now=VrBodyFollowing::Clock::now();
    VrBodyFollowing core; core.identify(1); core.allow(true); core.body(1,2,3,{0,0,0},now);
    auto game=vr_camera::IDENTITY; game[10]=-1; game[12]=0.1f; game[13]=2.3f; game[14]=-0.35f;
    auto head=vr_camera::IDENTITY;
    require(core.camera(game,head,1,0.9f,now)==game,"neutral reference must preserve existing lateral/forward head placement");
    head={0,0,1,0, 0,1,0,0, -1,0,0,0, 0,0,0,1};
    auto anchor=core.camera(game,head,1,0.9f,now);
    require(close(anchor[12],0.35f) && close(anchor[14],0.1f) && close(anchor[13],2.3f),
        "quarter turn must rotate root-relative head placement with body facing, preserving native height");
    auto native_turned=anchor; native_turned[0]=0; native_turned[2]=1; native_turned[8]=1; native_turned[10]=0;
    auto repeated=core.camera(native_turned,head,1,0.9f,now);
    require(repeated==anchor && close(*core.yaw(1,now),90),"native camera heading and translation feedback must not double-rotate calibrated head offset");
    head=vr_camera::IDENTITY; head[0]=-1; head[10]=-1;
    anchor=core.camera(game,head,1,0.9f,now);
    require(close(anchor[12],-0.1f) && close(anchor[14],0.35f),"half turn must keep camera on the same body-local side instead of exposing the back of the head");
    // Production eye calibration retracts 20 physical cm along level tracked-head forward.
    auto eye=head; eye[14]=-0.2f; eye[13]=0.05f;
    vr_camera::Matrix world{},view{};
    require(vr_camera::compose(anchor,eye,0.9f,world,view) && close(world[14],0.17f) && close(world[13],2.345f),
        "half-turn anchor plus eye calibration must retract 20 cm and raise 5 cm exactly once at game scale");
    head=vr_camera::IDENTITY; anchor=core.camera(game,head,1,0.9f,now);
    require(anchor==game,"returning to original heading must restore placement without accumulated orbit drift");
    head[12]=0.2f; anchor=core.camera(game,head,1,0.9f,now);
    require(anchor==game && vr_camera::compose(anchor,head,0.9f,world,view) && close(world[12],0.28f),
        "physical lean remains a single tracked displacement separate from rotating body-local placement");
}
// Joe, 24 Oct: standing still, the armour moved with every look around. The body keeps its
// facing until the head is turn_threshold_degrees (30) from it, then catches up.
void turn_threshold() {
    using Clock=VrBodyFollowing::Clock;auto t=Clock::now();const auto frame=std::chrono::milliseconds(33);
    VrBodyFollowing core;core.identify(1);core.allow(true);
    auto game=vr_camera::IDENTITY;game[10]=-1;game[12]=.1f;game[13]=2.3f;game[14]=-.35f;
    const auto turn=[](float degrees){const float a=degrees/57.295779513f;auto h=vr_camera::IDENTITY;h[0]=std::cos(a);h[2]=std::sin(a);h[8]=-std::sin(a);h[10]=std::cos(a);return h;};
    const auto step=[&](float degrees){t+=frame;core.body(1,2,3,{0,0,0},t);return core.camera(game,turn(degrees),1,.9f,t);};
    const auto start=step(0);const float y0=*core.yaw(1,t);
    const auto body=[&]{return std::remainder(*core.yaw(1,t)-y0,360.f);};
    require(core.settings().turn_threshold_degrees==30,"30 degrees by default");
    // Looking around within 30 degrees: the body and the eye's placement over it stay put.
    for(int d=1;d<=25;++d){const auto a=step(float(d));require(std::abs(body())<1e-3f && close(a[12],start[12]) && close(a[14],start[14]),std::string("a look within the threshold leaves the body and the eye placement: d=")+std::to_string(d)+" body="+std::to_string(body())+" x="+std::to_string(a[12]-start[12])+" z="+std::to_string(a[14]-start[14]));}
    for(int d=24;d>=-25;--d){step(float(d));require(std::abs(body())<1e-3f,"either way");}
    // Past 30 the body catches up with the head and settles on it.
    for(int d=-26;d>=-35;--d)step(float(d));
    require(std::abs(body())>1,"past the threshold the body starts to turn");
    for(int i=0;i<9;++i)step(-35);
    const auto settled=step(-35);
    require(std::abs(std::abs(body())-35)<1e-3f,"and catches up with the head within a few frames");
    // Holding at this facing, the head's axes turned back to the body give the settled placement.
    for(int d=-34;d<=-10;++d){const auto a=step(float(d));require(close(a[12],settled[12]) && close(a[14],settled[14]),"held at -35: placement unchanged while looking back");}
    require(std::abs(std::abs(body())-35)<1e-3f,"after catching up it holds again within the threshold");
    // A snap turn (30 degrees in one frame) turns the body at once, by the snap.
    const float before=body();step(20);require(std::abs(std::abs(std::remainder(body()-before,360.f))-30)<1e-3f,"a snap turn takes the body along");
    // Walking: the body follows the head every frame.
    VrBodyFollowing walker;walker.identify(1);walker.allow(true);auto u=Clock::now();VrBodyFollowing::Vector root{0,0,0};
    walker.body(1,2,3,root,u);walker.camera(game,turn(0),1,.9f,u);const float w0=*walker.yaw(1,u);
    for(int i=1;i<=10;++i){u+=frame;root[2]+=.05f;walker.body(1,2,3,root,u);walker.camera(game,turn(float(i)),1,.9f,u);}
    require(std::abs(std::abs(std::remainder(*walker.yaw(1,u)-w0,360.f))-10)<1e-3f,"walking, the body turns with the head as before");
    // 0: the body follows the head every frame (the rule before 24 Oct); bounds.
    auto settings=core.settings();settings.turn_threshold_degrees=0;require(core.set_settings(settings),"0 is valid");
    step(150);require(std::abs(std::abs(body())-150)<1e-3f,"threshold 0: follows at once");
    settings.turn_threshold_degrees=91;require(!core.set_settings(settings),"over 90 is rejected");
    settings.turn_threshold_degrees=-1;require(!core.set_settings(settings),"negative is rejected");
    // The camera guard file: the key is optional (older files load with 30) and tunable.
    const std::string limits="camera_radius_metres=1\ncamera_height_metres=4\n";
    std::istringstream old{limits};const auto loaded=VrBodyFollowing::Settings::read_camera_guard(old);
    require(loaded && loaded->turn_threshold_degrees==30,"a file without the key keeps 30");
    std::istringstream tuned{limits+"turn_threshold_degrees=45\n"};const auto tuned_settings=VrBodyFollowing::Settings::read_camera_guard(tuned);
    require(tuned_settings && tuned_settings->turn_threshold_degrees==45,"the file sets the threshold");
    std::istringstream bad{limits+"turn_threshold_degrees=120\n"};require(!VrBodyFollowing::Settings::read_camera_guard(bad),"an out-of-range threshold rejects the file");
}
void external_camera_handover() {
    const auto now=VrBodyFollowing::Clock::now();
    VrBodyFollowing core;core.identify(1);core.allow(true);core.body(1,2,3,{10,0,20},now);
    auto game=vr_camera::IDENTITY;game[10]=-1;game[12]=10.1f;game[13]=2.5f;game[14]=19.8f;
    auto head=vr_camera::IDENTITY;
    core.camera(game,head,1,.9f,now);
    require(core.snapshot().anchored && core.yaw(1,now).has_value(),"gameplay starts body following");
    auto cinematic=game;cinematic[12]+=30;cinematic[14]-=12;
    head={0,0,1,0,0,1,0,0,-1,0,0,0,0,0,0,1};
    require(core.camera(cinematic,head,1,.9f,now)==cinematic && !core.snapshot().anchored && !core.yaw(1,now),
        "remote sequence camera must not orbit around the player or keep writing native yaw");
    require(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::external && core.velocity(3,{0,0,0},.02f,now)==VrBodyFollowing::Vector{},
        "external camera releases room motion too");
    require(core.camera(cinematic,head,2,.9f,now)==cinematic,"recenter cannot turn an external shot into a body anchor");
    vr_camera::Matrix world{},view{};
    auto anchor=core.camera(game,head,2,.9f,now);
    require(vr_camera::compose(anchor,head,.9f,world,view) && close(world[12],game[12]) && close(world[14],game[14]) &&
        close(world[8],game[8]) && close(world[10],game[10]),"gameplay handover preserves native position/heading without double head yaw or manual recenter");
    auto repeated=core.camera(game,head,2,.9f,now);require(repeated==anchor,"both eyes must share one stable handover");
    head=vr_camera::IDENTITY;anchor=core.camera(game,head,2,.9f,now);
    require(std::hypot(anchor[12]-10,anchor[14]-20)<.3f,"later head turns use only small gameplay placement, never the cinematic offset");
    auto high=game;high[13]=20;
    require(core.camera(high,head,2,.9f,now)==high && !core.snapshot().anchored,"vertical sequence displacement also releases body ownership");
    // The envelope itself, without the takeover hold (covered by animation_takeover_*).
    auto settings=core.settings();settings.camera_radius_metres=.4f;settings.takeover_enabled=false;core.set_settings(settings);
    auto edge=game;edge[12]=10.6f;
    require(core.camera(edge,head,2,.9f,now)==edge && !core.snapshot().anchored,"tight live camera envelope rejects actual distant views");
    settings.camera_radius_metres=1.5f;core.set_settings(settings);core.camera(edge,head,2,.9f,now);
    require(core.snapshot().anchored,"changing live radius changes real anchor eligibility");
    settings.camera_height_metres=2;core.set_settings(settings);core.camera(game,head,2,.9f,now);
    require(!core.snapshot().anchored,"live height envelope changes eligibility");
    settings.camera_height_metres=4;core.set_settings(settings);core.camera(game,head,2,.9f,now);
    require(core.snapshot().anchored,"height reset restores gameplay eligibility");
    core.camera(game,head,2,.9f,now+std::chrono::milliseconds(101));
    require(!core.snapshot().anchored && !core.yaw(1,now),"stale body data clears anchor and previous head ownership immediately");
    std::istringstream config{"camera_radius_metres=1\ncamera_height_metres=4\n"};
    require(VrBodyFollowing::Settings::read_camera_guard(config).has_value(),"guard preset parses through production loader");
    std::istringstream duplicate{"camera_radius_metres=1\ncamera_height_metres=4\ncamera_radius_metres=2\n"};
    require(!VrBodyFollowing::Settings::read_camera_guard(duplicate),"duplicate guard keys reject atomically");
    std::istringstream invalid{"camera_radius_metres=nan\ncamera_height_metres=4\n"};
    require(!VrBodyFollowing::Settings::read_camera_guard(invalid),"invalid guard value rejects");
}
// 26 Oct playtest: the game's melee turns the Marine onto its target, and the yaw written after
// every native update undid that, so strikes went where the body faced while the lunge slid
// towards the enemy. A melee press hands the facing to the game while held and for
// MELEE_FACING_HOLD after; the body (and the eye's placement over it) follows the native facing,
// then turns back to the head at the usual catch-up rate instead of snapping.
void melee_facing() {
    // Which native buttons count as a melee press: RB, D-pad up and (Joe, 26 Oct) LB, the parry.
    require(VrBodyFollowing::melee_press(0x0200) && VrBodyFollowing::melee_press(0x0001) && VrBodyFollowing::melee_press(0x0100),"light, power and parry hand the facing to the game");
    require(!VrBodyFollowing::melee_press(0x1000) && !VrBodyFollowing::melee_press(0x4000) && !VrBodyFollowing::melee_press(0x2000) && !VrBodyFollowing::melee_press(0),"A, X and B don't (the dodge has its own hold)");
    using Clock=VrBodyFollowing::Clock;const auto frame=std::chrono::milliseconds(33);
    auto game=vr_camera::IDENTITY;game[10]=-1;game[12]=.1f;game[13]=2.3f;game[14]=-.35f;
    const auto turn=[](float degrees){const float a=degrees/57.295779513f;auto h=vr_camera::IDENTITY;h[0]=std::cos(a);h[2]=std::sin(a);h[8]=-std::sin(a);h[10]=std::cos(a);return h;};
    struct Rig {VrBodyFollowing core;Clock::time_point t{Clock::now()};VrBodyFollowing::Vector root{};float y0{};vr_camera::Matrix anchor{};};
    // One frame: the body update (the native facing the game left, if any), then the eye.
    const auto step=[&](Rig& r,float head,std::optional<float> native){
        // The game's camera walks with the root, so a walk stays inside the first-person envelope.
        auto moved=game;for(int j=0;j<3;++j)moved[12+j]+=r.root[j];
        r.t+=frame;r.core.body(1,2,3,r.root,r.t);if(native)r.core.native_facing(1,r.y0+*native,r.t);r.anchor=r.core.camera(moved,turn(head),1,.9f,r.t);};
    const auto start=[&](Rig& r){r.core.identify(1);r.core.allow(true);step(r,0,std::nullopt);r.y0=*r.core.yaw(1,r.t);};
    const auto body=[](Rig& r){return std::remainder(r.core.snapshot().yaw-r.y0,360.f);};
    // The eye's placement for a body facing `degrees` (threshold 0, head there): it depends only on the body's yaw.
    const auto placement=[&](float degrees){Rig r;start(r);auto s=r.core.settings();s.turn_threshold_degrees=0;r.core.set_settings(s);step(r,degrees,std::nullopt);return r.anchor;};

    Rig a;start(a);
    require(a.core.snapshot().melee_holds==0 && a.core.yaw(1,a.t),"before a melee press the mod writes the facing");
    a.core.melee(true,a.t);
    require(a.core.snapshot().melee_holds==1,"a press is counted");
    // The game turns the Marine 20 degrees onto a target over six frames; the head stays still.
    // The button (polled every frame) is let go at frame 3.
    Clock::time_point released{};
    for(int i=1;i<=6;++i) {
        a.core.melee(i<3,a.t);if(i==3)released=a.t-frame;
        step(a,0,20.f*i/6);
        require(!a.core.yaw(1,a.t) && a.core.snapshot().melee_facing,"while melee has the facing the mod never writes it");
        require(std::abs(body(a)-20.f*i/6)<1e-3f,"the body's facing is the native one");
    }
    require(a.core.snapshot().melee_holds==1,"one press, one hold");
    const auto at20=placement(-20),at0=placement(0); // turn(d) is a yaw of -d
    require(close(a.anchor[12],at20[12]) && close(a.anchor[14],at20[14]) && !close(at20[12],at0[12]),"the eye's placement turns with the native body: x="+std::to_string(a.anchor[12])+","+std::to_string(at20[12])+","+std::to_string(at0[12])+" z="+std::to_string(a.anchor[14])+","+std::to_string(at20[14])+","+std::to_string(at0[14]));
    // Held for MELEE_FACING_HOLD after the last pressed poll, then the mod writes again: the
    // native facing is kept (20 is within the 30 degree threshold), no snap back to the head.
    while(a.t+frame<released+VrBodyFollowing::MELEE_FACING_HOLD) {a.core.melee(false,a.t);step(a,0,20.f);require(!a.core.yaw(1,a.t),"held for MELEE_FACING_HOLD after the release");}
    step(a,0,20.f);
    step(a,0,20.f);
    const auto written=a.core.yaw(1,a.t);
    require(written && std::abs(std::remainder(*written-a.y0-20,360.f))<1e-3f && !a.core.snapshot().melee_facing,"afterwards the mod writes the facing the game left");
    for(int i=0;i<10;++i) step(a,0,std::nullopt);
    require(std::abs(body(a)-20)<1e-3f,"standing within the threshold, the body stays on the target");

    // The game turned it 60 degrees: past the threshold, the body turns back at the usual rate.
    Rig b;start(b);b.core.melee(true,b.t);b.core.melee(false,b.t);
    for(int i=1;i<=6;++i) step(b,0,10.f*i);
    while(!b.core.yaw(1,b.t)) step(b,0,60.f);
    const float first=body(b);
    require(first<60.01f && first>60-240*.034f-1e-3f,std::string("no snap: one catch-up step from the native facing, body=")+std::to_string(first));
    for(int i=0;i<20;++i) step(b,0,std::nullopt);
    require(std::abs(body(b))<1e-3f,"and settles on the head");

    // Walking straight after: the body also turns back at the catch-up rate, then follows the head.
    Rig w;start(w);w.core.melee(true,w.t);w.core.melee(false,w.t);
    for(int i=1;i<=6;++i) step(w,0,10.f*i);
    for(int i=0;i<40 && !w.core.yaw(1,w.t);++i) step(w,0,60.f);
    w.root[2]+=.05f;step(w,0,std::nullopt);w.root[2]+=.05f;step(w,0,std::nullopt);w.root[2]+=.05f;step(w,0,std::nullopt);w.root[2]+=.05f;step(w,0,std::nullopt);
    require(body(w)>60-5*240*.034f-1e-3f && body(w)<59,std::string("walking after melee turns back, it does not snap: body=")+std::to_string(body(w)));
    for(int i=0;i<20;++i){w.root[2]+=.05f;step(w,float(i),std::nullopt);}
    require(w.core.snapshot().anchored && std::abs(body(w)+19)<1e-3f,"then walking follows the head every frame again");

    // Bounds: a held button hands the facing over for at most MELEE_FACING_LIMIT plus the hold.
    Rig h;start(h);const auto pressed=h.t;
    while(h.t<pressed+VrBodyFollowing::MELEE_FACING_LIMIT+VrBodyFollowing::MELEE_FACING_HOLD+frame) {h.core.melee(true,h.t);step(h,0,0.f);}
    h.core.melee(true,h.t);step(h,0,0.f);require(h.core.yaw(1,h.t).has_value(),"a held button cannot keep the facing forever");
    h.core.melee(false,h.t);h.core.melee(true,h.t);step(h,0,0.f);require(!h.core.yaw(1,h.t) && h.core.snapshot().melee_holds==2,"a new press starts its own hold");
    h.core.allow(false);h.core.allow(true);step(h,0,0.f);require(h.core.yaw(1,h.t).has_value(),"losing focus or input clears it");
    h.core.melee(true,h.t);h.core.identify(2);h.core.body(2,4,5,{0,0,0},h.t);h.core.camera(game,vr_camera::IDENTITY,1,1,h.t);
    require(h.core.yaw(2,h.t).has_value() && h.core.snapshot().melee_holds==3,"a new actor never inherits it (the count is kept)");
    // Without a native reading the facing is still the game's; the body bookkeeping keeps its own rule.
    Rig n;start(n);n.core.melee(true,n.t);step(n,40,std::nullopt);require(!n.core.yaw(1,n.t),"no native reading: still yielded");
}
void dodge_camera_recovery() {
    const auto now=VrBodyFollowing::Clock::now();VrBodyFollowing core;core.identify(1);core.allow(true);
    // The native-external return path; the takeover return is in animation_takeover_return.
    auto legacy=core.settings();legacy.takeover_enabled=false;require(core.set_settings(legacy),"takeover can be switched off");
    core.body(1,2,3,{0,0,0},now);
    auto game=vr_camera::IDENTITY;game[10]=-1;game[13]=2.3f;game[14]=-.35f;
    const auto head=vr_camera::IDENTITY;core.camera(game,head,1,1,now);
    // Recorded dodge shape: root advances, the native chase camera trails by
    // >1m, then comes back within the guard while still ~0.85m behind the root.
    core.body(1,2,3,{0,0,-4},now);auto trailing=game;trailing[14]=-2.7f;
    require(core.camera(trailing,head,1,1,now)==trailing && !core.snapshot().anchored,"remote native camera still releases body authority");
    trailing[14]=-3.15f;
    auto restored=core.camera(trailing,head,1,1,now);
    require(close(restored[14],-4.35f),"dodge recovery retains calibrated forward offset instead of capturing trailing camera");
    trailing[14]=-4.35f;require(core.camera(trailing,head,1,1,now)==restored,"native camera catch-up cannot leave permanent placement drift");
    // A new XR reference intentionally calibrates anew, even after a remote view.
    trailing[14]=-2.7f;core.camera(trailing,head,1,1,now);trailing[14]=-3.15f;
    require(core.camera(trailing,head,2,1,now)==trailing,"explicit recenter still adopts current placement");
}
// Animation takeovers (docs/ANIMATION_CAMERA.md). Native first-person camera facing -Z
// (screen-right +X) 2.3 units above the root, at 0.9 units per metre, like the campaign.
namespace takeover {
using Clock=VrBodyFollowing::Clock;using Vector=VrBodyFollowing::Vector;
constexpr float S=.9f;constexpr auto FRAME=std::chrono::milliseconds(33);
vr_camera::Matrix native_at(const Vector& root){auto g=vr_camera::IDENTITY;g[10]=-1;g[12]=root[0];g[13]=root[1]+2.3f;g[14]=root[2]-.35f*S;return g;}
vr_camera::Matrix yawed_head(float degrees){const float a=degrees*.01745329252f;auto h=vr_camera::IDENTITY;h[0]=h[10]=std::cos(a);h[2]=std::sin(a);h[8]=-std::sin(a);return h;}
// Three seconds of settled first person: anchors and learns the standing eye height.
Clock::time_point settle(VrBodyFollowing& core,const Vector& root,const vr_camera::Matrix& head,Clock::time_point t){
    core.identify(1);core.allow(true);
    for(int i=0;i<90;++i,t+=FRAME){core.body(1,2,3,root,t);core.camera(native_at(root),head,1,S,t);}
    require(core.snapshot().anchored && core.snapshot().camera_mode==VrBodyFollowing::CameraMode::body,"first person anchors before the takeover");
    require(std::abs(core.snapshot().standing_height-2.3f)<1e-3f,"standing eye height is learned from the settled first-person camera");
    return t;
}
bool same_rotation(const vr_camera::Matrix& a,const vr_camera::Matrix& b){for(int i:{0,1,2,4,5,6,8,9,10}) if(std::abs(a[i]-b[i])>1e-5f) return false;return true;}
}
void animation_takeover_holds_place() {
    using namespace takeover;
    VrBodyFollowing core;Vector root{10,0,20};const auto head=yawed_head(30);
    auto t=settle(core,root,head,Clock::now());
    const auto first_person=core.camera(native_at(root),head,1,S,t);
    // The execution: the game's camera pulls 3 m back and bobs while root motion carries the
    // character 2 units forward and 0.3 units up. The view stays where first person stood.
    float lowest=1e9f,highest=-1e9f;
    for(int i=0;i<90;++i,t+=FRAME) {
        root[2]=20-2.f*i/89;root[1]=.3f*i/89;core.body(1,2,3,root,t);
        const auto animation=VrBodyFollowing::pulled_back(native_at(root),root,S,i*.033f);
        lowest=std::min(lowest,animation[13]);highest=std::max(highest,animation[13]);
        const auto anchor=core.camera(animation,head,1,S,t);
        require(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover && !core.snapshot().anchored && !core.yaw(1,t),
            "a pulled-back animation camera is a takeover: no anchored first person, no facing writes");
        require(close(anchor[13],2.3f),"head height stays at the entry's standing eye height while the root rises and the camera bobs");
        require(close(anchor[12],first_person[12]) && close(anchor[14],first_person[14]),"the view stays on the first-person spot, not dragged by the root");
        require(same_rotation(anchor,first_person),"no forced yaw and no roll: the takeover keeps the first-person heading");
        // Only the real head moves the eye, 1:1 at game scale.
        auto moved=head;moved[12]=.1f;moved[13]=-.2f;vr_camera::Matrix world{},view{},still{},still_view{};
        require(vr_camera::compose(anchor,moved,S,world,view) && vr_camera::compose(anchor,head,S,still,still_view),"eyes compose");
        require(close(world[13]-still[13],-.2f*S) && std::abs(std::hypot(world[12]-still[12],world[14]-still[14])-.1f*S)<1e-4f,"real head motion moves the view exactly once");
    }
    require(highest-lowest>.4f*S,"the injected animation camera really bobbed");
    require(core.snapshot().takeovers==1,"one takeover");
    // No first-person view yet (an animation straight after a cutscene or a load): hold the
    // character's own spot on the native camera's level heading, which the view already shows.
    VrBodyFollowing fresh;fresh.identify(1);fresh.allow(true);auto now=Clock::now();Vector r{0,0,0};fresh.body(1,2,3,r,now);
    auto animation=native_at(r);animation[12]-=.5f*S;animation[14]+=3*S;
    const auto held=fresh.camera(animation,vr_camera::IDENTITY,1,S,now);
    require(fresh.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover && same_rotation(held,native_at(r)) && close(held[12],0) &&
        close(held[14],0) && close(held[13],2.55f*S),"after a cutscene: on the character's spot, native heading, default eye height");
    for(int i=0;i<10;++i){now+=FRAME;fresh.body(1,2,3,r,now);fresh.camera(native_at(r),vr_camera::IDENTITY,1,S,now);}
    require(fresh.snapshot().anchored && fresh.snapshot().camera_mode==VrBodyFollowing::CameraMode::body,"and first person starts once the game's camera settles");
}
void animation_takeover_levels() {
    using namespace takeover;
    VrBodyFollowing core;Vector root{0,0,0};const auto head=vr_camera::IDENTITY;
    auto t=settle(core,root,head,Clock::now());
    const auto pulled=[&](float seconds){return VrBodyFollowing::pulled_back(native_at(root),root,S,seconds);};
    core.body(1,2,3,root,t);core.camera(pulled(0),head,1,S,t);
    // Root wobble and a 4-unit ledge drop: the view never moves vertically during the takeover.
    for(int i=0;i<30;++i,t+=FRAME){root[1]=.3f*S*std::sin(i*.4f);core.body(1,2,3,root,t);
        require(close(core.camera(pulled(i*.033f),head,1,S,t)[13],2.3f),"root height wobble never moves the view");}
    for(int i=0;i<=15;++i,t+=FRAME){root[1]=-4.f*i/15;root[2]-=.3f;core.body(1,2,3,root,t);
        require(close(core.camera(pulled(1),head,1,S,t)[13],2.3f),"the view does not ride the fall");}
    for(int i=0;i<15;++i,t+=FRAME){core.body(1,2,3,root,t);
        require(close(core.camera(pulled(1.5f),head,1,S,t)[13],2.3f) && core.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover,
            "nor re-level after landing: the drop rejoins first person when the game's camera settles");}
    const auto back=t;
    while(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover && t-back<std::chrono::seconds(1)){core.body(1,2,3,root,t);core.camera(native_at(root),head,1,S,t);t+=FRAME;}
    const auto anchor=core.camera(native_at(root),head,1,S,t);
    require(core.snapshot().anchored && close(anchor[13],root[1]+2.3f) && close(anchor[14],root[2]-.35f*S),"first person resumes on the landed character");
}
// The character's head during a takeover: hidden while the eye is still inside it, drawn at
// full size once the view is clear of it (head_clear_metres), with hysteresis.
void animation_takeover_head() {
    using namespace takeover;
    VrBodyFollowing core;Vector root{0,0,0};const auto head=vr_camera::IDENTITY;
    auto t=settle(core,root,head,Clock::now());
    const auto eye=core.camera(native_at(root),head,1,S,t); // the first-person eye: (0,2.3,-0.315)
    Vector bone{0,2.15f,-.2f};core.head_bone(1,bone,t);
    require(!core.snapshot().show_head,"first person hides the head");
    const auto distance=[&]{return std::hypot(eye[12]-bone[0],eye[13]-bone[1],eye[14]-bone[2])/S;};
    const float clear=core.settings().head_clear_metres;
    const auto step=[&](Vector at){bone=at;t+=FRAME;core.body(1,2,3,root,t);core.head_bone(1,bone,t);
        core.camera(VrBodyFollowing::pulled_back(native_at(root),root,S,0),head,1,S,t);return core.snapshot().show_head;};
    require(!step(bone) && core.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover,"the takeover starts with the eye inside the helmet: still hidden");
    // The character lunges forward: its head leaves the eye.
    std::optional<float> shown_at;
    for(int i=0;i<40 && !shown_at;++i) if(step({0,2.15f,-.2f-.03f*i})) shown_at=distance();
    require(shown_at && *shown_at>=clear && *shown_at<clear+.04f,"drawn once the eye is clear of the head: "+std::to_string(shown_at.value_or(-1)));
    require(step({0,eye[13],eye[14]-(clear-.05f)*S}),"coming back a little keeps it drawn (no flicker at the threshold)");
    require(!step({0,eye[13],eye[14]-(clear-.15f)*S}),"back inside the return margin: hidden again");
    require(!step({0,eye[13],eye[14]-.2f*S}),"right at the eye: hidden");
    // Another actor's head is ignored; without a fresh head the root stands in for it.
    core.head_bone(7,{50,0,50},t);require(!step(bone),"another actor's head bone is ignored");
    for(int i=0;i<5;++i){t+=FRAME;root[2]-=.3f;core.body(1,2,3,root,t);core.camera(VrBodyFollowing::pulled_back(native_at(root),root,S,0),head,1,S,t);}
    require(core.snapshot().show_head,"no fresh head bone: the moved root shows the head is clear");
    // Native cameras show the head; first person hides it again.
    auto remote=native_at(root);remote[12]+=9*S;core.camera(remote,head,1,S,t);
    require(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::external && core.snapshot().show_head,"a remote native camera shows the head");
    for(int i=0;i<12;++i){t+=FRAME;core.body(1,2,3,root,t);core.camera(native_at(root),head,1,S,t);}
    require(core.snapshot().anchored && !core.snapshot().show_head,"first person hides it again");
    core.camera(native_at(root),head,1,S,t,true);
    require(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::scripted && core.snapshot().show_head,"a scripted cinematic shows it");
    auto settings=core.settings();settings.head_clear_metres=.1f;require(!core.set_settings(settings),"head clearance is bounded");
}
void animation_takeover_return() {
    using namespace takeover;
    VrBodyFollowing core;Vector root{10,0,20};auto head=yawed_head(30);
    auto t=settle(core,root,head,Clock::now());
    const auto before=core.camera(native_at(root),head,1,S,t);const auto yaw=*core.yaw(1,t);
    for(int i=0;i<30;++i,t+=FRAME){root[0]+=.05f;core.body(1,2,3,root,t);core.camera(VrBodyFollowing::pulled_back(native_at(root),root,S,i*.033f),head,1,S,t);}
    // The native camera glides back: inside the envelope but still 0.8 m out and high. The view
    // waits (no glide-in bob) and returns at most INSIDE_LIMIT later.
    auto gliding=native_at(root);gliding[14]+=.8f*S;gliding[13]+=.5f*S;const auto inside=t;
    while(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover && t-inside<std::chrono::seconds(2)){core.body(1,2,3,root,t);core.camera(gliding,head,1,S,t);t+=FRAME;}
    require(t-inside>=VrBodyFollowing::INSIDE_LIMIT && t-inside<=VrBodyFollowing::INSIDE_LIMIT+2*FRAME,"an unsettled glide-in returns after the inside limit");
    // A settled camera returns after SETTLE_TIME, to the same first-person placement and heading.
    for(int i=0;i<10;++i,t+=FRAME){core.body(1,2,3,root,t);core.camera(VrBodyFollowing::pulled_back(native_at(root),root,S,0),head,1,S,t);}
    const auto settled=t;std::optional<vr_camera::Matrix> back;
    while(t-settled<std::chrono::seconds(1)){core.body(1,2,3,root,t);const auto a=core.camera(native_at(root),head,1,S,t);
        if(core.snapshot().anchored){back=a;break;}t+=FRAME;}
    require(back && t-settled>=VrBodyFollowing::SETTLE_TIME && t-settled<=VrBodyFollowing::SETTLE_TIME+FRAME,"a settled camera returns to first person after 150 ms");
    require(same_rotation(*back,before) && close((*back)[12]-root[0],before[12]-10) && close((*back)[14]-root[2],before[14]-20),
        "return keeps the first-person placement relative to the body and the heading (no rebase)");
    require(core.yaw(1,t) && close(*core.yaw(1,t),yaw),"facing resumes at the same tracked yaw: no turn on return");
    // Remote shots stay native; a recenter during a takeover hands over to the native camera.
    auto remote=native_at(root);remote[12]+=9*S;
    core.camera(VrBodyFollowing::pulled_back(native_at(root),root,S,0),head,1,S,t);
    require(core.camera(remote,head,1,S,t)==remote && core.snapshot().camera_mode==VrBodyFollowing::CameraMode::external,"a camera beyond 8 m is a remote shot, left native");
    core.camera(native_at(root),head,1,S,t);t+=std::chrono::milliseconds(200);core.body(1,2,3,root,t);core.camera(native_at(root),head,1,S,t);
    const auto animation=VrBodyFollowing::pulled_back(native_at(root),root,S,0);core.camera(animation,head,1,S,t);
    require(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover,"takeover again");
    const auto recentred=core.camera(animation,head,2,S,t);
    require(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover && same_rotation(recentred,animation),"a recenter restarts the hold on the native heading instead of keeping a stale one");
    // Switched off: the old behaviour, the game's own animation camera.
    VrBodyFollowing off;auto u=settle(off,root,head,Clock::now());auto settings=off.settings();settings.takeover_enabled=false;off.set_settings(settings);
    require(off.camera(animation,head,1,S,u)==animation,"takeover_enabled=false keeps the native animation camera");
}
// The stick in a takeover (docs/TAKEOVER_RETURN_2026-10-26.md): the game reads the native stick
// against its own camera, which a takeover turns away from the view. The mapping must keep the
// view's heading, as in first person. On 26 Oct it fell back to 0 and the stick moved the Marine a
// median 124 degrees off where Joe looked (26 Oct 16:50:41, the Vanguard's grapnel: 125 degrees).
void takeover_stick_follows_view() {
    using namespace takeover;constexpr float radians=.01745329252f;
    const auto native=[](float n,const Vector& at,float back){auto g=vr_camera::IDENTITY;g[0]=-std::cos(n);g[2]=std::sin(n);g[8]=std::sin(n);g[10]=std::cos(n);
        g[12]=at[0]-back*S*std::sin(n);g[13]=at[1]+2.3f;g[14]=at[2]-back*S*std::cos(n);return g;};
    for(float turned_degrees:{-120.f,-45.f,90.f,170.f})for(float head_degrees:{-60.f,0.f,40.f})for(int side:{0,1}) {
        VrBodyFollowing core;Vector root{0,0,0};const auto head=yawed_head(head_degrees);
        auto t=settle(core,root,head,Clock::now());
        // The execution camera: 4 m out and turned away from the first-person heading.
        const float n=(180+turned_degrees)*radians;const auto game=native(n,root,4);
        t+=FRAME;core.body(1,2,3,root,t);const auto anchor=core.camera(game,head,1,S,t);
        require(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::takeover,"a takeover");
        vr_camera::Matrix eye{},view{};require(vr_camera::compose(anchor,head,S,eye,view),"eye composes");
        VrControllerInput input;input.enable(true);input.update(VrControllerSample{},0,true,t);
        VrControllerSample move;move.move_x=side?1.f:0.f;move.move_y=side?0.f:1.f;
        input.update(move,head_degrees*radians+core.movement_heading_offset(t),true,t);
        const auto pad=input.snapshot(t).pad.Gamepad;const float x=pad.sThumbLX/32767.f,y=pad.sThumbLY/32767.f;
        // Where the game sends the Marine: x along its camera's right, y along its forward.
        const float world_x=-x*std::cos(n)+y*std::sin(n),world_z=x*std::sin(n)+y*std::cos(n);
        const float f=std::hypot(eye[8],eye[10]),fx=eye[8]/f,fz=eye[10]/f;
        const float expected_x=side?-fz:fx,expected_z=side?fx:fz;
        require(std::abs(world_x-expected_x)<1e-3f && std::abs(world_z-expected_z)<1e-3f,
            "in a takeover the stick moves the Marine along the view (turned "+std::to_string(turned_degrees)+", head "+std::to_string(head_degrees)+
            (side?", right":", forward")+"): got "+std::to_string(world_x)+","+std::to_string(world_z)+" expected "+std::to_string(expected_x)+","+std::to_string(expected_z));
        require(core.movement_heading_offset(t+std::chrono::milliseconds(101))==0,"a stale takeover frame supplies no correction");
    }
}
// Early return (docs/TAKEOVER_RETURN_2026-10-26.md). Joe, 26 Oct: "animations playing for a little
// too long after the attack or Execution ... with the Vanguard doing his launch attack". The game
// hands control back before its camera glides back; the hold waited for the camera, 0.85-1.1 s after
// an execution and 1.4-3.3 s after the grapnel, while the Marine walked away. Now first person comes
// back once the Marine walks along the stick, and holds again if an animation moves it after that.
namespace release {
using namespace takeover;
float units(float metres_per_second){return metres_per_second*S*.033f;} // world units per FRAME
struct Frame {VrBodyFollowing::CameraMode mode{};bool anchored{},released{};vr_camera::Matrix anchor{};};
Frame step(VrBodyFollowing& core,Clock::time_point& t,const Vector& root,const vr_camera::Matrix& game,std::array<float,2> stick){
    t+=FRAME;core.body(1,2,3,root,t);core.stick(stick[0],stick[1],t);const auto anchor=core.camera(game,vr_camera::IDENTITY,1,S,t);const auto s=core.snapshot();
    return {s.camera_mode,s.anchored,s.released,anchor};
}
vr_camera::Matrix out(const Vector& root,float seconds=0){return VrBodyFollowing::pulled_back(native_at(root),root,S,seconds);}
}
// Joe, 31 Oct (docs/ANIMATION_CAMERA.md, option 1): an execution hands back once its animation has run
// and settled, not 1.1-1.2 s after the Marine stopped. The root stops 1.2-1.5 s in while the arms
// keep stabbing for 3-4 s more, so the root, HEAD and HAND bones together decide.
void takeover_settled_return() {
    using namespace release;using M=VrBodyFollowing::CameraMode;
    const auto frame=[](VrBodyFollowing& core,Clock::time_point& t,const Vector& root,const Vector& hand){
        t+=FRAME;core.body(1,2,3,root,t);core.stick(0,0,t);
        core.character_motion({t,1,{root,Vector{root[0],root[1]+2.3f,root[2]},hand,hand}});
        core.camera(out(root),vr_camera::IDENTITY,1,S,t);return core.snapshot();};
    // Lunge, stab with the root still, pause briefly, stab again: all held.
    const auto execution=[&](VrBodyFollowing& core,Clock::time_point& t,Vector& root,Vector& hand){
        for(int i=0;i<15;++i){root[2]-=units(6);hand=root;require(frame(core,t,root,hand).camera_mode==M::takeover,"the lunge holds");}
        for(int i=0;i<45;++i){hand[1]+=(i%10<5?1.f:-1.f)*units(2);require(frame(core,t,root,hand).camera_mode==M::takeover,"the root has stopped but the arms still stab: held");}
        for(int i=0;i<6;++i) require(frame(core,t,root,hand).camera_mode==M::takeover,"a 0.2 s pause mid-execution: held");
        for(int i=0;i<10;++i){hand[1]+=(i%10<5?1.f:-1.f)*units(2);require(frame(core,t,root,hand).camera_mode==M::takeover,"stabbing again: held");}
    };
    {   VrBodyFollowing core;Vector root{0,0,0};auto t=settle(core,root,vr_camera::IDENTITY,Clock::now());Vector hand=root;
        require(core.settings().execution_still_seconds==.3f,"on by default: 0.3 s");
        execution(core,t,root,hand);
        int frames=0;VrBodyFollowing::Snapshot s;
        do{s=frame(core,t,root,hand);++frames;}while(s.camera_mode==M::takeover && frames<60);
        require(s.camera_mode==M::body && s.released && s.releases==1 && s.still_releases==1,"first person once everything has been still for 0.3 s");
        require(frames*FRAME>=std::chrono::milliseconds(300) && frames*FRAME<=std::chrono::milliseconds(300)+vr_viewer::Motion::INTERVAL+3*FRAME,
            "0.3 s after the last movement, not before: "+std::to_string(frames)+" frames");
        core.identify(2);require(core.snapshot().releases==1 && core.snapshot().still_releases==1,"a respawn keeps both counts, one a subset of the other");
    }
    {   // Off: only the stick or the game's camera ends the hold (the 26 Oct rules).
        VrBodyFollowing core;Vector root{0,0,0};auto t=settle(core,root,vr_camera::IDENTITY,Clock::now());Vector hand=root;
        auto settings=core.settings();settings.execution_still_seconds=0;require(core.set_settings(settings),"off");
        execution(core,t,root,hand);
        for(int i=0;i<60;++i) require(frame(core,t,root,hand).camera_mode==M::takeover,"off: still held while the camera is out");
        require(core.snapshot().still_releases==0,"no settled return");
    }
    {   VrBodyFollowing::Settings s;
        for(float v:{-.1f,.05f,2.1f}){auto b=s;b.execution_still_seconds=v;require(!b.valid(),"execution_still_seconds is 0 or [0.1,2]");}
    }
}
void takeover_early_return() {
    using namespace release;using M=VrBodyFollowing::CameraMode;
    constexpr std::array<float,2> neutral{0,0},forward{0,1},right{1,0};
    {   VrBodyFollowing core;Vector root{0,0,0};auto t=settle(core,root,vr_camera::IDENTITY,Clock::now());
        require(core.settings().release_speed_metres_per_second==1.5f && core.settings().release_seconds==.1f,"early return on by default: 1.5 m/s for 0.1 s");
        // The execution: held while the Marine stands, lunges with the stick neutral, or lunges
        // while the stick points elsewhere. None of that is the player walking.
        for(int i=0;i<30;++i) require(step(core,t,root,out(root,i*.033f),neutral).mode==M::takeover,"an execution holds");
        for(int i=0;i<15;++i){root[2]-=units(9);require(step(core,t,root,out(root),neutral).mode==M::takeover,"a lunge with the stick neutral holds");}
        for(int i=0;i<15;++i){root[2]-=units(9);require(step(core,t,root,out(root),right).mode==M::takeover,"a lunge across the stick holds");}
        for(int i=0;i<10;++i) step(core,t,root,out(root),forward); // standing, stick pushed: the animation still plays
        require(core.snapshot().camera_mode==M::takeover && core.snapshot().releases==0,"a pushed stick alone is not control");
        // The game gives control back: the Marine walks along the stick (its camera's forward, -Z)
        // at 6 m/s while that camera is still 3 m out.
        int frames=0;Frame f;
        do {root[2]-=units(6);f=step(core,t,root,out(root),forward);++frames;} while(!f.anchored && frames<30);
        require(f.anchored && f.mode==M::body && f.released && core.snapshot().releases==1,"first person once the Marine walks along the stick");
        require(frames*FRAME<=std::chrono::milliseconds(100)+VrBodyFollowing::TRAVEL_WINDOW+3*FRAME,"within release_seconds of walking: "+std::to_string(frames)+" frames");
        require(close(f.anchor[13],root[1]+2.3f) && std::abs(f.anchor[12]-root[0])<.01f && std::abs(f.anchor[14]-(root[2]-.35f*S))<.01f,"on the Marine, at the standing eye height, not the camera's");
        require(core.yaw(1,t).has_value(),"the mod has the facing again");
        // The camera is still out for a while: first person stays, at the standing height.
        for(int i=0;i<20;++i){root[2]-=units(6);f=step(core,t,root,out(root,i*.033f),forward);
            require(f.mode==M::body && f.released && close(f.anchor[13],root[1]+2.3f),"while the game's camera glides back, first person at the standing height");}
        // It settles (0.15 high): the release ends and the native height comes in through the blend.
        auto back=native_at(root);back[13]+=.15f;const auto settling=t;const float before=f.anchor[13];
        do f=step(core,t,root,back,neutral);while(f.released && t-settling<std::chrono::seconds(2));
        require(!f.released && f.anchored && t-settling<=VrBodyFollowing::SETTLE_TIME+2*FRAME && std::abs(f.anchor[13]-before)<.06f,"settled: plain first person, no cut");
        for(int i=0;i<20;++i) f=step(core,t,root,back,neutral);
        require(std::abs(f.anchor[13]-(root[1]+2.45f))<.005f,"then the native height");
    }
    {   // After a return an animation moves the Marine (a second execution's lunge): the hold comes back,
        // where first person was. Walking that turns more slowly than the stick does not.
        VrBodyFollowing core;Vector root{0,0,0};auto t=settle(core,root,vr_camera::IDENTITY,Clock::now());
        for(int i=0;i<10;++i) step(core,t,root,out(root),neutral);
        Frame f;for(int i=0;i<10;++i){root[2]-=units(6);f=step(core,t,root,out(root),forward);}
        require(f.released,"returned");
        for(int i=0;i<5;++i){root[2]-=units(6);f=step(core,t,root,out(root),right);require(f.released,"a brief lag behind a turned stick keeps first person");}
        for(int i=0;i<3;++i){root[2]-=units(6);f=step(core,t,root,out(root),neutral);require(f.released,"so does stopping");}
        std::optional<vr_camera::Matrix> spot;int frames=0;
        for(int i=0;i<30 && f.mode!=M::takeover;++i,++frames){spot=f.anchor;root[2]-=units(9);f=step(core,t,root,out(root),neutral);}
        require(f.mode==M::takeover && !f.released && frames*FRAME<=VrBodyFollowing::ANIMATED_TIME+VrBodyFollowing::TRAVEL_WINDOW+4*FRAME,"an animation moving the Marine holds again: "+std::to_string(frames));
        require(std::hypot(f.anchor[12]-(*spot)[12],f.anchor[14]-(*spot)[14])<.6f*S,"from where first person was");
    }
    {   // Switched off: the 23 Oct rule, the hold waits for the game's camera.
        VrBodyFollowing core;Vector root{0,0,0};auto t=settle(core,root,vr_camera::IDENTITY,Clock::now());
        auto s=core.settings();s.release_speed_metres_per_second=0;require(core.set_settings(s),"off");
        step(core,t,root,out(root),neutral);
        for(int i=0;i<40;++i){root[2]-=units(6);require(step(core,t,root,out(root),forward).mode==M::takeover,"release 0: walking stays held");}
        require(core.snapshot().releases==0,"no return");
        // No first-person placement to return to (a takeover straight after a cutscene): held.
        VrBodyFollowing fresh;fresh.identify(1);fresh.allow(true);auto u=Clock::now();Vector r{0,0,0};
        for(int i=0;i<40;++i){r[2]-=units(6);require(step(fresh,u,r,out(r),forward).mode==M::takeover,"after a cutscene: held");}
    }
    {   // The knobs: bounded, optional in the camera guard file, round trip.
        VrBodyFollowing::Settings s;
        for(float v:{-.1f,.4f,6.1f}){auto b=s;b.release_speed_metres_per_second=v;require(!b.valid(),"release speed is 0 or [0.5,6]");}
        for(float v:{.02f,.6f}){auto b=s;b.release_seconds=v;require(!b.valid(),"release time is bounded to [0.03,0.5]");}
        const std::string limits="camera_radius_metres=1\ncamera_height_metres=4\n";
        std::istringstream old{limits};const auto defaults=VrBodyFollowing::Settings::read_camera_guard(old);
        require(defaults && defaults->release_speed_metres_per_second==1.5f && defaults->release_seconds==.1f,"a file without the keys keeps the defaults");
        std::istringstream tuned{limits+"release_speed_metres_per_second=0\nrelease_seconds=0.25\n"};const auto loaded=VrBodyFollowing::Settings::read_camera_guard(tuned);
        require(loaded && loaded->release_speed_metres_per_second==0 && loaded->release_seconds==.25f,"the file tunes both (0: off)");
    }
}
// The 26 Oct playtest replayed (tests/fixtures/takeover_return_2026-10-26.txt, cut from
// build/run/20261026_164402_playtest_0186 by scripts/takeover_replay_fixture.py): the root, the
// game's camera and the stick the game was sent, at the trace's own times (30 Hz, interpolated to
// 90 Hz like the game's updates). Window 0: the Vanguard's grapnel at 16:50:40.7, 1: an execution at
// 16:50:55.6, 2: a grapnel at 16:55:28.4 whose pull rode along.
void takeover_replay(const char* path) {
    using Vector=VrBodyFollowing::Vector;
    struct Row {int window{};double t{};Vector root{},game{};float fx{},fz{},sx{},sy{},upm{};std::string mode;};
    std::ifstream in{path};require(in.good(),std::string("replay fixture: ")+path);
    std::vector<Row> rows;std::string line;
    while(std::getline(in,line)) {if(line.empty() || line[0]=='#') continue;std::istringstream s{line};Row r;
        s>>r.window>>r.t>>r.root[0]>>r.root[1]>>r.root[2]>>r.game[0]>>r.game[1]>>r.game[2]>>r.fx>>r.fz>>r.sx>>r.sy>>r.upm>>r.mode;require(bool(s),"fixture row: "+line);rows.push_back(r);}
    struct Result {double held{-1},back{-1},recorded_back{-1};float distance{};uint64_t releases{};bool flipped{},settled{};};
    const auto replay=[&](int window,float release_speed) {
        VrBodyFollowing core;core.identify(1);core.allow(true);auto settings=core.settings();settings.release_speed_metres_per_second=release_speed;require(core.set_settings(settings),"settings");
        const auto start=VrBodyFollowing::Clock::now();Result result;bool was_held=false;
        std::vector<const Row*> w;for(const auto& r:rows) if(r.window==window) w.push_back(&r);
        for(size_t i=0;i+1<w.size();++i) {
            const auto& a=*w[i];const auto& b=*w[i+1];
            if(result.recorded_back<0 && was_held && a.mode=="body") result.recorded_back=a.t;
            if(a.mode!="body") was_held=true;
            for(int k=0;k<3;++k) {
                const float u=k/3.f;const auto mix=[&](float x,float y){return x+(y-x)*u;};
                const double t=a.t+(b.t-a.t)*u;const auto now=start+std::chrono::microseconds(int64_t(t*1e6));
                const Vector root{mix(a.root[0],b.root[0]),mix(a.root[1],b.root[1]),mix(a.root[2],b.root[2])};
                float fx=mix(a.fx,b.fx),fz=mix(a.fz,b.fz);const float n=std::hypot(fx,fz);fx/=n;fz/=n;
                auto game=vr_camera::IDENTITY;game[0]=-fz;game[2]=fx;game[8]=fx;game[10]=fz;
                game[12]=mix(a.game[0],b.game[0]);game[13]=mix(a.game[1],b.game[1]);game[14]=mix(a.game[2],b.game[2]);
                core.body(1,2,3,root,now);core.stick(a.sx,a.sy,now);
                const auto anchor=core.camera(game,vr_camera::IDENTITY,1,a.upm,now);const auto s=core.snapshot();
                const bool held=s.camera_mode==VrBodyFollowing::CameraMode::takeover || s.camera_mode==VrBodyFollowing::CameraMode::ride;
                if(held && result.held<0) result.held=t;
                if(result.held>=0 && result.back<0 && s.anchored) {result.back=t;result.distance=std::hypot(anchor[12]-root[0],anchor[14]-root[2])/a.upm;}
                if(result.back>=0 && held) result.flipped=true;
                if(result.back>=0 && s.anchored && !s.released) result.settled=true;
                result.releases=s.releases;
            }
        }
        return result;
    };
    const auto text=[](const Result& r){return " held "+std::to_string(r.held)+" back "+std::to_string(r.back)+" (recorded "+std::to_string(r.recorded_back)+") at "+std::to_string(r.distance)+" m";};
    // Return off: the replay reproduces the recorded handback (the game's camera settling).
    for(int window:{0,1,2}) {const auto r=replay(window,0);
        require(r.held>0 && r.back>0 && std::abs(r.back-r.recorded_back)<.1,"replay off matches the recorded handback, window "+std::to_string(window)+text(r));}
    // Return on (the default).
    const float speed=VrBodyFollowing::Settings{}.release_speed_metres_per_second;
    const auto grapnel=replay(0,speed),execution=replay(1,speed),rode=replay(2,speed);
    std::cout<<"takeover replay, early return:\n  grapnel"<<text(grapnel)<<"\n  execution"<<text(execution)<<"\n  grapnel that rode"<<text(rode)<<"\n";
    // 16:50:40.7: Joe held the stick forward through the grapnel's landing; the Marine walked 8 m from
    // the held view before the camera let go at 16:50:42.99.
    require(grapnel.releases==1 && grapnel.back<grapnel.recorded_back-1.5 && grapnel.distance<1.5f,"the grapnel returns 1.5 s sooner, near the Marine"+text(grapnel));
    // 16:50:55.6: the execution plays 5.8 s with the stick neutral and holds all of it; Joe pushes the
    // stick at 380.74 (16:51:01.41) and the game's camera started back 0.95 s before the hold let go.
    require(execution.releases==1 && execution.back>380.7 && execution.back<execution.recorded_back-.6 && execution.distance<1.5f,"the execution holds until the stick, then returns 0.6 s sooner"+text(execution));
    require(rode.releases==1 && rode.back<rode.recorded_back-1.5 && rode.distance<2.5f,"the grapnel that rode returns 1.5 s sooner"+text(rode));
    for(const auto* r:{&grapnel,&execution,&rode}) require(!r->flipped && r->settled,"after the return no second hold, and plain first person once the camera settles"+text(*r));
}
// Joe, 31 Oct 16:15 (docs/ANIMATION_CAMERA.md): "three strikes turning into a big one ... have it so
// you stay with the character". The big strike lunges the Marine 2-6 m while the game's camera trails
// up to 1.6 m behind him, past the 1 m envelope, which held the view behind him.
namespace combo {
using namespace takeover;using Snapshot=VrBodyFollowing::Snapshot;using M=VrBodyFollowing::CameraMode;
constexpr uint16_t RB=0x0200,UP=0x0001,LB=0x0100,RS=0x0080;
struct Rig {VrBodyFollowing core;Clock::time_point t{};Vector root{0,0,0};vr_camera::Matrix anchor{};
    Rig(){t=settle(core,root,vr_camera::IDENTITY,Clock::now());}};
// One frame, with the game's camera trailing the root by that many metres.
Snapshot step(Rig& r,float trailing=.39f,uint16_t buttons=0){
    r.t+=FRAME;r.core.body(1,2,3,r.root,r.t);r.core.melee(VrBodyFollowing::melee_press(buttons),r.t);r.core.combo(buttons,r.t);
    auto game=native_at(r.root);game[14]=r.root[2]+trailing*S;
    r.anchor=r.core.camera(game,vr_camera::IDENTITY,1,S,r.t);return r.core.snapshot();
}
void press(Rig& r,uint16_t button){for(int i=0;i<4;++i) step(r,.39f,button);}
// Strikes as Joe's chains ran: about 0.7 s apart.
void strikes(Rig& r,int count,uint16_t button=RB,int idle=17){for(int i=0;i<count;++i){press(r,button);for(int k=0;k<idle;++k) step(r);}}
float eye_from_root(const Rig& r){return std::hypot(r.anchor[12]-r.root[0],r.anchor[14]-r.root[2])/S;}
}
void combo_stays_first_person() {
    using namespace combo;
    require(VrBodyFollowing::Settings{}.combo_camera_radius_metres==2.5f && VrBodyFollowing::Settings{}.combo_strikes==3,"on by default: three strikes, 2.5 m");
    {   // The finisher: 3 s of lunges, the game's camera creeping out to 1.6 m and back. It left the
        // envelope 0.25-0.81 s after the last strike in every recorded one, here 0.7 s. All this is longer
        // than the melee facing hold, which must last: the facing written mid-lunge would bend it.
        Rig r;strikes(r,2);strikes(r,1,RB,5);
        Snapshot s;
        for(int i=0;i<90;++i){r.root[2]-=4*S/90;s=step(r,.9f+.7f*i/89);
            require(s.anchored && s.camera_mode==M::body,"the finisher stays in first person: frame "+std::to_string(i));
            require(eye_from_root(r)<.5f,"on the Marine, however far the game's camera trails: "+std::to_string(eye_from_root(r)));
            require(!r.core.yaw(1,r.t),"the lunge keeps the game's facing: frame "+std::to_string(i));}
        require(s.takeovers==0 && s.combos==1 && s.combo_camera,"counted, and no takeover");
        for(int i=0;i<30;++i) s=step(r,1.6f-1.21f*i/29);
        require(s.anchored && !s.combo_camera && s.combos==1 && s.takeovers==0,"the camera's back: plain first person, one combo");
        for(int i=0;i<40;++i) s=step(r);
        require(r.core.yaw(1,r.t).has_value() && !r.core.snapshot().melee_facing,"the mod has the facing again");
        // The combo has ended: a camera that leaves the envelope now is a takeover.
        require(step(r,1.2f).camera_mode==M::takeover,"later, out of the envelope holds as ever");
    }
    {   // A power strike counts; a held button is one strike.
        Rig r;strikes(r,2);strikes(r,1,UP);
        require(step(r,1.2f).anchored,"RB, RB, D-pad up is a combo");
        Rig h;for(int i=0;i<30;++i) step(h,.39f,RB);
        require(step(h,1.2f).camera_mode==M::takeover,"a held button is one strike");
    }
    const auto held=[](Rig& r,const char* why){const auto first=r.anchor;const auto s=step(r,1.2f);
        require(s.camera_mode==M::takeover && !s.anchored && s.combos==0,std::string("holds at once: ")+why);
        require(std::hypot(r.anchor[12]-first[12],r.anchor[14]-first[14])<1e-3f,std::string("where first person stood: ")+why);};
    {Rig r;strikes(r,2);held(r,"two strikes are not a combo");}
    {Rig r;strikes(r,3,RB,60);held(r,"strikes 2 s apart are not a combo");}
    {Rig r;strikes(r,3);for(int i=0;i<50;++i) step(r);held(r,"the combo was over 1.6 s after its last strike");}
    {Rig r;strikes(r,3);press(r,RS);held(r,"a finisher press: the execution holds");}
    {Rig r;strikes(r,3);press(r,LB);held(r,"a parry: the gun strike holds");}
    {Rig r;strikes(r,3);press(r,LB);press(r,RB);held(r,"a strike after a parry starts a new count (a stray RB during a gun strike)");}
    {Rig r;strikes(r,3);r.core.allow(false);r.core.allow(true);held(r,"losing input clears the combo");}
    {   Rig r;strikes(r,3);auto settings=r.core.settings();settings.combo_camera_radius_metres=0;require(r.core.set_settings(settings),"off");
        held(r,"radius 0 is off");
    }
    {   // Past the wider limit it is no strike's camera (19:24:01 on 30 Oct: 6 m out after three strikes):
        // the usual hold, from where first person stood.
        Rig r;strikes(r,3);Snapshot s;for(int i=0;i<10;++i) s=step(r,1.1f+.1f*i);
        require(s.anchored,"inside the wider limit");
        const auto last=r.anchor;s=step(r,2.6f);
        require(s.camera_mode==M::takeover && !s.combo_camera && std::hypot(r.anchor[12]-last[12],r.anchor[14]-last[14])<1e-3f,"past it the view holds on the spot");
    }
    {   // A press mid-view doesn't drop it: a parry during the lunge is no reason to leave the Marine.
        Rig r;strikes(r,3);step(r,1.2f);
        require(step(r,1.2f,LB).anchored,"a guard raised mid-finisher stays with the Marine");
    }
    {   // Bounded: a camera that never comes back ends in the usual hold.
        Rig r;strikes(r,3);Snapshot s=step(r,1.5f);int frames=0;
        while(s.anchored && frames<300){s=step(r,1.5f);++frames;}
        require(s.camera_mode==M::takeover && std::abs(frames*.033f-5)<.1f,"combo_camera_seconds later: "+std::to_string(frames));
    }
    {   // A new actor never inherits it.
        Rig r;strikes(r,3);r.core.identify(2);r.core.identify(1);
        require(r.core.snapshot().combos==0 && !r.core.snapshot().combo_camera,"nothing counted");
    }
    {   // The knobs: bounded, optional in the camera guard file.
        VrBodyFollowing::Settings s;
        for(float v:{-.1f,1.f,4.1f}){auto b=s;b.combo_camera_radius_metres=v;require(!b.valid(),"combo radius is 0 or [1.2,4]");}
        for(float v:{1.f,9.f}){auto b=s;b.combo_strikes=v;require(!b.valid(),"combo strikes are [2,8]");}
        for(float v:{.4f,3.1f}){auto b=s;b.combo_gap_seconds=v;require(!b.valid(),"combo gap is [0.5,3]");}
        for(float v:{.9f,10.1f}){auto b=s;b.combo_camera_seconds=v;require(!b.valid(),"combo time is [1,10]");}
        std::istringstream tuned{"camera_radius_metres=1\ncamera_height_metres=4\ncombo_camera_radius_metres=0\ncombo_strikes=4\ncombo_gap_seconds=1\ncombo_camera_seconds=6\n"};
        const auto loaded=VrBodyFollowing::Settings::read_camera_guard(tuned);
        require(loaded && loaded->combo_camera_radius_metres==0 && loaded->combo_strikes==4 && loaded->combo_gap_seconds==1 && loaded->combo_camera_seconds==6,"the file tunes them");
    }
}
// The 31 Oct playtest replayed (tests/fixtures/combo_finisher_2026-10-31.txt, cut from
// build/run/20261031_160438_playtest_1604 by scripts/takeover_replay_fixture.py --buttons): the root,
// the game's camera and the buttons the game was sent, at the trace's own times. Windows 0, 1 and 4:
// the finishers at 16:14:53.9, 16:15:06.1 and 16:17:36.3; 3: a blip at 16:15:36.1 and a finisher at
// 16:15:40.0; 2 and 7: executions at 16:15:14.5 (after two strikes) and 16:20:03.9; 5: a gun strike at
// 16:19:42.3 with a stray RB after its shot; 6: a parry at 16:16:37.4.
void combo_replay(const char* path) {
    using Vector=VrBodyFollowing::Vector;using M=VrBodyFollowing::CameraMode;
    struct Row {int window{};double t{};Vector root{},game{};float fx{},fz{},sx{},sy{},upm{};std::string mode;unsigned buttons{};};
    std::ifstream in{path};require(in.good(),std::string("replay fixture: ")+path);
    std::vector<Row> rows;std::string line;
    while(std::getline(in,line)) {if(line.empty() || line[0]=='#') continue;std::istringstream s{line};Row r;
        s>>r.window>>r.t>>r.root[0]>>r.root[1]>>r.root[2]>>r.game[0]>>r.game[1]>>r.game[2]>>r.fx>>r.fz>>r.sx>>r.sy>>r.upm>>r.mode>>std::hex>>r.buttons;require(bool(s),"fixture row: "+line);rows.push_back(r);}
    struct Result {double held{-1},recorded{-1};float furthest{};uint64_t takeovers{},combos{};};
    const auto replay=[&](int window,float radius) {
        VrBodyFollowing core;core.identify(1);core.allow(true);auto settings=core.settings();settings.combo_camera_radius_metres=radius;require(core.set_settings(settings),"settings");
        const auto start=VrBodyFollowing::Clock::now();Result result;
        std::vector<const Row*> w;for(const auto& r:rows) if(r.window==window) w.push_back(&r);
        require(w.size()>30,"window "+std::to_string(window));
        for(size_t i=0;i+1<w.size();++i) {
            const auto& a=*w[i];const auto& b=*w[i+1];
            if(result.recorded<0 && a.mode=="takeover") result.recorded=a.t;
            for(int k=0;k<3;++k) {
                const float u=k/3.f;const auto mix=[&](float x,float y){return x+(y-x)*u;};
                const double t=a.t+(b.t-a.t)*u;const auto now=start+std::chrono::microseconds(int64_t(t*1e6));
                const Vector root{mix(a.root[0],b.root[0]),mix(a.root[1],b.root[1]),mix(a.root[2],b.root[2])};
                float fx=mix(a.fx,b.fx),fz=mix(a.fz,b.fz);const float n=std::hypot(fx,fz);fx/=n;fz/=n;
                auto game=vr_camera::IDENTITY;game[0]=-fz;game[2]=fx;game[8]=fx;game[10]=fz;
                game[12]=mix(a.game[0],b.game[0]);game[13]=mix(a.game[1],b.game[1]);game[14]=mix(a.game[2],b.game[2]);
                core.body(1,2,3,root,now);core.stick(a.sx,a.sy,now);
                core.melee(VrBodyFollowing::melee_press(uint16_t(a.buttons)),now);core.combo(uint16_t(a.buttons),now);
                const auto anchor=core.camera(game,vr_camera::IDENTITY,1,a.upm,now);const auto s=core.snapshot();
                if(s.camera_mode==M::takeover && result.held<0) result.held=t;
                result.furthest=std::max(result.furthest,std::hypot(anchor[12]-root[0],anchor[14]-root[2])/a.upm);
                result.takeovers=s.takeovers;result.combos=s.combos;
            }
        }
        return result;
    };
    const auto text=[](const Result& r){return " held "+std::to_string(r.held)+" (recorded "+std::to_string(r.recorded)+"), "+std::to_string(r.furthest)+" m from the Marine, "+std::to_string(r.combos)+" combos";};
    std::cout<<"combo replay:\n";
    for(int window:{0,1,3,4}) {
        // Off, the replay reproduces Joe's report: the same hold, and the view left behind the Marine.
        const auto off=replay(window,0);
        require(std::abs(off.held-off.recorded)<.1 && off.furthest>1.5f,"off reproduces the recorded hold, window "+std::to_string(window)+text(off));
        const auto on=replay(window,VrBodyFollowing::Settings{}.combo_camera_radius_metres);
        std::cout<<"  finisher "<<window<<":"<<text(off)<<" -> "<<text(on)<<"\n";
        require(on.takeovers==0 && on.combos>=1 && on.furthest<.6f,"the finisher stays on the Marine, window "+std::to_string(window)+text(on));
    }
    for(int window:{2,5,6,7}) {
        const auto on=replay(window,VrBodyFollowing::Settings{}.combo_camera_radius_metres),off=replay(window,0);
        std::cout<<"  takeover "<<window<<":"<<text(on)<<"\n";
        require(on.takeovers>=1 && on.combos==0 && on.held==off.held && std::abs(on.held-on.recorded)<.1,"executions, gun strikes and parries hold exactly as before, window "+std::to_string(window)+text(on));
    }
}
// Riding along (docs/ANIMATION_CAMERA.md, 26 Oct): the jump pack moves the Marine at 15-20 m/s
// (26 Oct trace: the dash 10.6 m in 0.65 s, its camera 2.3 m behind; the leap's camera 4.5 m back
// and up). In a takeover, above ride_speed_metres_per_second, horizontally or upwards, the eye
// rides on the Marine: at its standing eye height over the root, the head free, the facing the
// game's, a held view catching up without a cut. Executions, falls and a takeover with no first
// person before it still hold; the landing hands back to first person without a cut either.
namespace ride {
using namespace takeover;
float units(float metres_per_second){return metres_per_second*S*.033f;} // world units per FRAME
struct Frame {VrBodyFollowing::CameraMode mode{};bool anchored{},riding{};vr_camera::Matrix anchor{};float vignette{};};
Frame step(VrBodyFollowing& core,Clock::time_point& t,const Vector& root,const vr_camera::Matrix& game,const vr_camera::Matrix& head){
    t+=FRAME;core.body(1,2,3,root,t);const auto anchor=core.camera(game,head,1,S,t);const auto s=core.snapshot();
    return {s.camera_mode,s.anchored,s.riding,anchor,s.vignette};
}
float horizontal(const vr_camera::Matrix& a,const vr_camera::Matrix& b){return std::hypot(a[12]-b[12],a[14]-b[14]);}
}
void jump_pack_rides_along() {
    using namespace ride;
    const auto head=yawed_head(30);
    {   // The dash: the game's camera falls behind at once (a takeover holds for a few frames), then
        // the speed is measured and the eye catches up with the Marine and rides along.
        VrBodyFollowing core;Vector root{0,0,0};auto t=settle(core,root,head,Clock::now());
        const auto first_person=core.camera(native_at(root),head,1,S,t);
        const float offset_x=first_person[12]-root[0],offset_z=first_person[14]-root[2];
        require(core.settings().ride_enabled && core.settings().ride_speed_metres_per_second==13 && core.snapshot().rides==0,"riding along is on by default, from 13 m/s");
        std::optional<int> ride_from;float gap_max=0,catch_up_max=0;bool held=false;vr_camera::Matrix previous=first_person;
        for(int i=1;i<=30;++i) {
            const Vector before=root;root[2]-=units(16);
            auto game=native_at(root);game[14]+=std::min(1.5f+.1f*i,2.3f)*S; // the dash camera, 1.5-2.3 m behind
            const auto f=step(core,t,root,game,head);
            const Vector placed{root[0]+offset_x,0,root[2]+offset_z};
            const float gap=std::hypot(f.anchor[12]-placed[0],f.anchor[14]-placed[2]);
            if(f.mode==VrBodyFollowing::CameraMode::takeover) {held=true;gap_max=std::max(gap_max,gap);require(!ride_from,"once riding, the dash never drops back to a held view");}
            if(f.riding && !ride_from) ride_from=i;
            if(ride_from) {
                require(f.mode==VrBodyFollowing::CameraMode::ride && !f.anchored && !core.yaw(1,t) && !core.snapshot().show_head,"riding: the takeover's eye is on the Marine, the game keeps the facing, the head stays hidden");
                require(same_rotation(f.anchor,first_person),"riding: no forced yaw or roll, only the real head turns the view");
                // How much further the eye moves in one frame than the Marine does (a cut would be the whole gap).
                catch_up_max=std::max(catch_up_max,horizontal(f.anchor,previous)-std::hypot(root[0]-before[0],root[2]-before[2]));
                if(i>=*ride_from+10) require(gap<.1f && close(f.anchor[13],2.3f),"caught up: the eye rides on the Marine at the standing eye height, gap="+std::to_string(gap));
            }
            previous=f.anchor;
        }
        require(held && ride_from && *ride_from<=3 && core.snapshot().rides==1,"the camera left first: a hold of a frame or two (TRAVEL_WINDOW), then one ride");
        require(gap_max>.4f && catch_up_max<.5f*gap_max,"the held view rejoins the Marine without a cut: held "+std::to_string(gap_max)+", largest catch-up step "+std::to_string(catch_up_max));
        require(core.snapshot().travel_speed>15 && core.snapshot().travel_speed<17,"the snapshot reports the dash's speed");
        // It stops; the game's camera comes back to the head, a little high. The takeover ends as
        // any does (settled for SETTLE_TIME), and first person takes the native height through
        // the blend, not in a cut, and the mod has the facing again.
        auto back=native_at(root);back[13]+=.15f;
        Frame f;const auto stopped=t;
        do f=step(core,t,root,back,head);while(!f.anchored && t-stopped<std::chrono::seconds(1));
        require(f.anchored && !f.riding && f.mode==VrBodyFollowing::CameraMode::body && t-stopped<=VrBodyFollowing::SETTLE_TIME+2*FRAME,"the takeover ends once the camera has settled");
        require(f.anchor[13]<2.3f+.06f && std::abs(f.anchor[14]-previous[14])<.1f,"no cut at the handback: "+std::to_string(f.anchor[13]));
        for(int i=0;i<20;++i) f=step(core,t,root,back,head);
        require(std::abs(f.anchor[13]-2.45f)<.005f && core.yaw(1,t).has_value(),"then the native height, and the mod writes the facing again");
        // In first person (the camera inside the envelope: a dodge, a melee lunge, a sprint) the
        // same speed is plain first person: no ride, the mod keeps the facing, no vignette.
        for(int i=0;i<20;++i){root[2]-=units(16);const float fading=f.vignette;f=step(core,t,root,native_at(root),head);
            require(f.mode==VrBodyFollowing::CameraMode::body && f.anchored && !f.riding && f.vignette<=fading && core.yaw(1,t),"fast first person is not a ride (the last ride's vignette only fades)");}
        require(core.snapshot().rides==1 && f.vignette<.01f,"still the one ride, and no vignette");
        // Joe, 26 Oct: the gaze steers the native camera during a ride (the ground pound's landing
        // marker follows the gaze in the air), as in anchored first person, and never in a hold.
        {using M=VrBodyFollowing::CameraMode;
         require(VrBodyFollowing::gaze_steers(M::ride,false) && VrBodyFollowing::gaze_steers(M::body,true),"gaze steers in a ride and in anchored first person");
         require(!VrBodyFollowing::gaze_steers(M::takeover,false) && !VrBodyFollowing::gaze_steers(M::takeover,true) && !VrBodyFollowing::gaze_steers(M::body,false) &&
             !VrBodyFollowing::gaze_steers(M::scripted,true) && !VrBodyFollowing::gaze_steers(M::external,true) && !VrBodyFollowing::gaze_steers(M::unavailable,true),
             "never in a hold, a cutscene, an external camera or an unanchored body");}
    }
    {   // The leap: up at 14 m/s (6 across), over the apex (3 m/s for 0.2 s, inside RIDE_SETTLE), the
        // ground pound down at 20 m/s, the landing with the jump pack camera still 4.5 m back and up.
        VrBodyFollowing core;{auto tuned=core.settings();tuned.ride_vignette=.5f;require(core.set_settings(tuned),"a vignette tuned on (off by default)");}
        Vector root{0,0,0};auto t=settle(core,root,head,Clock::now());
        const auto jetpack=[&]{auto g=native_at(root);g[14]+=4.5f*S;g[13]+=2*S;return g;};
        float vignette_peak=0;
        const auto fly=[&](int frames,float across,float up,const char* phase){
            for(int i=0;i<frames;++i){root[2]-=units(across);root[1]+=units(up);const auto f=step(core,t,root,jetpack(),head);vignette_peak=std::max(vignette_peak,f.vignette);
                if(core.snapshot().rides) require(f.riding && f.mode==VrBodyFollowing::CameraMode::ride,std::string("riding through the ")+phase);}};
        fly(15,6,14,"rise");
        {const float h=core.camera(jetpack(),head,1,S,t)[13]-(root[1]+2.3f);require(core.snapshot().riding && std::abs(h)<.05f,"rising: the eye goes up with the Marine, at its standing eye height: "+std::to_string(h));}
        require(vignette_peak>.45f && vignette_peak<=core.settings().ride_vignette,"fast flight brings in the comfort vignette at ride_vignette: "+std::to_string(vignette_peak));
        fly(6,3,0,"apex");fly(10,4,-20,"ground pound");
        require(core.snapshot().rides==1 && std::abs(core.camera(jetpack(),head,1,S,t)[13]-(root[1]+2.3f))<.01f,"one ride from take-off to the slam, the eye on the Marine");
        // Landed, the camera still pulled back: the ride ends and the view holds where it landed.
        Frame f;const auto landed=t;
        do f=step(core,t,root,jetpack(),head);while(f.mode!=VrBodyFollowing::CameraMode::takeover && t-landed<std::chrono::seconds(1));
        require(f.mode==VrBodyFollowing::CameraMode::takeover && !f.riding,"landed under the pulled-back camera: a hold");
        const auto spot=f.anchor;
        for(int i=0;i<15;++i){f=step(core,t,root,jetpack(),head);require(horizontal(f.anchor,spot)<1e-4f && close(f.anchor[13],spot[13]),"the hold keeps its place");}
        require(f.vignette<.02f,"and the vignette has gone: "+std::to_string(f.vignette));
        // The camera comes back (0.15 high): first person without a cut, then the native height.
        auto back=native_at(root);back[13]+=.15f;
        do f=step(core,t,root,back,head);while(!f.anchored && t-landed<std::chrono::seconds(3));
        require(f.anchored && std::abs(f.anchor[13]-spot[13])<.06f && horizontal(f.anchor,spot)<.01f,"a landing hands back to first person without a cut");
        for(int i=0;i<20;++i) f=step(core,t,root,back,head);
        require(std::abs(f.anchor[13]-(root[1]+2.45f))<.005f && f.vignette==0,"then first person as usual");
    }
    {   // An execution: a 10 m/s lunge into it, then the camera pulls back and the root creeps.
        // Executions, gun strikes and rock kicks still hold the view in place.
        VrBodyFollowing core;Vector root{0,0,0};auto t=settle(core,root,head,Clock::now());
        for(int i=0;i<10;++i){root[2]-=units(10);step(core,t,root,native_at(root),head);}
        std::optional<vr_camera::Matrix> spot;
        for(int i=0;i<60;++i){root[2]-=units(1);const auto f=step(core,t,root,VrBodyFollowing::pulled_back(native_at(root),root,S,i*.033f),head);
            require(f.mode==VrBodyFollowing::CameraMode::takeover && !f.riding && f.vignette==0,"an execution is a hold, never a ride");
            if(!spot) spot=f.anchor;require(horizontal(f.anchor,*spot)<1e-4f,"its view stays where first person stood");}
        require(core.snapshot().rides==0,"no ride");
        // A fall (a ledge drop at 20 m/s down) holds too: only horizontal or upward travel rides.
        for(int i=0;i<15;++i){root[1]-=units(20);root[2]-=units(2);require(step(core,t,root,VrBodyFollowing::pulled_back(native_at(root),root,S,0),head).mode==VrBodyFollowing::CameraMode::takeover,"a fall is held");}
        require(core.snapshot().rides==0,"a fall never rides");
    }
    {   // Switched off: the dash is the old hold. With no first-person view before it (after a
        // cutscene or a load), a fast takeover holds too.
        VrBodyFollowing off;Vector root{0,0,0};auto t=settle(off,root,head,Clock::now());auto settings=off.settings();settings.ride_enabled=false;require(off.set_settings(settings),"ride switch");
        for(int i=0;i<30;++i){root[2]-=units(16);auto g=native_at(root);g[14]+=2*S;require(step(off,t,root,g,head).mode==VrBodyFollowing::CameraMode::takeover,"ride_enabled=false: the dash holds");}
        VrBodyFollowing fresh;fresh.identify(1);fresh.allow(true);auto u=Clock::now();Vector r{0,0,0};
        for(int i=0;i<30;++i){r[2]-=units(16);auto g=native_at(r);g[14]+=2*S;require(step(fresh,u,r,g,head).mode==VrBodyFollowing::CameraMode::takeover,"no first-person placement: held");}
        require(fresh.snapshot().rides==0 && off.snapshot().rides==0,"neither rides");
    }
    {   // The knobs: bounded, optional in the camera guard file (older files load with the defaults),
        // and live (a higher ride speed keeps the dash a hold; vignette 0 never shades the view).
        VrBodyFollowing::Settings s;
        for(float v:{7.9f,30.1f}){auto b=s;b.ride_speed_metres_per_second=v;require(!b.valid(),"ride speed is bounded to [8,30]");}
        for(float v:{-.1f,1.1f}){auto b=s;b.ride_vignette=v;require(!b.valid(),"vignette is bounded to [0,1]");}
        const std::string limits="camera_radius_metres=1\ncamera_height_metres=4\n";
        std::istringstream old{limits};const auto defaults=VrBodyFollowing::Settings::read_camera_guard(old);
        require(defaults && defaults->ride_speed_metres_per_second==13 && defaults->ride_vignette==0,"a file without the keys keeps the defaults (vignette off, Joe 26 Oct)");
        std::istringstream tuned{limits+"ride_speed_metres_per_second=20\nride_vignette=0\n"};const auto loaded=VrBodyFollowing::Settings::read_camera_guard(tuned);
        require(loaded && loaded->ride_speed_metres_per_second==20 && loaded->ride_vignette==0,"the file tunes both");
        std::istringstream bad{limits+"ride_vignette=2\n"};require(!VrBodyFollowing::Settings::read_camera_guard(bad),"an out-of-range value rejects the file");
        const auto path=std::filesystem::temp_directory_path()/("sm2vr-ride-"+std::to_string(GetCurrentProcessId())+".calibration");
        require(loaded->save_camera_guard(path),"save");const auto again=VrBodyFollowing::Settings::load_camera_guard(path);std::filesystem::remove(path);
        require(again && again->ride_speed_metres_per_second==20 && again->ride_vignette==0,"and they round trip through the file");
        VrBodyFollowing core;Vector root{0,0,0};auto t=settle(core,root,head,Clock::now());auto live=core.settings();live.ride_speed_metres_per_second=20;live.ride_vignette=0;require(core.set_settings(live),"live");
        for(int i=0;i<30;++i){root[2]-=units(16);auto g=native_at(root);g[14]+=2*S;require(step(core,t,root,g,head).mode==VrBodyFollowing::CameraMode::takeover,"under a 20 m/s ride speed the dash holds");}
        live.ride_speed_metres_per_second=13;core.set_settings(live);float peak=0;
        for(int i=0;i<30;++i){root[2]-=units(16);auto g=native_at(root);g[14]+=2*S;const auto f=step(core,t,root,g,head);peak=std::max(peak,f.vignette);}
        require(core.snapshot().riding && peak==0,"riding with ride_vignette 0: no vignette");
        // A teleport (a respawn on the same body) is not travel: no ride, no vignette.
        VrBodyFollowing jump;Vector r{0,0,0};auto u=settle(jump,r,head,Clock::now());
        r[0]+=40;const auto f=step(jump,u,r,native_at(r),head);
        require(!f.riding && f.vignette==0 && jump.snapshot().rides==0,"a teleport never rides");
    }
}
// The shipped camera guard file loads, with the ride knobs at their defaults.
void camera_guard_file(const char* path) {
    const auto loaded=VrBodyFollowing::Settings::load_camera_guard(path);
    const VrBodyFollowing::Settings defaults;
    require(loaded.has_value(),std::string("config/index_camera_guard.calibration loads: ")+path);
    for(const auto& p:VrBodyFollowing::Settings::CAMERA_PARAMETERS) require((*loaded).*p.field==defaults.*p.field,std::string("the shipped file keeps the default ")+p.name);
    std::ifstream text{path};std::string all{std::istreambuf_iterator<char>{text},{}};
    for(const auto& p:VrBodyFollowing::Settings::CAMERA_PARAMETERS) require(all.find(std::string(p.name)+"=")!=std::string::npos,std::string("the shipped file lists ")+p.name);
}
// Slopes (docs/BODY_CLIPPING.md): the game's camera trails the root's height, so the anchor
// leads it by height_lead_seconds times the root's vertical speed.
void height_lead_on_slopes() {
    using namespace takeover;
    VrBodyFollowing core;Vector root{0,0,0};const auto head=vr_camera::IDENTITY;
    auto t=settle(core,root,head,Clock::now());
    const float tau=core.settings().height_lead_seconds;require(tau>0,"the height lead is on by default");
    require(core.snapshot().height_lead==0 && close(core.camera(native_at(root),head,1,S,t)[13],root[1]+2.3f),"standing still: no lead, native height");
    // Climb at 0.5 units/s; the native camera sits tau*0.5 low, as measured on 21 Oct.
    const float dt=std::chrono::duration<float>(FRAME).count();
    for(int i=0;i<60;++i,t+=FRAME) {
        root[1]+=.5f*dt;core.body(1,2,3,root,t);
        auto game=native_at(root);game[13]-=tau*.5f;
        const auto anchor=core.camera(game,head,1,S,t);
        if(i>=12) require(std::abs(anchor[13]-(root[1]+2.3f))<1e-3f && core.snapshot().height_lead>0,
            "climbing: the anchor leads the lagging native height back to the standing eye height: "+std::to_string(anchor[13]-root[1]));
    }
    // Descend at 0.5 units/s: the native camera sits high, the lead is negative.
    for(int i=0;i<60;++i,t+=FRAME) {
        root[1]-=.5f*dt;core.body(1,2,3,root,t);
        auto game=native_at(root);game[13]+=tau*.5f;
        const auto anchor=core.camera(game,head,1,S,t);
        if(i>=12) require(std::abs(anchor[13]-(root[1]+2.3f))<1e-3f && core.snapshot().height_lead<0,"descending: negative lead");
    }
    // A fast rise (stairs at 3 units/s, the camera trailing tau*3) is capped at HEIGHT_LEAD_LIMIT.
    for(int i=0;i<30;++i,t+=FRAME) {
        root[1]+=3*dt;core.body(1,2,3,root,t);
        auto game=native_at(root);game[13]-=tau*3;
        const auto anchor=core.camera(game,head,1,S,t);
        if(i>=12) require(close(anchor[13]-(game[13]),VrBodyFollowing::HEIGHT_LEAD_LIMIT*S),"the lead is capped at 0.3 m");
    }
    // 24 Oct elevator: 1.5 units/s down with the camera riding along (no lag). Before, the lead hit
    // its 0.3 m cap and put the eye into the armour; now the eye stays at the native height.
    for(int i=0;i<60;++i,t+=FRAME) {
        root[1]-=1.5f*dt;core.body(1,2,3,root,t);
        const auto anchor=core.camera(native_at(root),head,1,S,t);
        const float margin=VrBodyFollowing::HEIGHT_LAG_MARGIN*S;
        if(i>=12) require(std::abs(anchor[13]-(root[1]+2.3f))<=margin+2e-3f && std::abs(core.snapshot().height_lead)<=margin+2e-3f,"an elevator carries the camera too: at most the 5 cm margin, not 0.3 m: "+std::to_string(core.snapshot().height_lead));
    }
    // A camera trailing less than the speed predicts gets only its actual lag back.
    for(int i=0;i<60;++i,t+=FRAME) {
        root[1]+=.5f*dt;core.body(1,2,3,root,t);
        auto game=native_at(root);game[13]-=.03f;
        const auto anchor=core.camera(game,head,1,S,t);
        if(i>=12) require(core.snapshot().height_lead>0 && core.snapshot().height_lead<=.03f+VrBodyFollowing::HEIGHT_LAG_MARGIN*S+2e-3f && core.snapshot().height_lead<tau*.5f,"the lead never exceeds the camera's lag plus the margin");
    }
    require(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::body && core.snapshot().anchored,"first person throughout");
    auto settings=core.settings();settings.height_lead_seconds=0;require(core.set_settings(settings),"zero lead is valid");
    for(int i=0;i<12;++i,t+=FRAME) {root[1]+=3*dt;core.body(1,2,3,root,t);}
    require(close(core.camera(native_at(root),head,1,S,t)[13],root[1]+2.3f) && core.snapshot().height_lead==0,"lead off: the native height as before");
    settings.height_lead_seconds=.6f;require(!core.set_settings(settings),"a lead over 0.5 s is rejected");
}
void native_cinematic_state() {
    EngineCinematicState reader;std::array<uint32_t,2> flags{};
    require(!reader.authority(),"unbound state reader must report unknown, not gameplay");
    require(reader.bind(reinterpret_cast<uintptr_t>(flags.data()),reader.BUILD_TIMESTAMP,SYNTH(EngineCinematicState::GETTER)),"guarded fixture state storage binds");
    require(reader.authority()==false,"ordinary native flags select gameplay");
    // First distinct samples of the passive Avarax capture, 2026-10-14 18:40-18:48.
    // UI observations independently establish the 0x810 cinematic and 0x0 return.
    // Keep startup/loading/menu bits from becoming false scripted-camera signals.
    struct Recorded {uint32_t mask;bool scripted,paused;};
    constexpr Recorded recorded[]{{0,false,false},{0x1400,false,true},{0x1800,false,false},
        {0x800,false,false},{0,false,false},{0x1400,false,true},{0xc00,false,true},
        {0,false,false},{0x810,true,false},{0,false,false}};
    for(const auto& row:recorded) {
        flags={row.mask,row.mask};const auto state=reader.sample();
        require(state.available && state.scripted==row.scripted && state.paused==row.paused,
            "recorded native startup/menu/campaign flags must classify only the observed cinematic");
    }
    flags[1]=reader.CINEMATIC;require(reader.authority()==true,"pending cinematic start must release close shots before publication");
    flags={reader.CINEMATIC,0};require(reader.authority()==true,"published cinematic remains until engine handover finishes");
    flags={0,reader.PAUSED};require(reader.authority()==true,"pause retains an observed scripted shot when native cinematic bit clears");
    flags={reader.PAUSED,0};require(reader.authority()==true,"published pause also retains scripted ownership");
    flags={0,0};require(reader.authority()==false,"unpaused native stop returns gameplay without manual recenter");
    flags={reader.PAUSED,0};require(reader.authority()==false,"pausing gameplay cannot fabricate a cinematic");
    const auto canary=flags;reader.sample();require(flags==canary,"state observer must never modify native flags");
    auto changed=SYNTH(EngineCinematicState::GETTER);changed[5]^=1;
    require(!reader.bind(reinterpret_cast<uintptr_t>(flags.data()),reader.BUILD_TIMESTAMP,changed) && !reader.authority(),"changed getter rejects and drops old binding");
    require(!reader.bind(reinterpret_cast<uintptr_t>(flags.data()),0,SYNTH(EngineCinematicState::GETTER)),"unknown build rejects");
    require(reader.bind(1,reader.BUILD_TIMESTAMP,SYNTH(EngineCinematicState::GETTER)) && !reader.authority(),"unreadable state fails closed without native calls");
}
void close_scripted_handover() {
    const auto now=VrBodyFollowing::Clock::now();VrBodyFollowing core;core.identify(1);core.allow(true);native_cutscenes(core);core.body(1,2,3,{0,0,0},now);
    auto game=vr_camera::IDENTITY;game[10]=-1;game[12]=.1f;game[13]=2.5f;game[14]=-.2f;
    auto head=vr_camera::IDENTITY;core.camera(game,head,1,.9f,now);
    core.camera_authority(true);require(!core.snapshot().anchored && !core.yaw(1,now),"native scripted state releases facing before next camera render");
    head={0,0,1,0,0,1,0,0,-1,0,0,0,0,0,0,1};
    require(core.camera(game,head,1,.9f,now,true)==game && core.snapshot().camera_mode==VrBodyFollowing::CameraMode::scripted,
        "a close scripted shot inside geometric limits must retain its native origin");
    require(core.camera(game,head,2,.9f,now,true)==game,"recenter during close scripted shot cannot reacquire the body");
    const auto anchor=core.camera(game,head,2,.9f,now,false);vr_camera::Matrix world{},view{};
    require(vr_camera::compose(anchor,head,.9f,world,view) && close(world[12],game[12]) && close(world[14],game[14]) && close(world[8],game[8]) && close(world[10],game[10]),
        "native cinematic end restores close gameplay position/heading without another recenter");
    core.camera(game,head,2,.9f,now,std::nullopt);require(!core.snapshot().anchored && !core.yaw(1,now),"unknown native state cannot restore body control");
    core.identify(0);require(core.camera(game,head,2,.9f,now,true)==game && core.snapshot().camera_mode==VrBodyFollowing::CameraMode::scripted,
        "scripted ownership must work even when the TPS actor disappears");
}
void scripted_heading_sweep() {
    // Exercise return in arbitrary native headings and accumulated physical/snap yaw,
    // keeping the XR reference fixed throughout. Both eyes call this same provider.
    const auto now=VrBodyFollowing::Clock::now();
    for(int native=-180;native<=180;native+=45) for(int tracked=-180;tracked<=180;tracked+=15) {
        const float n=native/57.295779513f,h=tracked/57.295779513f;
        VrBodyFollowing core;core.identify(1);core.allow(true);native_cutscenes(core);core.body(1,2,3,{10,0,20},now);
        auto game=vr_camera::IDENTITY;game[0]=std::cos(n);game[2]=std::sin(n);
        game[8]=std::sin(n);game[10]=-std::cos(n);game[12]=10.1f;game[13]=2.5f;game[14]=19.8f;
        auto head=vr_camera::IDENTITY;core.camera(game,head,7,.9f,now);
        head[0]=head[10]=std::cos(h);head[2]=std::sin(h);head[8]=-std::sin(h);
        EngineCinematicState reader;std::array<uint32_t,2> flags{0x810,0x810};
        require(reader.bind(reinterpret_cast<uintptr_t>(flags.data()),reader.BUILD_TIMESTAMP,SYNTH(EngineCinematicState::GETTER)),"recorded native cinematic storage binds");
        core.camera(game,head,7,.9f,now,reader.authority());
        // Native ownership can change during a sequence/load, before gameplay resumes.
        core.identify(4);core.body(4,5,6,{10,0,20},now);
        auto shot=game;shot[12]+=.2f;
        require(core.camera(shot,head,7,.9f,now,reader.authority())==shot && !core.yaw(4,now),
            "close shots remain native across head turns and owner changes");
        flags={0,0}; // Observed native handover at 18:47:55; no XR recenter.
        const auto anchor=core.camera(game,head,7,.9f,now,reader.authority());
        vr_camera::Matrix world{},view{};
        require(vr_camera::compose(anchor,head,.9f,world,view),"reacquired camera must compose");
        for(int i:{0,2,8,10,12,13,14}) require(close(world[i],game[i]),
            "automatic return must preserve native heading/placement at every tracked yaw without XR recenter");
        require(core.snapshot().anchored && core.yaw(4,now) && core.camera(game,head,7,.9f,now,false)==anchor,
            "facing and repeated eye sampling resume on one stable gameplay anchor");
    }
}
// Sprint camera confirmation (docs/SPRINT_CAMERA.md): horizontal native body speed over
// >=100 ms windows, in world units/second; vertical motion and per-update jitter ignored.
// Joe, 24 Oct: a story cutscene holds the view at the spot where it triggered (the player's body
// is moved out of the scene, 530 units away on 24 Oct), fixed for the whole cutscene.
void cutscene_holds_trigger_spot() {
    using namespace takeover;
    VrBodyFollowing core;Vector root{0,0,0};auto head=vr_camera::IDENTITY;
    auto t=settle(core,root,head,Clock::now());
    const auto spot=core.camera(native_at(root),head,1,S,t);
    require(core.snapshot().anchored && !core.cutscene_holds(),"first person before the cutscene");
    // The cutscene: the body is parked far away and the cinematic camera flies around.
    root={0,-530,40};core.body(1,2,3,root,t);
    auto cinematic=native_at({30,5,-20});
    for(int i=0;i<40;++i,t+=FRAME) {
        cinematic[12]+=.3f;cinematic[13]+=.05f;
        const auto anchor=core.camera(cinematic,head,1,S,t,true);
        require(anchor!=cinematic && close(anchor[12],spot[12]) && close(anchor[13],spot[13]) && close(anchor[14],spot[14]) && same_rotation(anchor,spot),
            "the view stays on the trigger spot, height and heading, whatever the cinematic camera does");
        require(core.snapshot().camera_mode==VrBodyFollowing::CameraMode::scripted && core.snapshot().cutscene_hold && core.snapshot().show_head && !core.snapshot().anchored,"a held cutscene");
    }
    require(core.cutscene_holds()==1,"one hold per cutscene");
    // Body ticks during the cutscene (camera_authority) and an ownership change keep the hold.
    core.camera_authority(true);core.identify(4);core.body(4,5,6,root,t);
    require(close(core.camera(cinematic,head,1,S,t,true)[12],spot[12]) && core.cutscene_holds()==1,"ownership changing mid-sequence keeps the spot");
    // Only the real head moves the view.
    auto turned=head;turned[12]=.2f;const auto moved=core.camera(cinematic,turned,1,S,t,true);
    vr_camera::Matrix eye{},view{};require(vr_camera::compose(moved,turned,S,eye,view) && close(moved[12],spot[12]),"head motion is applied by the eyes, not by moving the anchor");
    // Gameplay again: first person re-anchors on the character.
    root={5,0,5};core.identify(1);core.body(1,2,3,root,t);
    for(int i=0;i<12;++i,t+=FRAME){core.body(1,2,3,root,t);core.camera(native_at(root),head,1,S,t,false);}
    require(core.snapshot().anchored && !core.snapshot().cutscene_hold,"after the cutscene first person resumes on the character");
    // A second cutscene uses the new spot.
    const auto spot2=core.camera(native_at(root),head,1,S,t);
    require(close(core.camera(cinematic,head,1,S,t,true)[12],spot2[12]) && core.cutscene_holds()==2,"each cutscene holds its own trigger spot");
    // No first-person spot in the last 30 s (a cutscene right after a load): native camera.
    t+=std::chrono::seconds(31);core.camera(native_at(root),head,1,S,t,false);
    VrBodyFollowing fresh;fresh.identify(1);fresh.allow(true);fresh.body(1,2,3,root,t);
    require(fresh.camera(cinematic,head,1,S,t,true)==cinematic && !fresh.snapshot().cutscene_hold,"no first-person spot yet: the native cinematic camera");
    VrBodyFollowing stale;auto u=settle(stale,root,head,Clock::now());stale.camera(native_at(root),head,1,S,u);u+=std::chrono::seconds(31);
    require(stale.camera(cinematic,head,1,S,u,true)==cinematic,"a spot older than 30 s is not used");
    // Switched off: the native cinematic camera, as before.
    native_cutscenes(core);require(core.camera(cinematic,head,1,S,t,true)==cinematic,"cutscene_hold=false keeps the native cinematic camera");
}
void ground_speed() {
    const auto start=VrBodyFollowing::Clock::time_point{std::chrono::seconds(50)};
    VrBodyFollowing core;core.identify(1);
    require(!core.ground_speed(start),"no speed before any body update");
    // 7.74 world units/s (8.6 m/s at 0.9), updates every 11 ms with alternating timing jitter.
    float x=0;auto t=start;
    for(int n=0;n<40;++n) { const auto dt=std::chrono::microseconds(n%2?9000:13000);t+=dt;x+=7.74f*std::chrono::duration<float>(dt).count();
        core.body(1,2,3,{x,float(n%3),0},t); }
    const auto speed=core.ground_speed(t);
    require(speed && std::abs(*speed-7.74f)<0.01f,"windowed horizontal speed matches the native body (height changes ignored)");
    require(!core.ground_speed(t+std::chrono::milliseconds(150)),"stale body: speed unobserved");
    core.body(1,9,3,{x+5,0,0},t+std::chrono::milliseconds(5));
    require(!core.ground_speed(t+std::chrono::milliseconds(5)),"a different native body restarts the measurement (no teleport speed)");
    core.identify(2);require(!core.ground_speed(t+std::chrono::milliseconds(5)),"actor change clears the speed");
}
void detours() {
    auto& probe=EngineBodyProbe::get(); auto& follow=EngineBodyFollowing::get();native_cutscenes(follow);
    require(probe.install(reinterpret_cast<void*>(&native_update),reinterpret_cast<void*>(&native_world),0x140000000,0x5000000),"native ownership hooks must install");
    std::optional<bool> scripted=false;
    require(follow.install(reinterpret_cast<void*>(&native_context),{set_yaw,[&]{return scripted;}}),"native facing/context adapter must install");
    probe.enable(true); follow.enable(true); follow.allow(true);
    std::array<uint8_t,0x700> body{},entity{},actor{};
    std::array<uintptr_t,1> actor_wrapper{reinterpret_cast<uintptr_t>(actor.data())},args{};
    std::array<uintptr_t,10> table{}; table[9]=0x140000000+0x134c2e0;
    std::array<uint8_t,0x180> tracker{};
    std::array<float,16> world=vr_camera::IDENTITY,requested=vr_camera::IDENTITY;
    int context{};
    write(body.data(),8,reinterpret_cast<uintptr_t>(entity.data()));
    write(body.data(),0x130,uintptr_t(123)); write(body.data(),0x138,reinterpret_cast<uintptr_t>(actor_wrapper.data()));
    write(body.data(),0x4d8,reinterpret_cast<uintptr_t>(&context));
    write(actor.data(),0x38,reinterpret_cast<uintptr_t>(entity.data()));
    write(entity.data(),0xc8,std::array<float,3>{0,0,0}); write(entity.data(),0xb8,std::array<float,3>{0,0,-1});
    args[0]=reinterpret_cast<uintptr_t>(requested.data());
    write(tracker.data(),0,reinterpret_cast<uintptr_t>(table.data())); write(tracker.data(),0xd0,reinterpret_cast<uintptr_t>(actor_wrapper.data()));
    probe.observe_tracker(0x140000000+0x13a4476,tracker.data(),world.data(),true);
    require(probe.active_actor()==reinterpret_cast<uintptr_t>(actor.data()),"local player must be identified by exact camera-root owner pointer");
    void(*volatile tick)(void*,void*)=native_update; tick(body.data(),args.data());
    EngineCameraProbe::VrEye eye; eye.captured=std::chrono::steady_clock::now(); eye.reference_epoch=1;
    eye.relative_head={0,0,1,0, 0,1,0,0, -1,0,0,0, 0,0,0,1};
    auto game=vr_camera::IDENTITY; game[10]=-1;
    follow.camera(game,eye); tick(body.data(),args.data());
    require(yaw_calls==1 && yaw_actor==actor.data() && close(yaw_degrees,90),"original body-update hook must invoke regular native yaw setter for camera-owned actor on real path");
    follow.dodge(true,std::chrono::steady_clock::now());tick(body.data(),args.data());
    require(yaw_calls==1 && follow.snapshot().follow.anchored,"actual native body hook must yield its yaw setter during a dodge without detaching the camera");
    follow.allow(false);follow.allow(true);
    scripted=true;tick(body.data(),args.data());
    require(yaw_calls==1 && !follow.snapshot().follow.anchored,"native sequence state must suspend yaw before a close shot renders");
    require(follow.camera(game,eye)==game && follow.snapshot().follow.camera_mode==VrBodyFollowing::CameraMode::scripted,"close shot detection must reach actual native camera adapter");
    scripted=false;follow.camera(game,eye);
    auto remote=game;remote[12]=30;
    require(follow.camera(remote,eye)==remote && !follow.snapshot().follow.anchored,"native adapter must release the player anchor for remote camera views");
    tick(body.data(),args.data());
    require(yaw_calls==1 && native_body_calls==5,"sequence camera must stop yaw writes while preserving the original native body update");
    follow.camera(game,eye);
    {   // A native camera 1.2 units off the root: outside the Marine's 1 m envelope at 0.9 units/m, inside it at the eyes' 1.45.
        auto scaled=eye;scaled.units_per_metre=1.45f;scaled.fit_factor=1.45f/.9f;scaled.captured=std::chrono::steady_clock::now();
        auto beside=game;beside[12]=1.2f;
        follow.camera(beside,scaled);
        require(close(follow.snapshot().follow.camera_radius,1.2f/.9f),"the camera envelope stays in the Marine's metres at a larger first-person scale");
    }
    auto other_body=body,other_actor=actor;
    actor_wrapper[0]=reinterpret_cast<uintptr_t>(other_actor.data()); tick(other_body.data(),args.data());
    require(yaw_calls==1,"NPC/other actor body updates must never receive player-facing writes"); actor_wrapper[0]=reinterpret_cast<uintptr_t>(actor.data());
    write(body.data(),0x578,0.02f); std::array<float,8> output{};
    void(*volatile update)(void*,void*,float*)=native_context;
    update(&context,body.data()+0x4f0,output.data());
    require(native_context_calls==1 && follow.snapshot().matched_contexts==1 && output[1]==-0.1f && output[3]==123 && output[4]==321,
        "context detour must identify exact body-owned context/parameters and preserve native velocity/remaining output with room mode off");
    follow.allow(false); tick(body.data(),args.data()); require(yaw_calls==1,"focus/tracking/neutral gate must stop further native facing");
    require(native_body_calls==7,"body following must preserve every original native body update");
    require(!probe.active_actor(std::chrono::steady_clock::now()+std::chrono::milliseconds(101)),"expired root ownership must not select an actor");
    auto second_tracker=tracker;
    std::array<uintptr_t,1> second_owner{reinterpret_cast<uintptr_t>(other_actor.data())};
    write(second_tracker.data(),0xd0,reinterpret_cast<uintptr_t>(second_owner.data()));
    probe.observe_tracker(0x140000000+0x13a4476,second_tracker.data(),world.data(),true);
    require(probe.active_actor()==0,"two currently active camera owners must fail closed rather than choosing by proximity");
    auto yaw_bytes=synthetic_at<16>(0x185de20);auto context_bytes=synthetic_at<16>(0x2b21390);
    require(follow.matches_build(0x6ac3e716,yaw_bytes,context_bytes) && !follow.matches_build(0,yaw_bytes,context_bytes),"facing/context integration must require exact executable build");
    yaw_bytes[15]^=1; require(!follow.matches_build(0x6ac3e716,yaw_bytes,context_bytes),"changed native yaw helper must reject writes");
    yaw_bytes[15]^=1; context_bytes[15]^=1; require(!follow.matches_build(0x6ac3e716,yaw_bytes,context_bytes),"changed native context helper must reject detour");
    follow.enable(false); require(follow.camera(game,eye)==game,"disabled follow must preserve the native game camera");
    follow.uninstall(); probe.uninstall();
}
}
int main(int argc,char** argv) { try { if(argc>1) camera_guard_file(argv[1]); if(argc>2) takeover_replay(argv[2]); if(argc>3) combo_replay(argv[3]); math(); dodge_facing_authority(); movement_camera_frames(); gaze_steered_native_camera(); head_placement_turns(); turn_threshold(); melee_facing(); external_camera_handover(); dodge_camera_recovery(); animation_takeover_holds_place(); animation_takeover_levels(); animation_takeover_head(); animation_takeover_return(); takeover_stick_follows_view(); takeover_early_return(); takeover_settled_return(); combo_stays_first_person(); jump_pack_rides_along(); height_lead_on_slopes(); cutscene_holds_trigger_spot(); native_cinematic_state(); close_scripted_handover(); scripted_heading_sweep(); ground_speed(); detours(); std::cout << "camera-owned native facing, reference isolation and accepted room-motion bookkeeping passed\n"; return 0; }
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
