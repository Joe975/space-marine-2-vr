// Replays the production sword/guard/holster arbitration on samples exported from a playtest
// trace (scripts/replay_melee_trace.py writes them), so a rule change can be judged against
// what the player actually did. Uses the same classes and the same per-poll function
// (vr_melee_poll) as VrSession, in the same order as VrSession::poll_controllers; it runs at
// the trace's sample rate, not the input poll rate, so speeds are lower than live and short
// pulses are seen less often.
//
// Usage: melee_replay SAMPLES.txt [--old | --no-strokes | --no-zones | --no-two-handed | --no-gun]
//   --dump FROM TO: also print the gate state of every sample between FROM and TO seconds.
//   --old-emote: the 19 Oct emote (0.15 m above the eyes, a squeezed fist owns the hand anywhere).
//   --old: the 20 Oct rules (a holster zone resets the detector, no combo rearm or wind-up,
//          no two-handed hold, no swings with the bolter out, raw loadout gate).
//   --no-power-hold: power on the trigger alone (before 21 Oct evening; VrPowerTrigger::HOLD).
//   --no-fresh-rearm: no reversal rearm after a detector reset (before 21 Oct evening).
//   --no-push: no left-arm push (VrLeftPush).
//   --set guard.NAME=VALUE | holster.NAME=VALUE: override one calibration parameter.
//   --no-grenade: no grenade gesture (VrGrenadeThrow); the guard never waits at the belt.
//   --guard-belt-latch: a hand resting in the grenade belt zone latches the guard, so a raise
//          already turned across when it leaves the belt does not guard (before 26 Oct).
// Each input line: t epoch armed loadout rt grip lt  head(x y z fx fy fz)
//                  right_valid right(x y z fx fy fz)  left_valid left(x y z fx fy fz)  [support [left_grip]]
// loadout: 1 pistol/chainsword, 0 primary (bolter), -1 unknown.
// Output: one line per swing (with sword or gun), veto, flick, wind-up and guard start, then
// a summary line.
#include "VrSwingMelee.h"
#include "VrHolster.h"
#include "VrGuard.h"
#include "VrCombatRouting.h"
#include "VrLeftPush.h"
#include "VrGrenadeThrow.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Clock=VrSwingMelee::Clock;
vr_camera::Matrix basis(const float* p,const float* f) {
    float z[3]={-f[0],-f[1],-f[2]};float n=std::sqrt(z[0]*z[0]+z[1]*z[1]+z[2]*z[2]);
    if(!(n>1e-4f)) return {};
    for(float& v:z) v/=n;
    const float up[3]={std::abs(z[1])>.95f?1.f:0.f,std::abs(z[1])>.95f?0.f:1.f,0};
    float x[3]={up[1]*z[2]-up[2]*z[1],up[2]*z[0]-up[0]*z[2],up[0]*z[1]-up[1]*z[0]};
    n=std::sqrt(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]);for(float& v:x) v/=n;
    const float y[3]={z[1]*x[2]-z[2]*x[1],z[2]*x[0]-z[0]*x[2],z[0]*x[1]-z[1]*x[0]};
    return {x[0],x[1],x[2],0, y[0],y[1],y[2],0, z[0],z[1],z[2],0, p[0],p[1],p[2],1};
}
}

int main(int argc,char** argv) {
    if(argc<2) { std::cerr<<"usage: melee_replay SAMPLES.txt [--old | --no-strokes | --no-zones | --no-two-handed | --no-gun]\n";return 2; }
    bool old=false,strokes=true,zones=true,two_handed=true,gun=true,old_emote=false,power_hold=true,fresh_rearm=true,pushes_on=true,grenade_on=true,belt_latch=false;
    double dump_from=1,dump_to=0;std::vector<std::string> sets;
    for(int i=2;i<argc;++i) { const std::string a=argv[i];
        if(a=="--dump" && i+2<argc) { dump_from=std::atof(argv[i+1]);dump_to=std::atof(argv[i+2]);i+=2; }
        else if(a=="--set" && i+1<argc) sets.push_back(argv[++i]);
        else if(a=="--no-power-hold") power_hold=false; else if(a=="--no-fresh-rearm") fresh_rearm=false;
        else if(a=="--no-push") pushes_on=false;
        else if(a=="--no-grenade") grenade_on=false; else if(a=="--guard-belt-latch") belt_latch=true;
        else if(a=="--old") old=true; else if(a=="--no-strokes") strokes=false; else if(a=="--no-zones") zones=false;
        else if(a=="--no-two-handed") two_handed=false; else if(a=="--no-gun") gun=false; else if(a=="--old-emote") old_emote=true; }
    if(old) { strokes=zones=two_handed=gun=power_hold=fresh_rearm=pushes_on=grenade_on=false;old_emote=true; }
    std::ifstream in{argv[1]};
    if(!in) { std::cerr<<"cannot open "<<argv[1]<<"\n";return 2; }
    VrSwingMelee swing;swing.set_stroke_rules(strokes);swing.set_fresh_rearm(fresh_rearm);
    VrHandMotion left_motion;VrTwoHanded hands;VrHolster holster;VrGuard guard;VrCombatRouting routing;VrGunMeleeGate gun_gate;VrPowerTrigger power_trigger;VrLeftPush push;VrGrenadeThrow grenade;
    for(const auto& set:sets) {
        const auto dot=set.find('.'),eq=set.find('=');
        if(dot==std::string::npos || eq==std::string::npos || eq<dot) { std::cerr<<"bad --set "<<set<<"\n";return 2; }
        const auto group=set.substr(0,dot),name=set.substr(dot+1,eq-dot-1);const float value=std::strtof(set.c_str()+eq+1,nullptr);bool done=false;
        if(group=="guard") { auto s=guard.settings();for(size_t k=0;k<VrGuard::PARAMETERS.size();++k) if(name==VrGuard::PARAMETERS[k].name) { s.values.values[k]=value;done=guard.set_settings(s); } }
        else if(group=="holster") { auto s=holster.settings();for(size_t k=0;k<VrHolster::PARAMETERS.size();++k) if(name==VrHolster::PARAMETERS[k].name) { s.values[k]=value;done=holster.set_settings(s); } }
        if(!done) { std::cerr<<"cannot set "<<set<<"\n";return 2; }
    }
    if(old_emote) { auto s=holster.settings();s.values[6]=0.15f;holster.set_settings(s);holster.set_raise_lead(10); } // 19 Oct emote: 0.15 m, fist owned anywhere
    uint64_t lines{},swings{},guns{},power{},vetoes{},flicks{},guards{},windups{},emotes{},pushes{},selections{};
    std::string line;
    while(std::getline(in,line)) {
        std::istringstream s{line};
        double t;uint64_t epoch;int armed,loadout,rv,lv,support=0;float rt,grip,lt,h[6],r[6],l[6],left_grip=0;
        if(!(s>>t>>epoch>>armed>>loadout>>rt>>grip>>lt)) continue;
        for(float& v:h) s>>v;
        s>>rv;for(float& v:r) s>>v;
        s>>lv;for(float& v:l) s>>v;
        if(!s) continue;
        if(!(s>>support)) support=0;
        else if(!(s>>left_grip)) left_grip=0;
        ++lines;
        const std::optional<bool> secondary=loadout>0?std::optional{true}:loadout==0?std::optional{false}:std::nullopt;
        const auto now=Clock::time_point{std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>{t+1000})};
        const auto head=basis(h,h+3);
        const std::optional<vr_camera::Matrix> right=rv?std::optional{basis(r,r+3)}:std::nullopt,left=lv?std::optional{basis(l,l+3)}:std::nullopt;
        const bool valid=vr_camera::rigid(head);
        const auto salutes=holster.snapshot().salutes;
        const auto selected=holster.snapshot().primary+holster.snapshot().secondary;
        holster.update(valid?std::optional{head}:std::nullopt,right,grip,armed!=0,now,secondary,rt<=.05f && lt<=.05f);
        if(holster.snapshot().primary+holster.snapshot().secondary!=selected) { ++selections;std::printf("select %.3f %s\n",t,holster.snapshot().last_slot==VrHolster::Slot::primary?"primary":"secondary"); }
        if(holster.snapshot().salutes!=salutes) { ++emotes;std::printf("emote %.3f\n",t); }
        const bool two=two_handed && hands.update(right,left,now);
        // As VrSession: the grenade reads the left grip first; a held primary's support squeeze is not a grab.
        grenade.update(valid?std::optional{head}:std::nullopt,left,left_grip,grenade_on && armed!=0 && !(support!=0 && left_grip>VrGrenadeThrow::RELEASE && secondary==std::optional{false}),now);
        const bool grenade_busy=grenade_on && grenade.busy(),at_belt=grenade_on && grenade.owns_grip() && !grenade_busy;
        const bool guard_allowed=armed!=0 && !two && !grenade_busy;
        const auto guards_before=guard.snapshot().guards;
        guard.poll(valid?std::optional{head}:std::nullopt,left,lt,guard_allowed,at_belt,belt_latch,now);
        if(guard.snapshot().guards!=guards_before) { ++guards;std::printf("guard %.3f\n",t); }
        VrMeleePoll p;
        if(valid && right) p.right=VrSwingMelee::Pose{*right,head,now,epoch+1};
        if(valid && left) p.left=VrSwingMelee::Pose{*left,head,now,epoch+1};
        const bool sword=old?secondary==std::optional{true}:routing.sword_out(secondary,now);
        const bool quiet=gun_gate.update(rt,lt,support!=0,now);
        p.gun=gun && !sword && (old?secondary==std::optional{false}:routing.gun_out(secondary,now));
        const bool hard=zones?(p.gun?holster.quiets_gun_swing(now):holster.quiets_swing(now)):holster.suppresses_swing(now);
        p.allowed=armed && (sword || (p.gun && quiet)) && !hard;
        p.zone_block=zones && holster.blocks_swing();
        p.two_handed=two;p.trigger=rt;
        p.trigger_held=power_hold?power_trigger.update(rt,now):std::chrono::hours(1);
        const auto before=swing.snapshot();
        vr_melee_poll(swing,left_motion,p,now);
        const auto after=swing.snapshot();
        if(after.swings!=before.swings) {
            ++swings;power+=after.power;guns+=p.gun;guard.yield(now);
            std::printf("swing %.3f %s %.2f %s\n",t,after.power?"power":"light",after.speed,p.gun?"gun":"sword");
        }
        if(after.vetoes!=before.vetoes) { ++vetoes;std::printf("veto %.3f %.2f\n",t,after.other_hand_speed); }
        if(after.flicks!=before.flicks) { ++flicks;std::printf("flick %.3f\n",t); }
        if(after.windups!=before.windups) { ++windups;std::printf("windup %.3f\n",t); }
        VrLeftPush::Input pi{.head=valid?std::optional{head}:std::nullopt,.hand=left,.trigger=lt,.grenade=grenade_on && grenade.owns_grip(),.right_speed=after.wrist_speed,
            .guard=guard.owns_hand(),.allowed=pushes_on && armed && sword && !two && !after.pressed};
        const auto pushes_before=push.snapshot().pushes;push.update(pi,now);
        if(push.snapshot().pushes!=pushes_before) { ++pushes;std::printf("push %.3f %.2f %.2f %.2f\n",t,push.snapshot().forward,push.snapshot().travel,push.snapshot().peak); }
        if(t>=dump_from && t<=dump_to)
            std::printf("dump %.3f armed=%d sword=%d gun=%d hard=%d zone_block=%d two=%d allowed=%d ready=%d pressed=%d speed=%.2f travel=%.2f rt=%.2f\n",t,armed,sword,p.gun,hard,p.zone_block,two,p.allowed,after.ready,after.pressed,after.speed,after.travel,rt);
        VrControllerSample routed;routed.fire=rt;routed.aim=lt;routing.update(routed,secondary,support!=0,now);
    }
    std::printf("summary rules=%s samples=%llu swings=%llu gun=%llu power=%llu vetoes=%llu flicks=%llu windups=%llu guards=%llu emotes=%llu pushes=%llu selections=%llu\n",old?"old":"new",
        (unsigned long long)lines,(unsigned long long)swings,(unsigned long long)guns,(unsigned long long)power,(unsigned long long)vetoes,
        (unsigned long long)flicks,(unsigned long long)windups,(unsigned long long)guards,(unsigned long long)emotes,(unsigned long long)pushes,(unsigned long long)selections);
    return 0;
}
