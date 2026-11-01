#include "VrWeaponAim.h"
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value,const char* why) {if(!value) throw std::runtime_error(why);}
bool near(const vr_arm::Matrix& a,const vr_arm::Matrix& b) {for(size_t i=0;i<a.size();++i) if(std::abs(a[i]-b[i])>1e-4f) return false;return true;}
auto at(float x,float y,float z) {auto result=vr_camera::IDENTITY;vr_arm::position(result,{x,y,z});return result;}
}
int main(int argc,char** argv) {try {
    const std::vector<std::string> names{"ROOT","BACKup","ARM2R","HANDR","GUN_ANIM","Gun_pose","OTHER"};
    std::vector<int32_t> parents{-1,0,1,2,1,4,0};
    std::vector<vr_arm::Matrix> original{at(10,0,20),at(0,1,0),at(.3f,.3f,0),at(.4f,0,0),at(.1f,.2f,.3f),at(.2f,0,0),at(0,0,3)};
    auto changed=original;changed[3]=at(.1f,.2f,.3f);changed[3][0]=changed[3][10]=0;changed[3][2]=-1;changed[3][8]=1;
    const auto output=vr_weapon::retarget(original,changed,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
    require(output && output->index==5,"resolve the independent gun branch by name");
    std::vector<vr_arm::Matrix> before,after;vr_arm::hierarchy(original,parents,vr_camera::IDENTITY,before);vr_arm::hierarchy(changed,parents,vr_camera::IDENTITY,after);
    const auto native_grip=vr_camera::multiply(before[5],vr_camera::inverse(before[3]));
    require(near(output->world,vr_camera::multiply(native_grip,after[3])),"gun must keep authored right-wrist grip while following translation and rotation");
    auto applied=changed;applied[output->index]=output->local;std::vector<vr_arm::Matrix> committed;vr_arm::hierarchy(applied,parents,vr_camera::IDENTITY,committed);
    require(near(committed[5],output->world) && applied[4]==original[4] && applied[6]==original[6],"attachment local reconstructs driven world while leaving spine, animation parent and unrelated bones unchanged");
    for(size_t i=0;i<vr_weapon::PARAMETERS.size();++i) {
        vr_weapon::Settings settings;settings.values[i]=i<3?.1f:20;
        const auto tuned=vr_weapon::retarget(original,changed,parents,names,vr_camera::IDENTITY,settings.matrix(),.9f);
        require(tuned && !near(tuned->world,output->world),"every live gun-grip setting changes the production attachment output");
    }
    auto broken=parents;broken[4]=999;require(!vr_weapon::retarget(original,changed,broken,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1),"invalid ancestry rejects before indexing");
    auto duplicate=names;duplicate[6]="Gun_pose";require(!vr_weapon::retarget(original,changed,parents,duplicate,vr_camera::IDENTITY,vr_camera::IDENTITY,1),"duplicate named attachment is ambiguous");
    {
        const std::vector<std::string> dual_names{"ROOT","BACKup","HANDR","HANDL","GUN_ANIM","Gun_pose","GUN_L","GUN_R"};
        const std::vector<int32_t> dual_parents{-1,0,1,1,1,4,3,2};
        std::vector<vr_arm::Matrix> native_dual{at(0,0,0),at(0,1,0),at(.8f,0,0),at(-.8f,0,0),at(0,0,0),at(-.8f,0,-.3f),at(0,0,-.3f),at(0,0,-.1f)};
        auto driven=native_dual;driven[2]=at(.25f,.5f,-.5f);driven[3]=at(-.25f,.5f,-.5f);
        auto dual=vr_weapon::retarget(native_dual,driven,dual_parents,dual_names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        require(dual && dual->hand==0,"pistol socket identity selects the native left grip");
        {
            auto firing=native_dual;firing[5]=at(1.2f,.6f,.2f);
            const auto recoil=vr_weapon::retarget(firing,driven,dual_parents,dual_names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,0);
            require(recoil && recoil->hand==0 && near(recoil->world,dual->world),"pistol firing off both sockets retains left ownership and the stable socket grip");
            // Reproduce the missing case: the actual hand socket rotates too.
            firing[6][0]=firing[6][10]=-1;
            const auto stable=vr_weapon::retarget(firing,driven,dual_parents,dual_names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,dual->hand,dual->relative);
            require(stable && near(stable->world,dual->world),"pistol keeps the complete committed grip while firing animation flips its socket");
            firing[5]=at(.8f,0,-.1f);
            const auto primary=vr_weapon::retarget(firing,driven,dual_parents,dual_names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,dual->hand,dual->relative);
            require(primary && primary->hand==1,"actual primary switch does not reuse the left-hand pistol fit");
            auto rifle_fire=firing;rifle_fire[5]=at(2,2,2);rifle_fire[7][0]=rifle_fire[7][10]=-1;
            const auto rifle=vr_weapon::retarget(rifle_fire,driven,dual_parents,dual_names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,primary->hand,primary->relative);
            require(rifle && near(rifle->world,primary->world),"primary firing socket rotation cannot flip a one-handed bolter");
            auto turned=driven;turned[2][0]=turned[2][10]=-1;
            const auto moved=vr_weapon::retarget(rifle_fire,turned,dual_parents,dual_names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,primary->hand,primary->relative);
            require(moved && !near(moved->world,rifle->world),"stable authored grip still follows current tracked hand rotation");
            require(!vr_weapon::retarget(rifle_fire,driven,dual_parents,dual_names,vr_camera::IDENTITY,vr_camera::IDENTITY,1),"first observation during an unowned firing transition cannot learn a random fit");
        }
        require(near(dual->world,at(-.25f,1.5f,-.8f)),"pistol keeps its own 30cm grip offset at the left wrist instead of the 1.6m hand separation");
        auto committed_dual=driven;committed_dual[5]=dual->local;
        std::vector<vr_arm::Matrix> dual_world;vr_arm::hierarchy(committed_dual,dual_parents,vr_camera::IDENTITY,dual_world);
        require(near(dual_world[5],dual->world) && near(dual_world[7],at(.25f,1.5f,-.6f)) && committed_dual[2]==driven[2] && committed_dual[3]==driven[3],
            "pistol local reconstructs its world while chainsword remains on the native right wrist");
        // Preserve authored grip orientation through the calibrated left wrist.
        auto left_grip=vr_camera::IDENTITY;left_grip[0]=left_grip[10]=-1;
        native_dual[3]=vr_camera::multiply(left_grip,at(-.8f,0,0));
        native_dual[5]=at(-.8f,0,.3f);native_dual[5][0]=native_dual[5][10]=-1;
        driven=native_dual;driven[2]=at(.25f,.5f,-.5f);driven[3]=vr_camera::multiply(left_grip,at(-.25f,.5f,-.5f));
        dual=vr_weapon::retarget(native_dual,driven,dual_parents,dual_names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        auto expected=vr_camera::multiply(at(0,0,-.3f),vr_camera::multiply(left_grip,at(-.25f,1.5f,-.5f)));
        require(dual && near(dual->world,expected),"left wrist calibration preserves the pistol's authored controller-relative barrel direction");
        native_dual[5]=at(.8f,0,-.1f);
        dual=vr_weapon::retarget(native_dual,driven,dual_parents,dual_names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        require(dual && dual->hand==1,"switching to a right socket changes weapon hand on that same native pose");
    }
    {
        // The Heavy (26 Oct playtest). Same rig as above: HANDR 2, HANDL 3, Gun_pose 5, GUN_L 6, GUN_R 7.
        using vr_weapon::Grip;
        const std::vector<std::string> names{"ROOT","BACKup","HANDR","HANDL","GUN_ANIM","Gun_pose","GUN_L","GUN_R"};
        const std::vector<int32_t> parents{-1,0,1,1,1,4,3,2};
        const auto sockets=[&](float right_x,float left_z,vr_arm::Matrix gun){
            return std::vector<vr_arm::Matrix>{at(0,0,0),at(0,1,0),at(right_x,0,0),at(0,0,left_z),at(0,0,0),gun,at(0,0,0),at(0,0,-.1f)};};
        auto driven=sockets(.3f,-.2f,at(0,0,-.2f));driven[2]=at(.25f,.5f,-.5f);driven[3]=at(-.3f,.4f,-.9f);
        // A heavy weapon rides GUN_L (here 0,1,-0.2) with GUN_R (0.3,1,-0.1) 32 cm away: both hands on it.
        const auto heavy=sockets(.3f,-.2f,at(0,0,-.2f));
        const auto two=vr_weapon::retarget(heavy,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        require(two && two->hand==1 && two->grip==Grip::two_handed,"a weapon on the left socket with the right hand on it is a primary, not the pistol");
        require(near(two->world,at(-.05f,1.5f,-.7f)),"the right hand drives it with the native gun-to-right-hand fit, wherever the left hand is");
        // The pistol: also on GUN_L, but the right hand 57 cm away (the nearest seen live was 56 cm).
        const auto pistol=vr_weapon::retarget(sockets(.56f,-.2f,at(0,0,-.2f)),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        require(pistol && pistol->hand==0 && pistol->grip==Grip::left,"a weapon on GUN_L with the right hand away is still the pistol");
        // The other heavy weapons sat 2 cm and 9 degrees off GUN_R (here 3 cm): the old check (2 cm and 0.02 per
        // axis) never matched, so there was nothing to learn from and no gun in the hand.
        auto off=at(.33f,0,-.1f);const float c=std::cos(.157f),s=std::sin(.157f);off[0]=off[10]=c;off[2]=-s;off[8]=s;
        const auto rotated=vr_weapon::retarget(sockets(.3f,-.9f,off),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        require(rotated && rotated->hand==1 && rotated->grip==Grip::right,"a gun near GUN_R and 9 degrees off it reads as the primary");
        {
            // 27 Oct evening, an execution with the pistol: Gun_pose on GUN_L, GUN_R 2 cm away turned 150
            // degrees. Position alone read right, and the pistol went to the right hand.
            const float c150=std::cos(2.618f),s150=std::sin(2.618f);
            auto right_socket=at(.02f,1,-.1f);right_socket[0]=right_socket[10]=c150;right_socket[2]=-s150;right_socket[8]=s150;
            require(vr_weapon::read_grip(at(0,1,-.1f),at(0,1,-.1f),right_socket,1)==Grip::left,"a gun on GUN_L is the pistol when GUN_R is near but turned away");
            require(vr_weapon::read_grip(at(.02f,1,-.1f),at(0,1,-.1f),at(.02f,1,-.1f),1)==Grip::right,"on GUN_R and lined up with it: still the primary");
        }
        require(near(rotated->world,at(.25f,1.5f,-.6f)),"its grip is learned from the GUN_R socket, not the rotated Gun_pose");
        require(!vr_weapon::retarget(sockets(.3f,-.9f,at(.36f,0,-.1f)),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1),
            "6 cm from every socket is still between sockets: nothing learned");
        // A grip change on the same hand relearns the fit: bolter-style GUN_R to the heavy's GUN_L.
        const auto bolter=vr_weapon::retarget(sockets(.3f,-.9f,at(.3f,0,-.1f)),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        require(bolter && bolter->grip==Grip::right,"fixture: a primary on GUN_R");
        const auto relearned=vr_weapon::retarget(heavy,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,bolter->hand,bolter->relative,bolter->grip);
        require(relearned && relearned->grip==Grip::two_handed && near(relearned->world,two->world),"a bolter's fit is not reused for a heavy weapon held differently");

        // GripSwitch: the reading that flapped (bone dumps: GUN_L, then GUN_R for a few frames).
        using namespace std::chrono_literals;
        const auto t0=vr_weapon::GripSwitch::Clock::time_point{}+10s;
        vr_weapon::GripSwitch sw;
        require(!sw.update(std::nullopt,t0),"nothing read, nothing held");
        require(sw.update(Grip::left,t0)==Grip::left,"the first reading is taken at once");
        require(sw.update(Grip::right,t0+10ms)==Grip::left && sw.update(Grip::right,t0+200ms)==Grip::left,"a different reading under HOLD changes nothing");
        require(sw.update(Grip::left,t0+210ms)==Grip::left && sw.update(Grip::right,t0+220ms)==Grip::left && sw.update(Grip::right,t0+460ms)==Grip::left,
            "the current reading cancels a pending change: the hold starts again");
        require(sw.update(std::nullopt,t0+300ms)==Grip::left && sw.update(Grip::right,t0+470ms)==Grip::right,
            "between-socket polls neither confirm nor cancel; HOLD of the new reading switches");
        require(sw.switches()==1,"one settled change counted");
        // Through retarget: a reading flicker never moves the gun to the other hand.
        vr_weapon::GripSwitch held;
        auto first=vr_weapon::retarget(heavy,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,std::nullopt,std::nullopt,std::nullopt,&held,t0);
        require(first && first->grip==Grip::two_handed,"fixture: heavy held");
        const auto flicker=vr_weapon::retarget(sockets(.56f,-.2f,at(0,0,-.2f)),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,first->hand,first->relative,first->grip,&held,t0+100ms);
        require(flicker && flicker->hand==1 && flicker->grip==Grip::two_handed && near(flicker->world,first->world),"a 100 ms pistol-like reading keeps the heavy in the right hand");
        // Joe's rule: a primary never goes to the left hand. Without a weapon switch the native carrying
        // the primary on GUN_L alone (a finisher, a knife strike) is no reading: hand and fit stay.
        const auto carried=vr_weapon::retarget(sockets(.56f,-.2f,at(0,0,-.2f)),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,first->hand,first->relative,first->grip,&held,t0+400ms);
        require(carried && carried->hand==1 && carried->grip==Grip::two_handed && near(carried->world,first->world),
            "a primary on GUN_L alone, with no weapon switch asked, stays in the right hand with its fit");
        require(held.refused()==2 && held.switches()==0,"both pistol-like frames were refused and nothing settled");
        held.weapon_switch(t0+1s);
        const auto pending=vr_weapon::retarget(sockets(.56f,-.2f,at(0,0,-.2f)),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,first->hand,first->relative,first->grip,&held,t0+1100ms);
        require(pending && pending->hand==1,"after a switch request the pistol still has to hold for HOLD");
        const auto switched=vr_weapon::retarget(sockets(.56f,-.2f,at(0,0,-.2f)),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,first->hand,first->relative,first->grip,&held,t0+1400ms);
        require(switched && switched->hand==0 && switched->grip==Grip::left,"a real switch to the pistol (D-pad down sent) lands after HOLD");
        const auto back=vr_weapon::retarget(heavy,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,switched->hand,switched->relative,switched->grip,&held,t0+10s);
        const auto back_held=vr_weapon::retarget(heavy,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,switched->hand,switched->relative,switched->grip,&held,t0+10300ms);
        require(back && back->hand==0 && back_held && back_held->hand==1 && back_held->grip==Grip::two_handed,
            "pistol to primary needs no switch window: a primary reading always takes the right hand after HOLD");

        // GripSwitch on its own: the window opens SWITCH_WINDOW after the request and only for a primary held.
        vr_weapon::GripSwitch rule;
        require(rule.update(Grip::right,t0)==Grip::right,"fixture: primary held");
        require(rule.update(Grip::left,t0+100ms)==Grip::right && rule.update(Grip::left,t0+5s)==Grip::right,"a left reading with no switch never replaces a primary");
        rule.weapon_switch(t0+6s);
        require(rule.update(Grip::left,t0+6s+100ms)==Grip::right && rule.update(Grip::left,t0+6s+400ms)==Grip::left,"inside the window the pistol lands after HOLD");
        vr_weapon::GripSwitch late;late.update(Grip::two_handed,t0);late.weapon_switch(t0);
        require(late.update(Grip::left,t0+vr_weapon::GripSwitch::SWITCH_WINDOW+1ms)==Grip::two_handed &&
            late.update(Grip::left,t0+vr_weapon::GripSwitch::SWITCH_WINDOW+400ms)==Grip::two_handed,"after the window closes the rule applies again");
        vr_weapon::GripSwitch pistol_first;
        require(pistol_first.update(Grip::left,t0)==Grip::left,"a pistol drawn when the actor appears is taken at once");
        // 30 Oct respawn (20:46:30): the bolter's new actor first read left. A primary survives the actor change.
        vr_weapon::GripSwitch respawn;
        require(respawn.update(Grip::right,t0)==Grip::right && respawn.update(Grip::left,t0+100ms)==Grip::right,"fixture: bolter held, left readings refused");
        respawn.new_actor();
        require(respawn.update(Grip::left,t0+5s)==Grip::right && respawn.update(Grip::left,t0+10s)==Grip::right,
            "after a respawn the bolter on GUN_L alone stays in the right hand");
        require(respawn.update(Grip::two_handed,t0+11s)==Grip::right && respawn.update(Grip::two_handed,t0+11300ms)==Grip::two_handed,
            "a different primary still lands after HOLD");
        respawn.weapon_switch(t0+12s);
        require(respawn.update(Grip::left,t0+12100ms)==Grip::two_handed && respawn.update(Grip::left,t0+12400ms)==Grip::left,"a real pistol draw still lands");
        // 30 Oct 20:42:12: died with the pistol out, came back with the bolter, which first read left.
        respawn.new_actor();
        require(respawn.update(Grip::left,t0+20s)==Grip::right && respawn.update(Grip::left,t0+21s)==Grip::right,
            "a pistol held before the respawn: the new actor holds the bolter, in the right hand");
        respawn.new_actor();
        require(respawn.update(Grip::right,t0+30s)==Grip::right,"and a primary first reading too");
        vr_weapon::GripSwitch spawn;spawn.new_actor();
        require(spawn.update(Grip::left,t0)==Grip::left,"with nothing held before, the first reading is still taken at once");

        // 30 Oct 20:37:09 and 20:37:11 (battle simulator, 30 Hz trace): the pistol out, two shoulder draws.
        // The first left GUN_L for 0.2 s and changed nothing; the second drew the bolter (off both sockets
        // for 1.6 s), then a parry put it on GUN_L alone (the melee carry) until a reload at 20:39:23.
        const auto none=std::optional<Grip>{};
        vr_weapon::GripSwitch draw;
        require(draw.update(Grip::left,t0)==Grip::left,"fixture: the pistol held");
        draw.weapon_switch(t0+1s);
        require(draw.update(Grip::left,t0+1s+30ms)==Grip::left,"the press");
        draw.weapon_switch(t0+1s+50ms); // Polled while the 0.1 s press is held: one request.
        require(draw.update(none,t0+1s+60ms)==Grip::left &&
            draw.update(none,t0+1s+260ms)==Grip::left && draw.update(Grip::left,t0+1s+270ms)==Grip::left &&
            draw.update(Grip::left,t0+2s)==Grip::left,"a press that leaves GUN_L for 0.2 s draws nothing: still the pistol");
        draw.weapon_switch(t0+3s);
        require(draw.update(Grip::left,t0+3s+20ms)==Grip::left && draw.update(none,t0+3s+60ms)==Grip::left &&
            draw.update(none,t0+3s+500ms)==Grip::left,"the gun off both sockets under DRAW_HOLD: not yet");
        require(draw.update(none,t0+3s+560ms)==Grip::right && draw.draws()==1,"off both sockets for DRAW_HOLD after the press: the bolter is drawn, right hand");
        require(draw.update(Grip::left,t0+4s+660ms)==Grip::right && draw.update(Grip::left,t0+5s)==Grip::right && draw.refused()==2,
            "the parry's one-handed carry on GUN_L, inside the same press's window, is refused: the press is spent");
        require(draw.update(Grip::left,t0+60s)==Grip::right,"and stays refused after it");
        // Without the draw rule the gun stayed with the pistol for 150 s (to 20:39:41): every left reading
        // confirmed the pistol and cancelled the rare right ones (a reload's 0.17 s on GUN_R at 20:39:24).
        vr_weapon::GripSwitch ignored;ignored.update(Grip::left,t0);ignored.weapon_switch(t0+1s);
        require(ignored.update(Grip::left,t0+1s+100ms)==Grip::left && ignored.update(Grip::left,t0+3s)==Grip::left && ignored.draws()==0,
            "a press the game ignored (20:42:03: the pistol never left GUN_L) keeps the pistol");
        vr_weapon::GripSwitch late_off;late_off.update(Grip::left,t0);late_off.weapon_switch(t0+1s);
        require(late_off.update(Grip::left,t0+1s+600ms)==Grip::left && late_off.update(none,t0+1s+700ms)==Grip::left &&
            late_off.update(none,t0+2s+500ms)==Grip::left,"the gun leaving GUN_L later than DRAW_START after the press is not the draw");
        vr_weapon::GripSwitch no_press;no_press.update(Grip::left,t0);
        require(no_press.update(none,t0+1s)==Grip::left && no_press.update(none,t0+5s)==Grip::left,"without a press the pistol off both sockets (recoil, sprint) stays the pistol");
        vr_weapon::GripSwitch held_primary;held_primary.update(Grip::right,t0);held_primary.weapon_switch(t0+1s);
        require(held_primary.update(none,t0+1s+50ms)==Grip::right && held_primary.update(none,t0+2s)==Grip::right && held_primary.draws()==0,
            "a press with a primary held asks for the pistol: off both sockets lands nothing");
        // A trackpad press held 1 s is polled every frame: still one request, so after the pistol lands
        // the rest of the hold does not reopen a draw back to the primary.
        vr_weapon::GripSwitch long_press;long_press.update(Grip::right,t0);
        std::optional<Grip> after_draw,at_end;
        for(auto t=t0+1s;t<=t0+2700ms;t+=10ms) {
            if(t<=t0+2s) long_press.weapon_switch(t);
            at_end=long_press.update(t<t0+1400ms?std::optional{Grip::left}:none,t); // The pistol drawn, then off both sockets.
            if(t==t0+1300ms) after_draw=at_end;
        }
        require(after_draw==Grip::left,"fixture: the pistol drawn while the trackpad is held");
        require(at_end==Grip::left && long_press.draws()==0,"one held press: no second switch back");
        // The heavy weapon flaps between right and two_handed: that settles but does not spend the press.
        vr_weapon::GripSwitch flap;flap.update(Grip::right,t0);flap.weapon_switch(t0+1s);
        require(flap.update(Grip::two_handed,t0+1s+10ms)==Grip::right && flap.update(Grip::two_handed,t0+1s+300ms)==Grip::two_handed,"fixture: a primary-to-primary change");
        require(flap.update(Grip::left,t0+1s+400ms)==Grip::two_handed && flap.update(Grip::left,t0+1s+700ms)==Grip::left,"the pistol drawn with the same press still lands");
        // Through retarget: the drawn bolter is driven by the right hand.
        vr_weapon::GripSwitch drawn;
        const auto pistol_held=vr_weapon::retarget(sockets(.56f,-.2f,at(0,0,-.2f)),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,std::nullopt,std::nullopt,std::nullopt,&drawn,t0);
        require(pistol_held && pistol_held->hand==0 && pistol_held->grip==Grip::left,"fixture: the pistol in the left hand");
        drawn.weapon_switch(t0+1s);
        const auto travelling=sockets(.3f,-.9f,at(.36f,0,-.1f)); // 6 cm from every socket: no reading.
        const auto mid=vr_weapon::retarget(travelling,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,pistol_held->hand,pistol_held->relative,pistol_held->grip,&drawn,t0+1s+50ms);
        require(mid && mid->hand==0,"the draw in progress keeps the pistol's hand");
        const auto landed=vr_weapon::retarget(travelling,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,mid->hand,mid->relative,mid->grip,&drawn,t0+1s+600ms);
        require(landed && landed->hand==1 && landed->grip==Grip::right && !landed->seated,"the drawn primary goes to the right hand, its fit waiting for GUN_R");
        const auto carried_left=vr_weapon::retarget(sockets(.56f,-.2f,at(0,0,-.2f)),driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,landed->hand,landed->relative,landed->grip,&drawn,t0+2s+700ms);
        require(carried_left && carried_left->hand==1 && carried_left->grip==Grip::right,"then carried on GUN_L alone (melee) it stays in the right hand");

        // 31 Oct 16:04 playtest. With the gun off it GUN_R sits in the chainsword's grip: 11 cm from HANDR,
        // 68 degrees and 27.5 cm from the carried gun, which is 33 cm from HANDR (16:12:03.957, 16:13:18.8).
        // The fit was learned from that socket, so the bolter was held and fired at the chainsword's angle,
        // up out of the hand. Once the gun is on GUN_R the two agree, and that fit is the seat.
        // Here HANDR is at (.3,1,0), GUN_R local to it, Gun_pose seated 30 cm along HANDR's x.
        const float c68=std::cos(1.187f),s68=std::sin(1.187f),c18=std::cos(.314f),s18=std::sin(.314f);
        const auto turned=[](float x,float c,float s){auto m=at(x,0,0);m[0]=m[10]=c;m[2]=-s;m[8]=s;return m;};
        const auto with_socket=[](std::vector<vr_arm::Matrix> rig,const vr_arm::Matrix& socket){rig[7]=socket;return rig;};
        const auto chainsword_socket=turned(.11f,c68,s68),seat=at(.3f,0,0);
        const auto carry=with_socket(sockets(.3f,-.9f,at(.6f,0,0)),chainsword_socket);
        const auto blending=with_socket(sockets(.3f,-.9f,at(.6f,0,0)),turned(.28f,c18,s18)); // 18 degrees off, 2 cm.
        const auto on_gun=with_socket(sockets(.3f,-.9f,at(.6f,0,0)),seat);
        const auto pistol_on_socket=sockets(.56f,-.2f,at(0,0,-.2f));
        auto out_of_reach=with_socket(sockets(.3f,-.9f,at(.3f+vr_weapon::CARRY_METRES+.05f,0,0)),chainsword_socket);
        const auto wanted=at(.55f,1.5f,-.5f); // The seat on the driven HANDR (.25,1.5,-.5).
        {
            std::vector<vr_arm::Matrix> world;vr_arm::hierarchy(carry,parents,vr_camera::IDENTITY,world);
            require(near(world[5],at(.6f,1,0)) && vr_weapon::turn_degrees(world[5],world[7])>60,"fixture: the carried gun off a turned GUN_R");
            require(!vr_weapon::read_grip(world[5],world[6],world[7],1),"fixture: carried off both sockets, no reading");
            vr_arm::hierarchy(blending,parents,vr_camera::IDENTITY,world);
            require(vr_weapon::read_grip(world[5],world[6],world[7],1)==Grip::right,"fixture: blending onto the gun reads right");
        }
        const auto step=[&](const std::vector<vr_arm::Matrix>& pose,const std::optional<vr_weapon::Attachment>& last,vr_weapon::GripSwitch& grip_switch,vr_weapon::GripSwitch::Clock::time_point when){
            return last?vr_weapon::retarget(pose,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,last->hand,last->relative,last->grip,&grip_switch,when,last->seated):
                vr_weapon::retarget(pose,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,std::nullopt,std::nullopt,std::nullopt,&grip_switch,when);};
        {
            // 16:13:18.8: the draw lands with the gun carried in the hand, 0.8 s before it reaches GUN_R.
            vr_weapon::GripSwitch grips;
            auto fit=step(pistol_on_socket,std::nullopt,grips,t0);
            require(fit && fit->grip==Grip::left && fit->seated,"fixture: the pistol in the left hand, on its socket");
            grips.weapon_switch(t0+1s);
            fit=step(travelling,fit,grips,t0+1s+50ms);
            fit=step(carry,fit,grips,t0+1s+600ms);
            require(fit && fit->grip==Grip::right && fit->hand==1 && grips.draws()==1,"fixture: the draw landed");
            require(near(fit->world,wanted),"a drawn bolter takes the carried gun's fit, not GUN_R in the chainsword's grip");
            require(!fit->seated && !grips.seated(),"that fit is provisional");
            fit=step(carry,fit,grips,t0+1s+700ms);
            require(fit && near(fit->world,wanted),"and stays at the seat while it is carried");
            fit=step(blending,fit,grips,t0+1s+800ms);
            require(fit && !fit->seated && near(fit->world,wanted),"the first frames on GUN_R do not seat: the socket is still moving");
            fit=step(on_gun,fit,grips,t0+1s+900ms);
            require(fit && !fit->seated,"under HOLD on the socket: still waiting");
            fit=step(on_gun,fit,grips,t0+1s+1150ms);
            require(fit && fit->seated && grips.seated() && near(fit->world,wanted) && near(fit->relative,seat),"HOLD on GUN_R seats the grip, learned from the socket");
            fit=step(pistol_on_socket,fit,grips,t0+2s+800ms);
            require(fit && fit->hand==1 && near(fit->relative,seat),"the melee carry on GUN_L does not move a seated fit");
            fit=step(carry,fit,grips,t0+3s);
            require(fit && fit->seated && near(fit->relative,seat) && near(fit->world,wanted),"nor does the chainsword's GUN_R");
        }
        {
            // 16:12:03.957: a new actor. The hub read left, so the primary lands on the actor's first frame.
            vr_weapon::GripSwitch grips;grips.update(Grip::left,t0);
            grips.new_actor();
            require(!grips.seated(),"a new actor's primary has not been seen on its socket");
            auto fit=step(carry,std::nullopt,grips,t0+1s);
            require(fit && fit->grip==Grip::right && !fit->seated && near(fit->world,wanted),"the bolter's first frame is the carried gun, not the chainsword's grip");
            fit=step(on_gun,fit,grips,t0+1s+100ms);
            fit=step(carry,fit,grips,t0+1s+150ms);
            require(fit && !fit->seated,"a blip on GUN_R (0.09 s at 16:12:04.3) does not seat");
            fit=step(on_gun,fit,grips,t0+2s);
            fit=step(on_gun,fit,grips,t0+2s+260ms);
            require(fit && fit->seated && near(fit->relative,seat),"a sustained reading does");
        }
        {
            // Before a seat the gun can leave the hand: neither the melee carry nor a mid-draw gun is a fit.
            vr_weapon::GripSwitch grips;grips.update(Grip::left,t0);grips.new_actor();
            auto fit=step(carry,std::nullopt,grips,t0+1s);
            fit=step(pistol_on_socket,fit,grips,t0+1s+100ms);
            require(fit && !fit->seated && near(fit->world,wanted),"the gun on GUN_L, refused: the provisional fit stays");
            fit=step(out_of_reach,fit,grips,t0+1s+200ms);
            require(fit && !fit->seated && near(fit->world,wanted),"further than CARRY_METRES with no reading: the provisional fit stays");
            // A landing with nothing carried yet (08:46:57.7: the gun 1.6 m off) has only the socket, until it is carried.
            vr_weapon::GripSwitch away;away.update(Grip::left,t0);away.new_actor();
            auto landing=step(out_of_reach,std::nullopt,away,t0+1s);
            require(landing && !landing->seated && near(landing->relative,chainsword_socket),"no carried gun: the socket");
            landing=step(carry,landing,away,t0+1s+100ms);
            require(landing && near(landing->world,wanted),"the first carried frame corrects it");
            // Never carried at all: the seat still replaces the provisional socket, once.
            vr_weapon::GripSwitch never;never.update(Grip::left,t0);never.new_actor();
            auto direct=step(out_of_reach,std::nullopt,never,t0+1s);
            direct=step(on_gun,direct,never,t0+2s);
            require(direct && !direct->seated && near(direct->relative,chainsword_socket),"fixture: on GUN_R, under HOLD, the provisional fit stands");
            direct=step(on_gun,direct,never,t0+2s+260ms);
            require(direct && direct->seated && near(direct->relative,seat) && near(direct->world,wanted),"HOLD on GUN_R replaces it with the socket");
        }
        {
            // Everything else lands on its socket, unchanged. The pistol is seated from its first frame and
            // keeps its fit; so does a primary settled from its reading, including back from the pistol.
            vr_weapon::GripSwitch pistol_grip;
            auto pistol_fit=step(pistol_on_socket,std::nullopt,pistol_grip,t0);
            const auto kept=pistol_fit->relative;
            require(pistol_fit->seated && pistol_grip.seated(),"the pistol seats on its first reading");
            pistol_fit=step(sockets(.56f,-.2f,at(.4f,.1f,-.2f)),pistol_fit,pistol_grip,t0+100ms);
            require(pistol_fit && pistol_fit->seated && pistol_fit->hand==0 && near(pistol_fit->relative,kept),"a pistol off its socket (recoil) keeps its fit and never takes the carried gun's");
            vr_weapon::GripSwitch bolter_grip;
            auto bolter_fit=step(on_gun,std::nullopt,bolter_grip,t0);
            require(bolter_fit && bolter_fit->seated && near(bolter_fit->relative,seat),"a first reading on GUN_R seats at once");
            bolter_fit=step(carry,bolter_fit,bolter_grip,t0+100ms);
            require(bolter_fit && bolter_fit->seated && near(bolter_fit->relative,seat),"a seated primary carried off its socket keeps the fit");
            vr_weapon::GripSwitch settled;settled.update(Grip::left,t0);settled.update(Grip::right,t0+1s);
            require(settled.update(Grip::right,t0+1s+300ms)==Grip::right && settled.seated(),"a grip settled from its reading is seated");
            vr_weapon::GripSwitch flapped;flapped.update(Grip::left,t0);flapped.new_actor();
            require(flapped.update(Grip::left,t0+1s)==Grip::right && flapped.update(Grip::left,t0+2s)==Grip::right && !flapped.seated(),"refused left readings do not seat a primary");
            flapped.update(Grip::two_handed,t0+3s);
            require(flapped.update(Grip::two_handed,t0+3s+300ms)==Grip::two_handed && flapped.seated(),"another primary settled from its reading is seated");
            vr_weapon::GripSwitch spawned;spawned.new_actor();
            require(spawned.seated(),"nothing held before: the first reading is taken at once, on its socket");
            flapped.new_actor();flapped.reset();
            require(flapped.seated(),"weapon following restarted: the first reading is taken at once");
        }

        // The pistol with the right hand's own weapon close (the Sniper's knife 23-33 cm, the Assault's hammer
        // 12-28 cm, 26 Oct classes trace): GUN_R points another way, so it is not a two-handed primary.
        auto knife=sockets(.25f,-.2f,at(0,0,-.2f));knife[7][0]=knife[7][5]=-1; // GUN_R turned 180 degrees about z.
        const auto stab=vr_weapon::retarget(knife,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        require(stab && stab->hand==0 && stab->grip==Grip::left,"the pistol with a knife 25 cm away and turned is still the pistol");
        auto tilted=sockets(.3f,-.2f,at(0,0,-.2f));const float c30=std::cos(.524f),s30=std::sin(.524f);
        tilted[7][0]=tilted[7][5]=c30;tilted[7][1]=s30;tilted[7][4]=-s30; // 30 degrees: the sniper rifle sat at 21.
        const auto rifle=vr_weapon::retarget(tilted,driven,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        require(rifle && rifle->grip==Grip::two_handed,"a long gun whose two sockets agree within 45 degrees is two-handed");
        require(std::abs(vr_weapon::turn_degrees(vr_camera::IDENTITY,knife[7])-180)<.01f && std::abs(vr_weapon::turn_degrees(vr_camera::IDENTITY,tilted[7])-30)<.1f,
            "turn_degrees measures the rotation between frames");
    }
    require(!vr_weapon::retarget(original,changed,parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,NAN),"invalid world scale fails closed");
    std::string config;for(const auto& p:vr_weapon::PARAMETERS) config+=std::string(p.name)+"=0\n";
    std::istringstream valid{config};require(vr_weapon::Settings::read(valid).has_value(),"complete calibration parses");
    std::istringstream bad{config+"yaw_degrees=1\n"};require(!vr_weapon::Settings::read(bad),"duplicate calibration rejects atomically");
    require(argc==2 && vr_weapon::Settings::load(argv[1]).has_value(),"versioned candidate loads through production parser");
    VrWeaponAim driver;const auto now=VrWeaponAim::Clock::now();VrWeaponAim::Pose pose{42,2,1,now,output->world};
    const auto demo_gate=VrWeaponAim::gate(true,true,false,false,0);
    require(demo_gate.allowed && demo_gate.epoch==1,"desktop diagnostic must not require a runtime session");
    require(!VrWeaponAim::gate(false,true,false,false,0).allowed,"desktop diagnostic still requires foreground");
    require(!VrWeaponAim::gate(true,false,false,true,2).allowed && !VrWeaponAim::gate(true,false,true,false,2).allowed,
        "real weapon aim requires both runtime and right tracking");
    require(VrWeaponAim::gate(true,false,true,true,2).allowed && !VrWeaponAim::gate(true,false,true,true,0).allowed,
        "real weapon aim requires a live tracking epoch");
    driver.allow(true,2);driver.publish(pose);require(!driver.pose(42,now),"disabled driver cannot publish a ray");
    driver.enable(true);driver.allow(true,2);driver.publish(pose);require(driver.pose(42,now).has_value() && !driver.pose(99,now),"only exact owner gets committed pose");
    require(!driver.pose(42,now-std::chrono::milliseconds(1)) && !driver.pose(42,now+std::chrono::milliseconds(101)),"future and stale pose fail closed");
    require(driver.hit(pose,{1,2,3},now) && driver.hit(42,now).has_value(),"native collision result retains the exact source pose");
    auto newer=pose;newer.sequence=2;driver.publish(newer);require(!driver.hit(pose,{4,5,6},now),"late collision cannot attach to a newer pose sequence");
    driver.allow(true,3);require(!driver.pose(42,now) && !driver.hit(42,now),"snap or recenter epoch discards pose and hit");
    pose.epoch=3;driver.publish(pose);driver.allow(false,3);require(!driver.pose(42,now),"focus/tracking loss releases aim");
    driver.allow(true,3);driver.publish(pose);driver.publish({});require(!driver.pose(42,now),"failed attachment publication releases the ray immediately");
    driver.publish(pose);auto thrown=pose;thrown.world=at(5,6,7);thrown.sequence=11;driver.throw_aim(thrown);
    const auto launch=driver.ray_pose(42,now);
    require(launch && near(launch->world,thrown.world) && near(driver.pose(42,now)->world,pose.world),"throw targets the owned ray without changing gun attachment or selected hand");
    require(driver.hit(*launch,{8,9,10},now) && driver.hit(42,now),"throw collision result reaches the existing native target alias path");
    auto later=now+std::chrono::milliseconds(751);pose.captured=later;driver.publish(pose);
    require(near(driver.ray_pose(42,later)->world,pose.world) && !driver.hit(42,later),"throw window expires and never leaves aliases aimed at an old grenade");
    driver.throw_aim(thrown);driver.allow(false,3);require(!driver.ray_pose(42,now),"focus release clears throw targeting too");
    std::cout<<"weapon attachment, live grip, committed collision source and lifecycle passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
