#include "VrSwingMelee.h"
#include "VrControllerInput.h"
#include "EngineChainswordRev.h"
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
using namespace std::chrono;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
const auto START=steady_clock::time_point{seconds(10)};
VrSwingMelee::Pose pose(int ms,float x=0,float head_x=0,uint64_t epoch=1) {
    auto hand=vr_camera::IDENTITY,head=hand;hand[12]=x;head[12]=head_x;
    return {hand,head,START+milliseconds(ms),epoch};
}
void gestures() {
    VrSwingMelee detector;const auto step=[&](int ms,float x=0,float hx=0,uint64_t epoch=1,bool allow=true) {
        return detector.update(pose(ms,x,hx,epoch),allow,START+milliseconds(ms));
    };
    require(!step(0) && !detector.snapshot().ready,"first tracking point must not arm a swing");
    require(!step(20) && detector.snapshot().ready,"stationary tracking must prepare a swing");
    require(!step(40,0.06f) && !step(60,0.12f),"short fast motion must not request an attack");
    require(step(80,0.2f) && detector.snapshot().swings==1,"fast committed motion must produce one bounded attack");
    VrControllerInput bridge;bridge.enable(true);bridge.update(VrControllerSample{},0,true,START+milliseconds(80));
    VrControllerSample sample;sample.swing_melee=true;bridge.update(sample,0,true,START+milliseconds(80));
    XINPUT_STATE native{};uint32_t result=ERROR_DEVICE_NOT_CONNECTED;
    bridge.get_state(&result,0,&native,START+milliseconds(80));
    require(result==ERROR_SUCCESS && native.Gamepad.wButtons==XINPUT_GAMEPAD_RIGHT_SHOULDER,"physical request must reach native RB without grip force");
    for(int ms=100;ms<=200;ms+=20) require(step(ms,0.2f),"native request must remain held for the configured pulse");
    require(!step(220,0.2f) && !detector.snapshot().ready,"pulse must expire before cooldown permits another attack");
    sample.swing_melee=false;bridge.update(sample,0,true,START+milliseconds(220));
    require(!bridge.snapshot(START+milliseconds(220)).pad.Gamepad.wButtons,"motion request must not remain latched as a grip");
    for(int ms=240;ms<=540;ms+=20) step(ms,0.2f);
    require(detector.snapshot().ready && detector.snapshot().swings==1,"rest after recovery must rearm without repeating");
    require(!step(560,0.26f) && !step(580,0.32f) && step(600,0.40f) && detector.snapshot().swings==2,"a second distinct swing must request the next combo input");
    require(!step(620,0.4f,0,1,false),"combat/focus gate must release a pulse immediately");
    require(!step(640,0.5f,0,2) && !detector.snapshot().ready,"epoch changes must not turn displacement into a strike");
    step(660,0.5f,0,2);
    require(!step(680,20,0,2) && detector.snapshot().discontinuities==1,"tracking teleports must not attack");
    require(!detector.update(pose(700),true,START+milliseconds(801)),"stale tracking must release attacks");
    auto broken=pose(820);broken.hand[0]=NAN;
    require(!detector.update(broken,true,broken.captured),"malformed hand poses must fail closed");
    require(!detector.update(pose(840),true,START+milliseconds(839)),"future samples must fail closed");
    const auto rejected=detector.snapshot().discontinuities;
    auto overflow=pose(860,std::numeric_limits<float>::max(),-std::numeric_limits<float>::max());
    require(!detector.update(overflow,true,overflow.captured) && detector.snapshot().discontinuities==rejected+1,"finite poses whose relative tip overflows must reject before entering motion history");
    step(880);step(900);
    require(detector.snapshot().ready && std::isfinite(detector.snapshot().speed) && std::isfinite(detector.snapshot().travel),"valid tracking must recover with finite statistics after arithmetic overflow");
}
void body_and_settings() {
    VrSwingMelee detector;
    for(int ms=0;ms<=300;ms+=20) {
        const auto p=pose(ms,ms*0.003f,ms*0.003f);
        require(!detector.update(p,true,p.captured),"walking together with head and hand must not attack");
    }
    require(detector.snapshot().swings==0,"whole-body translation is not blade travel");
    auto head_turn=pose(320,0.9f,0.9f);head_turn.head[0]=head_turn.head[10]=0;head_turn.head[2]=1;head_turn.head[8]=-1;
    require(!detector.update(head_turn,true,head_turn.captured),"head rotation alone must not create blade motion");
    auto settings=detector.settings();std::ostringstream text;require(settings.write(text),"settings serialize");
    std::istringstream input{text.str()};require(VrSwingMelee::Settings::read(input)==settings,"settings roundtrip exactly");
    std::istringstream duplicate{text.str()+"pulse_ms=100\n"};require(!VrSwingMelee::Settings::read(duplicate),"duplicate settings must reject whole preset");
    std::istringstream incomplete{"pulse_ms=140\n"};require(!VrSwingMelee::Settings::read(incomplete),"incomplete presets must not mix configurations");
    settings.values[0]=NAN;require(!detector.set_settings(settings),"invalid settings must preserve active configuration");
    for(size_t knob=0;knob<6;++knob) {
        VrSwingMelee a,b;auto alternative=b.settings();
        alternative.values[knob]=std::array<float,6>{4.0f,0.5f,0.9f,0.05f,240,900}[knob];
        require(b.set_settings(alternative),"each declared knob must accept a different valid preset");
        bool output_changed=false;
        for(int ms=0;ms<=1100;ms+=20) {
            float x=ms<=20?0:ms<=100?(ms-20)*0.0025f:ms<=640?0.2f:0.2f+(ms-640)*0.0025f;
            auto p=pose(ms,x);
            if(knob==2) { // Same wrist position, rotating blade: reach changes tip travel.
                const auto angle=ms<=20?0.0f:std::min((ms-20)*0.008f,1.0f);
                // The wrist draws back 12 cm as the blade turns (no flick), too little to commit by
                // itself; when the tip commits still depends on reach.
                p.hand[12]=0;p.hand[14]=ms<=20?0:std::min(ms-20,20)*0.006f;p.hand[0]=p.hand[10]=std::cos(angle);p.hand[2]=std::sin(angle);p.hand[8]=-std::sin(angle);
            }
            if(knob==3 && ms>=500 && ms<=640) p.hand[12]=0.2f+(ms-500)*0.0002f;
            a.update(p,true,p.captured);b.update(p,true,p.captured);
            const auto xstate=a.snapshot(),ystate=b.snapshot();
            output_changed|=xstate.pressed!=ystate.pressed || xstate.ready!=ystate.ready || xstate.swings!=ystate.swings;
        }
        require(output_changed,"every knob must change actual gesture output on a production trajectory");
    }
}
void cancelled_tip_arc() {
    VrSwingMelee detector;
    for(int ms=0;ms<=800;ms+=20) {
        const float yaw=(-60.f+(ms<=200?0.f:ms>=600?120.f:(ms-200)*.3f))*.01745329252f;
        auto p=pose(ms);p.hand[0]=p.hand[10]=std::cos(yaw);p.hand[2]=-std::sin(yaw);p.hand[8]=std::sin(yaw);
        p.hand[12]=.45f*std::sin(yaw);p.hand[13]=-.2f;p.hand[14]=-.8f+.45f*std::cos(yaw);
        detector.update(p,true,p.captured);
    }
    require(detector.snapshot().swings==1,"a committed wrist arc rotating around a stationary estimated tip must trigger exactly once");
}
void attack_kind_latches() {
    VrSwingMelee detector;
    const auto step=[&](int ms,float x,bool power) {return detector.update(pose(ms,x),true,START+milliseconds(ms),power);};
    step(0,0,false);step(20,0,false);step(40,.06f,false);step(60,.12f,false);
    require(step(80,.2f,true) && detector.snapshot().power,"trigger at swing commitment selects power");
    require(step(100,.2f,false) && detector.snapshot().power,"trigger release cannot convert power pulse to light");
    detector.reset();step(0,0,true);step(20,0,true);step(40,.06f,true);step(60,.12f,true);
    require(step(80,.2f,false) && !detector.snapshot().power,"released trigger at commitment selects light");
    require(step(100,.2f,true) && !detector.snapshot().power,"trigger press cannot create a second action inside light pulse");
    VrControllerInput input;input.enable(true);input.update(VrControllerSample{},0,true,START);
    VrControllerSample sample;sample.swing_melee=true;sample.swing_power=true;sample.guard=true;sample.parry=1;
    input.update(sample,0,true,START+milliseconds(20));
    require(input.snapshot(START+milliseconds(20)).pad.Gamepad.wButtons==XINPUT_GAMEPAD_DPAD_UP,"power pulse reserves up and cannot also parry or light attack");
    sample.swing_melee=false;sample.swing_power=false;sample.guard=false;input.update(sample,0,true,START+milliseconds(40));
    // A new LB edge after the pulse would be a native parry that also aborts the attack.
    require(!input.snapshot(START+milliseconds(40)).pad.Gamepad.wButtons,"a parry grip held through the attack does not press LB again when the pulse ends");
    sample.parry=0;input.update(sample,0,true,START+milliseconds(60));
    sample.parry=1;input.update(sample,0,true,START+milliseconds(80));
    require(input.snapshot(START+milliseconds(80)).pad.Gamepad.wButtons==XINPUT_GAMEPAD_LEFT_SHOULDER,"a fresh grip squeeze parries again");
    sample.parry=0;sample.guard=true;input.update(sample,0,true,START+milliseconds(100));
    require(input.snapshot(START+milliseconds(100)).pad.Gamepad.wButtons==XINPUT_GAMEPAD_LEFT_SHOULDER,"a guard the gesture raises after the attack blocks (VrGuard::yield decides when)");
}
void fast_rotational_swipe() {
    // Captured physical failures had wrist ~6 m/s while the blade tip crossed
    // the old 12 m/s ceiling. Reproduce those independent speeds with a rigid
    // 20 ms wrist/yaw trajectory, then verify the native released-trigger route.
    for(bool power:{false,true}) {
        VrSwingMelee detector;
        detector.update(pose(0),true,START,power);
        detector.update(pose(20),true,START+milliseconds(20),power);
        auto fast=pose(40,-.12f);
        const float yaw=.35f;
        fast.hand[0]=fast.hand[10]=std::cos(yaw);
        fast.hand[2]=-std::sin(yaw);fast.hand[8]=std::sin(yaw);
        require(detector.update(fast,true,fast.captured,power),"valid fast rotational swipe must request attack instead of resetting");
        require(detector.snapshot().speed>12 && detector.snapshot().discontinuities==0,
                "rotational blade speed above 12 m/s is not a wrist tracking teleport");
        require(detector.snapshot().power==power,"fast arc retains trigger-selected attack kind");
        auto teleport=fast;teleport.captured+=milliseconds(20);teleport.hand[12]+=1;
        require(!detector.update(teleport,true,teleport.captured,power) && detector.snapshot().discontinuities==1,
                "fast tip allowance must retain wrist teleport rejection and cancel active attacks");
    }
}
// Playtest 20 Oct, t=561.71 s: a left punch (left wrist 5.8 m/s) twisted the right
// controller enough for its 0.45 m virtual tip to commit a light attack.
void other_hand_veto() {
    for(float other:{0.0f,1.0f,3.0f,5.8f}) {
        VrSwingMelee detector;
        const auto step=[&](int ms,float x){return detector.update(pose(ms,x),true,START+milliseconds(ms),false,{.other_hand_speed=other});};
        step(0,0);step(20,0);step(40,.06f);step(60,.12f);
        const bool pressed=step(80,.2f); // This hand: 4 m/s at commitment.
        const auto s=detector.snapshot();
        if(other<4) require(pressed && s.swings==1 && !s.vetoes,"a slower or resting other hand never blocks this hand's swing");
        else require(!pressed && !s.swings && s.vetoes==1 && s.other_hand_speed==other,"a faster other hand owns the motion: no native attack");
        if(other<4) continue;
        require(!step(100,.26f) && !step(120,.32f) && detector.snapshot().vetoes==1 && !detector.snapshot().swings,"the vetoed motion cannot commit later in the same movement");
        for(int ms=140;ms<=700;ms+=20) step(ms,.32f);
        require(detector.snapshot().ready,"rest after the cooldown rearms");
        VrSwingMelee::Commit alone{};
        bool later=false;for(int ms=720;ms<=780;ms+=20) later|=detector.update(pose(ms,.32f+(ms-700)*.004f),true,START+milliseconds(ms),false,alone);
        require(later && detector.snapshot().swings==1,"the next swing of this hand alone attacks");
    }
}
// Playtest 20 Oct: swings whose follow-through reached the hip holster zone sent 44 ms
// pulses, and the cleared cooldown let a second (power) swing start 313 ms later.
void holster_zone_keeps_pulse() {
    VrSwingMelee detector;
    const auto step=[&](int ms,float x,bool zone){return detector.update(pose(ms,x),true,START+milliseconds(ms),false,{.allowed=!zone});};
    step(0,0,false);step(20,0,false);step(40,.06f,false);step(60,.12f,false);
    require(step(80,.2f,false),"swing commits outside the zone");
    for(int ms=100;ms<=200;ms+=20) require(step(ms,.22f,true),"a follow-through into a holster zone keeps the configured pulse");
    require(!step(220,.22f,true),"the pulse still ends on time");
    step(240,.22f,false);step(260,.22f,false);step(280,.22f,false);
    bool second=false;for(int ms=300;ms<=400;ms+=20) second|=step(ms,.22f+(ms-280)*.004f,false);
    require(!second && detector.snapshot().swings==1,"the cooldown survives the zone: no second attack 313 ms later");
    for(int ms=420;ms<=700;ms+=20) step(ms,.7f,false);
    require(detector.snapshot().ready,"rest after the cooldown rearms");
    bool inside=false;for(int ms=720;ms<=780;ms+=20) inside|=step(ms,.7f+(ms-700)*.004f,true);
    require(!inside && detector.snapshot().ready,"a motion inside the zone never commits, and stays ready");
    // Joe, 21 Oct: the sword rests at the hip and is raised by the shoulder, both holster
    // zones. A swing leaving a zone is a swing.
    bool leaving=false;for(int ms=800;ms<=840;ms+=20) leaving|=step(ms,1.02f+(ms-780)*.004f,false);
    require(leaving && detector.snapshot().swings==2,"a swing that started in a zone commits once it leaves");
}
// Joe, 20 Oct: no wrist flicks. Rotating the controller about a still wrist sweeps the
// virtual tip past the speed and travel thresholds but must not attack.
void wrist_flick() {
    const auto twisted=[](int ms,float yaw,float x){auto p=pose(ms,x);p.hand[0]=p.hand[10]=std::cos(yaw);p.hand[2]=-std::sin(yaw);p.hand[8]=std::sin(yaw);p.hand[12]=x;return p;};
    for(float wrist:{0.0f,0.08f,0.15f}) {
        VrSwingMelee detector;bool pressed=false;
        for(int ms=0;ms<=400;ms+=20) {
            const float f=ms<100?0:ms>=200?1:(ms-100)/100.0f;
            const auto p=twisted(ms,f*0.8f,f*wrist); // 46 degrees: tip 0.35 m in 100 ms.
            pressed|=detector.update(p,true,p.captured);
        }
        const auto s=detector.snapshot();
        if(wrist<VrSwingMelee::MIN_WRIST_TRAVEL) require(!pressed && !s.swings && s.flicks==1,"a twist with the wrist (nearly) still is a flick: no attack, counted once");
        else require(pressed && s.swings==1 && !s.flicks,"the same twist with the hand moving 15 cm attacks");
    }
}
// Joe's 21 Oct trace: flowing slashes reversed at about 1 m/s and never reached the 0.5 m/s
// rearm, so only the first stroke of a combo could attack.
void combo_rearm() {
    struct Case { float second;bool combo;bool expected;const char* message; };
    for(const auto& c:{Case{.1f,true,true,"a second hard stroke after a reversal attacks without stopping"},
                       Case{.04f,true,false,"bringing the sword back slowly after a hard stroke is not a second attack"},
                       Case{.1f,false,false,"the 20 Oct rules (no combo rearm) need the hand to stop between strokes"}}) {
        VrSwingMelee detector;detector.set_stroke_rules(c.combo);float x=0;int ms=0;
        const auto step=[&](float dx){ms+=20;x+=dx;return detector.update(pose(ms,x),true,START+milliseconds(ms));};
        step(0);step(0);
        bool first=false;for(int i=0;i<6;++i) first|=step(.1f); // 5 m/s stroke
        require(first && detector.snapshot().swings==1,"the first stroke attacks");
        for(int i=0;i<20;++i) step(-.02f); // Reversal at 1 m/s: above the 0.5 rearm, past the cooldown.
        bool second=false;for(int i=0;i<6;++i) second|=step(c.second);
        require(second==c.expected && detector.snapshot().swings==(c.expected?2u:1u),c.message);
    }
    VrSwingMelee gentle;float x=0;int ms=0;
    const auto step=[&](float dx){ms+=20;x+=dx;return gentle.update(pose(ms,x),true,START+milliseconds(ms));};
    step(0);step(0);for(int i=0;i<6;++i) step(.04f); // A 2 m/s stroke: too gentle to rearm mid-motion.
    for(int i=0;i<20;++i) step(-.02f);
    bool again=false;for(int i=0;i<6;++i) again|=step(.04f);
    require(gentle.snapshot().swings==1 && !again,"after a gentle stroke the hand must still settle before the next attack");
}
// Joe's 21 Oct slashes reversed sooner than the 450 ms cooldown: the dip is remembered.
void reversal_inside_cooldown() {
    VrSwingMelee detector;float x=0;int ms=0;
    const auto step=[&](float dx){ms+=20;x+=dx;return detector.update(pose(ms,x),true,START+milliseconds(ms));};
    step(0);step(0);for(int i=0;i<6;++i) step(.1f);
    for(int i=0;i<8;++i) step(-.02f); // Reversal ends 300 ms in, before the cooldown.
    bool second=false;for(int i=0;i<12;++i) second|=step(.1f);
    require(second && detector.snapshot().swings==2,"a reversal inside the cooldown rearms the next hard stroke once the cooldown ends");
}
// Joe's 21 Oct trace: lifting the sword from the hip to the shoulder read as the swing and
// the real slash that followed fell in the cooldown.
void windup_keeps_readiness() {
    VrSwingMelee detector;float y=0,z=0;int ms=0;
    const auto step=[&](float dy,float dz){ms+=20;y+=dy;z+=dz;auto p=pose(ms);p.hand[13]=y;p.hand[14]=z;return detector.update(p,true,START+milliseconds(ms));};
    step(0,0);step(0,0);
    bool lift=false;for(int i=0;i<5;++i) lift|=step(.06f,.04f); // Up and back towards the body (gaze is -Z).
    require(!lift && !detector.snapshot().swings && detector.snapshot().windups==1 && detector.snapshot().ready,"a wind-up neither attacks nor uses up readiness");
    bool strike=false;for(int i=0;i<4;++i) strike|=step(-.06f,-.06f); // Down and forward.
    require(strike && detector.snapshot().swings==1,"the strike after the wind-up attacks");
    VrSwingMelee rising;y=z=0;ms=0;
    const auto up=[&](float dy,float dz){ms+=20;y+=dy;z+=dz;auto p=pose(ms);p.hand[13]=y;p.hand[14]=z;return rising.update(p,true,START+milliseconds(ms));};
    up(0,0);up(0,0);bool uppercut=false;for(int i=0;i<5;++i) uppercut|=up(.06f,-.04f);
    require(uppercut,"a rising slash that moves forward is a strike");
}
// 20 Oct replay: raising the fist straight up for the emote read as a swing just before it
// fired (220.5, 679.3, 682.6, 685.3 s) once the squeezed fist no longer owned the hand.
void vertical_raise_is_windup() {
    VrSwingMelee detector;float y=0,x=0;int ms=0;
    const auto step=[&](float dy,float dx){ms+=20;y+=dy;x+=dx;auto p=pose(ms,x);p.hand[13]=y;return detector.update(p,true,START+milliseconds(ms));};
    step(0,0);step(0,0);
    bool raise=false;for(int i=0;i<5;++i) raise|=step(.07f,.005f);
    require(!raise && detector.snapshot().windups==1 && detector.snapshot().ready,"a near-vertical raise is a wind-up");
    VrSwingMelee across;y=x=0;ms=0;
    const auto slash=[&](float dy,float dx){ms+=20;y+=dy;x+=dx;auto p=pose(ms,x);p.hand[13]=y;return across.update(p,true,START+milliseconds(ms));};
    slash(0,0);slash(0,0);bool rising=false;for(int i=0;i<5;++i) rising|=slash(.05f,.06f);
    require(rising,"a rising slash across the body is a strike");
}
// Joe, 21 Oct: swings with the bolter out, but aiming and recoil never attack.
void gun_strokes() {
    const auto run=[](float per_step,float start_height,float trigger) {
        VrSwingMelee swing;VrHandMotion left;VrMeleePoll p;p.allowed=true;p.gun=true;p.trigger=trigger;bool pressed=false;
        for(int i=0;i<=8;++i) {
            auto right=pose(i*20,i<2?0:(i-1)*per_step,0);right.hand[13]=start_height-(i<2?0:(i-1)*.03f);
            p.right=right;p.left=pose(i*20,-.3f);
            pressed|=vr_melee_poll(swing,left,p,START+milliseconds(i*20));
        }
        return std::pair{pressed,swing.snapshot()};
    };
    require(!run(.07f,-.3f,0).first,"a 3.5 m/s stroke with the bolter out is aiming speed, not a swing");
    const auto [hard,state]=run(.13f,-.3f,1);
    require(hard && state.swings==1 && !state.power,"a 6.5 m/s stroke attacks, always light (the trigger is fire)");
    require(!run(.13f,.2f,0).first,"a stroke starting above eye level (an arm lowered from overhead) is not a gun swing");
    VrGunMeleeGate gate;const auto t=START;
    require(gate.update(0,0,false,t),"triggers released: open");
    require(!gate.update(.6f,0,false,t+milliseconds(20)),"a shot closes it");
    require(!gate.update(0,0,false,t+milliseconds(300)),"recoil and aim settle for 300 ms");
    require(gate.update(0,0,false,t+milliseconds(330)),"then it opens");
    require(!gate.update(0,.4f,false,t+milliseconds(340)) && !gate.update(0,0,true,t+milliseconds(700)),"aiming down sights or the support grip close it");
}
void two_handed_hold() {
    VrTwoHanded hands;auto right=vr_camera::IDENTITY,left=right;left[12]=.2f;
    require(hands.update(right,left,START),"hands 20 cm apart are one grip on the sword");
    left[12]=.5f;require(hands.update(right,left,START+milliseconds(200)),"a two-handed swing may part the hands briefly");
    require(!hands.update(right,left,START+milliseconds(320)),"apart for longer, each hand is its own");
    left[12]=.25f;require(hands.update(right,left,START+milliseconds(340)) && !hands.update(right,std::nullopt,START+milliseconds(350)),"a lost hand is not a grip");
    // Through the shared poll the left hand's speed then cannot veto the right.
    VrSwingMelee swing;VrHandMotion left_motion;VrMeleePoll p;p.allowed=true;p.two_handed=true;bool pressed=false;
    for(int i=0;i<=6;++i) {
        p.right=pose(i*20,i<2?0:(i-1)*.07f);p.left=pose(i*20,i<2?-.2f:-.2f+(i-1)*.1f);
        pressed|=vr_melee_poll(swing,left_motion,p,START+milliseconds(i*20));
    }
    require(pressed && !swing.snapshot().vetoes,"both hands swinging the sword together attack");
}
// Joe's 21 Oct evening trace: an execution camera reset the detector mid-flurry; his strokes
// then reversed at 0.54-1.1 m/s, never below the 0.5 m/s rearm, and 6 s of slashes sent nothing.
void fresh_rearm() {
    struct Case { bool fresh;float second;bool expected;const char* message; };
    for(const auto& c:{Case{true,.1f,true,"after a reset mid-flurry, a hard stroke after a reversal attacks"},
                       Case{false,.1f,false,"without the reversal rearm the flurry never attacks (21 Oct evening)"},
                       Case{true,.04f,false,"after a reset, bringing the sword back slowly is not an attack"}}) {
        VrSwingMelee detector;detector.set_fresh_rearm(c.fresh);float x=0;int ms=0;
        const auto step=[&](float dx,bool allowed=true){ms+=20;x+=dx;return detector.update(pose(ms,x),allowed,START+milliseconds(ms));};
        for(int i=0;i<3;++i) step(.1f,false); // Moving while not allowed: the detector resets.
        bool first=false;for(int i=0;i<6;++i) first|=step(.1f); // The stroke in progress continues at 5 m/s.
        require(!first && !detector.snapshot().swings,"a stroke already moving when the detector starts never attacks");
        for(int i=0;i<10;++i) step(-.02f); // Reversal at 1 m/s, above the 0.5 m/s rearm.
        bool second=false;for(int i=0;i<6;++i) second|=step(c.second);
        require(second==c.expected && detector.snapshot().swings==(c.expected?1u:0u),c.message);
    }
    VrSwingMelee gentle;float x=0;int ms=0;
    const auto step=[&](float dx,bool allowed=true){ms+=20;x+=dx;return gentle.update(pose(ms,x),allowed,START+milliseconds(ms));};
    for(int i=0;i<3;++i) step(.04f,false);
    for(int i=0;i<6;++i) step(.04f); // 2 m/s: too gentle to arm by reversal.
    for(int i=0;i<10;++i) step(-.02f);
    bool again=false;for(int i=0;i<6;++i) again|=step(.04f);
    require(!again && !gentle.snapshot().swings,"after a reset, gentle motion still needs the hand to settle first");
}
// Joe, 21 Oct evening: "power attack maybe is still too sensitive". Power needs the trigger
// pulled before the stroke, not clenched during it.
void power_hold() {
    const auto run=[](int pulled_before_ms) {
        VrSwingMelee swing;VrHandMotion left;VrPowerTrigger trigger;VrMeleePoll p;p.allowed=true;bool pressed=false;
        const int stroke=400;
        for(int ms=0;ms<=stroke+120;ms+=20) {
            p.trigger=ms>=stroke-pulled_before_ms?1.f:0.f;p.trigger_held=trigger.update(p.trigger,START+milliseconds(ms));
            p.right=pose(ms,ms<stroke?0:(ms-stroke+20)*.005f);p.left=pose(ms,-.3f);
            pressed|=vr_melee_poll(swing,left,p,START+milliseconds(ms));
        }
        require(pressed && swing.snapshot().swings==1,"the stroke attacks");
        return swing.snapshot().power;
    };
    require(run(300),"the trigger held before the stroke started selects power");
    require(!run(60),"the trigger pulled as the stroke starts (a clench) leaves it light");
    require(!run(-1000),"no trigger: light");
    VrPowerTrigger t;
    require(t.update(.1f,START)==VrSwingMelee::Clock::duration{},"a resting finger (0.1) is not held");
    t.update(.5f,START);require(t.update(.5f,START+milliseconds(250))==milliseconds(250),"held time accumulates");
    require(t.update(0,START+milliseconds(260))==VrSwingMelee::Clock::duration{} && t.update(.5f,START+milliseconds(270))==VrSwingMelee::Clock::duration{},"a release restarts it");
}
void power_threshold_matches_rev() {
    require(VrSwingMelee::POWER_TRIGGER==EngineChainswordRev::START,"power starts where the chainsword rev starts");
    require(0.09f<VrSwingMelee::POWER_TRIGGER,"a resting finger (0.05-0.09 measured) is not power");
}
void hand_motion_meter() {
    VrHandMotion meter;const float reach=.45f;
    const auto at=[&](int ms,float x,int now_ms=-1,uint64_t epoch=1){return meter.update(pose(ms,x,0,epoch),reach,START+milliseconds(now_ms<0?ms:now_ms));};
    require(at(0,0)==0,"one sample has no speed");
    require(std::abs(at(20,.04f)-2)<1e-3f,"speed is the endpoint translation rate (2 m/s)");
    require(std::abs(at(20,.04f,30)-2)<1e-3f,"a repeated poll of a cached pose keeps the measured peak and adds no derivative");
    require(std::abs(at(40,.04f)-2)<1e-3f,"the peak is held while the hand rests");
    for(int ms=60;ms<=160;ms+=20) at(ms,.04f);
    require(at(180,.04f)==0,"the peak expires after its window");
    require(at(200,.04f,320)==0 && at(220,.30f,320)==0,"stale tracking reads as no motion and restarts the derivative");
    at(240,.30f);
    require(at(260,.40f,260,2)==0,"a re-anchored epoch never becomes speed");
    require(at(280,1.60f,280,2)==0,"a tracking jump is not a punch");
    auto turned=pose(300,1.6f,0,2);turned.head[0]=turned.head[10]=0;turned.head[2]=1;turned.head[8]=-1;
    meter.update(pose(290,1.6f,0,2),reach,START+milliseconds(290));
    require(meter.update(turned,reach,START+milliseconds(300))==0,"head rotation alone is not hand motion");
}
}
int main() { try { gestures();body_and_settings();cancelled_tip_arc();attack_kind_latches();fast_rotational_swipe();other_hand_veto();holster_zone_keeps_pulse();hand_motion_meter();wrist_flick();power_threshold_matches_rev();combo_rearm();reversal_inside_cooldown();windup_keeps_readiness();vertical_raise_is_windup();gun_strokes();two_handed_hold();fresh_rearm();power_hold();std::cout<<"physical swing lifecycle, native requests and live settings passed\n";return 0; }
catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
