#include "VrCombatRouting.h"
#include "VrSupportGrip.h"
#include "UnknownGrace.h"
#include <iostream>
#include <stdexcept>
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
auto at(float x,float y,float z){auto m=vr_camera::IDENTITY;m[12]=x;m[13]=y;m[14]=z;return m;}
int main(){try{
    using namespace std::chrono_literals;
    auto t=VrCombatRouting::Clock::now();const auto step=[&]{t+=20ms;return t;};
    VrCombatRouting routing;VrControllerSample s;
    routing.update(s,false,false,step());s.fire=.8f;s.aim=.3f;routing.update(s,false,false,step());
    require(s.fire==.8f && s.aim==.3f && !s.chainsword_rev,"rifle keeps right fire and left aim, and never revs");
    s.fire=.8f;s.aim=.9f;routing.update(s,true,false,step());
    require(!s.fire && !s.aim && !s.chainsword_rev,"weapon switch with held triggers cannot fire or rev");
    s={};routing.update(s,true,false,t+=VrCombatRouting::REV_HOLDOVER);
    require(routing.snapshot().confirmed==std::optional{true},"a loadout held for the hold-over is confirmed");s.aim=.7f;s.fire=.8f;routing.update(s,true,false,step());
    require(s.fire==.7f && !s.aim && s.chainsword_rev==.8f,"pistol uses left trigger and reserves right for rev without a native attack");
    // A short loss of the gun attachment mid-rev (Joe's 20 October OpenXR trace) keeps revving.
    s={};s.fire=.8f;routing.update(s,std::nullopt,false,step());
    require(!s.fire && s.chainsword_rev==.8f && routing.snapshot().rev_holdover,"a 20 ms attachment loss holds the rev over");
    s={};s.fire=.8f;s.aim=.7f;routing.update(s,true,false,step());
    require(!s.fire && s.chainsword_rev==.8f,"attachment recovery resumes rev without a release; the pistol shot still needs one");
    s={};s.fire=.8f;routing.update(s,std::nullopt,false,t+=VrCombatRouting::REV_HOLDOVER+1ms);
    require(!s.fire && !s.chainsword_rev && !routing.snapshot().rev_holdover,"stale weapon ownership beyond the hold-over releases fire/rev");
    s={};s.fire=.8f;routing.update(s,true,false,step());
    require(s.chainsword_rev==.8f && routing.snapshot().confirmed==std::optional{true},"a held rev resumes when the same chainsword loadout is confirmed again");
    // Live 20 October OpenXR test driver: a one-frame false rifle reading stopped a held rev.
    s={};s.fire=.8f;routing.update(s,false,false,step());
    require(!s.fire && s.chainsword_rev==.8f && routing.snapshot().confirmed==std::optional{true},"a one-frame false rifle reading keeps the rev and cannot fire");
    s={};s.fire=.8f;routing.update(s,true,false,step());
    require(s.chainsword_rev==.8f && routing.snapshot().rev_rearmed,"after the flicker the held rev carries on without a release");
    s={};s.fire=.8f;routing.update(s,false,false,step());s={};s.fire=.8f;routing.update(s,false,false,t+=VrCombatRouting::REV_HOLDOVER);
    require(!s.fire && !s.chainsword_rev && routing.snapshot().confirmed==std::optional{false},"a held rev ends on a genuine rifle switch and cannot become a shot");
    s={};s.fire=.8f;routing.update(s,true,false,step());require(!s.chainsword_rev,"an unconfirmed return to the chainsword does not rev");
    s={};s.fire=.8f;routing.update(s,true,false,t+=VrCombatRouting::REV_HOLDOVER);
    require(!s.chainsword_rev && !routing.snapshot().rev_rearmed,"returning to the chainsword with the trigger held waits for a release");
    s={};routing.update(s,true,false,step());s.fire=.8f;routing.update(s,true,false,step());require(s.chainsword_rev==.8f,"release re-arms the rev");
    // Live 19:25 run: a 280 ms render stall reset routing mid-hold; the rev stayed dead until release.
    routing.reset();s={};s.fire=.8f;s.aim=.8f;routing.update(s,true,false,t+=300ms);
    require(s.chainsword_rev==.8f && !s.fire,"after a stall reset the held rev resumes on the same loadout; the pistol shot still needs a release");
    routing.reset();s={};s.fire=.8f;routing.update(s,false,false,t+=VrCombatRouting::REV_HOLDOVER+1ms);
    require(!s.fire && !s.chainsword_rev,"after a reset a rifle reading neither fires a held trigger nor revs past the hold-over");
    VrCombatRouting fresh;s={};s.fire=.8f;fresh.update(s,std::nullopt,false,step());
    require(!s.chainsword_rev && !fresh.snapshot().confirmed,"no confirmed loadout yet: no hold-over");
    s={};s.fire=.6f;routing.update(s,false,false,step());routing.update(s,false,false,step());s={};routing.update(s,false,false,step());s.fire=.6f;routing.update(s,false,true,step());
    require(s.fire==.6f && s.aim==1,"support grip drives actual native aim while firing");
    VrSupportGrip support;const auto right=at(.2f,1.3f,-.3f),left=at(.1f,1.25f,-.6f);
    require(!support.update(right,left,1,true),"held grip on recovery must release first");
    support.update(right,left,0,true);require(support.update(right,left,1,true),"left grip under barrel starts support");
    require(support.update(right,at(.1f,1.25f,-.6f),.5f,true),"grip hysteresis holds aim");
    require(!support.update(right,left,0,true),"grip release ends aim");
    support.reset();
    require(support.snapshot().engagements==1,"one support hold counted once, kept through reset (the log shows whether support ever engaged)");
    support.update(right,left,0,true);
    auto changed=support.settings();changed.values.values[VrSupportGrip::RADIUS]=.05f;
    require(support.set_settings(changed),"bounded support radius is accepted");support.update(right,left,0,true);
    require(!support.update(right,left,1,true),"changing radius changes the production zone result");
    changed={};support.set_settings(changed);
    for(float yaw:{0.f,1.5707963f,3.1415926f}) {
        auto rotate=vr_camera::IDENTITY;rotate[0]=rotate[10]=std::cos(yaw);rotate[2]=std::sin(yaw);rotate[8]=-std::sin(yaw);rotate[12]=4;
        auto r=vr_camera::multiply(right,rotate),l=vr_camera::multiply(left,rotate);
        support.reset();support.update(r,l,0,true);require(support.update(r,l,1,true),"support zone survives shared world translation and turn");
        require(!support.update(r,l,1,false),"pistol/grenade/focus exclusion releases support");
    }
    {   // Live 21 Oct driver run: the attachment reading dropped for one or two polls during
        // each native attack, and the raw gate reset the sword mid-combo.
        VrCombatRouting sword;auto now=VrCombatRouting::Clock::now();VrControllerSample x;
        require(sword.sword_out(true,now),"an observed pistol/chainsword loadout is sword out");
        sword.update(x,true,false,now);x={};sword.update(x,true,false,now+=VrCombatRouting::REV_HOLDOVER);
        require(sword.sword_out(std::nullopt,now+50ms) && sword.sword_out(false,now+50ms),"a brief missing or false reading keeps the sword");
        require(!sword.sword_out(std::nullopt,now+VrCombatRouting::REV_HOLDOVER+1ms),"beyond the hold-over it is gone");
        VrCombatRouting rifle;x={};rifle.update(x,false,false,now);require(!rifle.sword_out(std::nullopt,now) && !rifle.sword_out(false,now),"the rifle is never the sword");
    }
    {   // 29 Oct battle simulator: the bolter's reading dropped out during each native attack too,
        // and a raw gate reset the swing detector so the next stroke never armed.
        VrCombatRouting gun;auto now=VrCombatRouting::Clock::now();VrControllerSample x;
        require(gun.gun_out(false,now),"an observed primary is the bolter out");
        gun.update(x,false,false,now);
        require(gun.gun_out(std::nullopt,now+80ms),"a brief missing reading keeps the bolter");
        require(!gun.gun_out(std::nullopt,now+VrCombatRouting::REV_HOLDOVER+1ms),"beyond the hold-over it is gone");
        require(!gun.gun_out(true,now+10ms),"the pistol/chainsword reading is never the bolter");
        VrCombatRouting sword;x={};sword.update(x,true,false,now);require(!sword.gun_out(std::nullopt,now+10ms),"a missing reading after the sword is not the bolter");
    }
    {   // GENERIC_GUARDS piece 5: a held weapon the attachment check never recognises (25 Oct battle
        // simulator, "couldn't fire the heavy gun") left both triggers dead for good. Past its grace
        // the unknown loadout routes like the primary; entering that needs both triggers released.
        const auto now=VrCombatRouting::Clock::now();VrCombatRouting unknown;VrControllerSample x;
        x.fire=.9f;x.aim=.6f;unknown.update(x,std::nullopt,false,now);require(!x.fire && !x.aim,"a brief unknown (a switch in progress): no shot");
        x={};x.fire=.9f;x.aim=.6f;unknown.update(x,std::nullopt,false,now+2s,true);
        require(!x.fire && !x.aim && unknown.unknown_as_primary(),"the grace elapsing with the triggers held is a loadout change: no shot until released");
        x={};unknown.update(x,std::nullopt,false,now+2100ms,true);x.fire=.9f;x.aim=.6f;unknown.update(x,std::nullopt,false,now+2200ms,true);
        require(x.fire==.9f && x.aim==.6f && !x.chainsword_rev,"after a release the right trigger fires and the left aims, as for the primary");
        x={};x.fire=.9f;x.aim=.2f;unknown.update(x,std::nullopt,true,now+2300ms,true);require(x.fire==.9f && x.aim==1,"two-handed support aims fully, as for the primary");
        x={};x.fire=.9f;unknown.update(x,false,false,now+2400ms,true);require(!x.fire && !unknown.unknown_as_primary(),"the weapon becomes known: an ordinary loadout change, release first");
        x={};unknown.update(x,false,false,now+2500ms);x.fire=.9f;unknown.update(x,false,false,now+2600ms);require(x.fire==.9f,"then the rifle fires as before");
        x={};x.fire=.9f;unknown.update(x,true,false,now+2700ms,true);require(!x.fire && !unknown.unknown_as_primary(),"a known weapon ignores the grace flag");
        // The grace itself.
        UnknownGrace grace{std::chrono::milliseconds(2000)};
        require(!grace.update(false,now) && !grace.update(false,now+1999ms) && grace.update(false,now+2s),"unknown for the grace: act on the default");
        require(!grace.update(true,now+3s) && !grace.update(false,now+3s) && !grace.update(false,now+4999ms) && grace.update(false,now+5s),"known in between starts it over");
        require(!grace.update(false,now+1s),"a clock that goes back starts it over, never acts early");
        grace.reset();require(!grace.update(false,now+10s) && grace.grace()==2000ms,"reset");
    }
    {   // Gun strike (30 Oct): after a parry press or a dodge either trigger fires, with either loadout.
        VrCombatRouting strike;VrControllerSample p;auto now=VrCombatRouting::Clock::now();
        const auto poll=[&](VrControllerSample value,std::optional<bool> secondary,std::chrono::milliseconds after){now+=after;strike.update(value,secondary,false,now);return value;};
        // Bolter out: the left trigger aims, except after a parry, when it fires.
        poll({},false,20ms);p={};p.aim=.9f;auto out=poll(p,false,20ms);
        require(out.aim==.9f && !out.fire && !strike.snapshot().strike,"bolter, no parry: left aims");
        poll({},false,20ms);p={};p.guard=true;out=poll(p,false,20ms);require(strike.snapshot().strike,"a guard press opens the window");
        p={};p.aim=.9f;out=poll(p,false,500ms);require(out.fire==.9f && !out.aim,"bolter, 0.5 s after the guard: the left trigger is the gun strike");
        poll({},false,20ms);p={};p.aim=.9f;out=poll(p,false,VrCombatRouting::STRIKE_WINDOW);
        require(out.aim==.9f && !out.fire && !strike.snapshot().strike,"after the window the left trigger aims again");
        // Pistol out: the right trigger revs, except after a parry or a dodge, when it fires.
        strike.reset();poll({},true,20ms);poll({},true,VrCombatRouting::REV_HOLDOVER);p={};p.fire=.8f;out=poll(p,true,20ms);
        require(out.chainsword_rev==.8f && !out.fire,"pistol, no parry: right revs");
        poll({},true,20ms);p={};p.parry=1;poll(p,true,20ms);p={};p.fire=.8f;out=poll(p,true,300ms);
        require(out.fire==.8f && !out.chainsword_rev,"pistol, after a left-grip parry: the right trigger is the gun strike");
        poll({},true,VrCombatRouting::STRIKE_WINDOW);p={};p.confirm=true;poll(p,true,20ms);poll({},true,20ms);p={};p.aim=.7f;out=poll(p,true,200ms);
        require(out.fire==.7f,"after a dodge the pistol's own trigger still fires");
        // A trigger held through a reset still needs a release first.
        strike.reset();p={};p.guard=true;p.fire=1;out=poll(p,false,20ms);require(!out.fire,"a trigger held through a reset never fires");
    }
    std::cout<<"weapon trigger ownership, rev reservation and support-grip aim passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
