#include "VrUprightTorso.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
using vr_upright::Matrix;using vr_upright::Vector;
Matrix at(Vector p) { auto m=vr_camera::IDENTITY;vr_arm::position(m,p);return m; }
// The real rig's names and parent order (sm2vr.log ArmLocalBone, 18 Oct): root, CENTRE,
// BACKdown, BACKmiddle, BACKup, NECK, HEAD, clavicleR, ARM1R, shoulder_R, LEG (sibling),
// clavicleL, ARM1L, shoulder_L. Forward is -Z, the character's right +X.
const std::vector<std::string> NAMES{"ROOT","CENTRE","BACKdown","BACKmiddle","BACKup","NECK","HEAD","clavicleR","ARM1R","shoulder_R","LEG1R","clavicleL","ARM1L","shoulder_L"};
const std::vector<int32_t> PARENTS{-1,0,1,2,3,4,5,4,7,8,1,4,11,12};
constexpr size_t NECK=5,HEAD=6,ARM1R=8,ARM1L=12,PAULDRON_L=13;
std::vector<Matrix> bind() {
    return {at({0,0,0}),at({0,1.2f,0}),at({0,0.1f,0}),at({0,0.35f,0}),at({0,0.35f,0}),at({0,0.3f,0}),at({0,0.2f,0}),
            at({0.2f,0,0}),at({0.25f,0,0}),at({0,0.1f,0}),at({0.15f,-0.6f,0}),at({-0.2f,0,0}),at({-0.25f,0,0}),at({0,0.1f,0})};
}
// The native stance turn: BACKdown turned about the vertical so the left shoulder leads
// (moves towards -Z), optionally on top of a forward lean.
std::vector<Matrix> stance(float twist_degrees,float lean_degrees=0) {
    auto l=bind();
    l[2]=vr_camera::multiply(vr_upright::rotation({0,1,0},-twist_degrees/57.2957795f),l[2]);
    l[2]=vr_camera::multiply(vr_upright::rotation({1,0,0},-lean_degrees/57.2957795f),l[2]);
    return l;
}
// Lean the spine forward (towards -Z here) by rotating BACKdown about X.
std::vector<Matrix> leaned(float degrees) {
    auto l=bind();const auto r=vr_upright::rotation({1,0,0},-degrees/57.2957795f);
    auto m=vr_camera::multiply(r,l[2]);l[2]=m;return l;
}
std::vector<Matrix> world(const std::vector<Matrix>& locals,const Matrix& root=vr_camera::IDENTITY) {
    std::vector<Matrix> w;require(vr_arm::hierarchy(locals,PARENTS,root,w),"hierarchy");return w;
}
float dist(Vector a,Vector b) { return vr_arm::length(vr_arm::sub(a,b)); }

void corrects_lean() {
    const auto b=bind();const auto l=leaned(12);
    const auto before=world(l);
    require(before[6][14]<-0.1f,"fixture: a 12 degree lean carries the head forward");
    const auto lean=vr_upright::lean(l,b,PARENTS,NAMES,vr_camera::IDENTITY);
    require(lean && std::abs(*lean-12)<0.05f,"lean is measured from the bind spine line");
    const auto r=vr_upright::correct(l,b,PARENTS,NAMES,vr_camera::IDENTITY,{});
    require(r && r->corrected && std::abs(r->before_degrees-12)<0.05f && r->after_degrees<0.05f,"full strength removes the lean");
    const auto after=world(r->locals);const auto upright=world(b);
    require(dist(vr_arm::position(after[6]),vr_arm::position(upright[6]))<1e-4f,"head returns over the pelvis");
    require(dist(vr_arm::position(after[9]),vr_arm::position(upright[9]))<1e-4f,"shoulder pads follow the torso");
    require(dist(vr_arm::position(after[2]),vr_arm::position(before[2]))<1e-6f,"BACKdown pivot stays in place");
    for(size_t i:{size_t(0),size_t(1),size_t(10)}) require(after[i]==before[i],"root, pelvis and legs are untouched");
    for(size_t i=0;i<l.size();++i) if(i!=2) require(r->locals[i]==l[i],"only BACKdown's local changes");
}
void recorded_sprint_lean() {
    // Latest physical playtest, fresh driven unpaused samples: sprint median
    // 55.87 degrees, peak 78.75. Previously the default left 25.87/48.75 degrees.
    for(float degrees:{44.70f,55.87f,78.75f}) {
        const auto l=leaned(degrees);
        vr_upright::Settings old;old.max_degrees=30;
        const auto residual=vr_upright::correct(l,bind(),PARENTS,NAMES,vr_camera::IDENTITY,old);
        require(residual && residual->after_degrees>14,"recorded lean reproduces the old cap failure");
        const auto r=vr_upright::correct(l,bind(),PARENTS,NAMES,vr_camera::IDENTITY,{});
        require(r && r->after_degrees<0.05f,"default now removes the recorded sprint lean");
        const auto before=world(l),after=world(r->locals),reference=world(bind());
        require(dist(vr_arm::position(after[6]),vr_arm::position(reference[6]))<1e-4f,"head follows the restored spine, not the animated lean");
        for(size_t i:{size_t(0),size_t(1),size_t(10)}) require(after[i]==before[i],"recorded correction leaves root, pelvis and legs unchanged");
    }
}
void settings() {
    const auto b=bind();const auto l=leaned(12);
    vr_upright::Settings half;half.strength=0.5f;
    const auto h=vr_upright::correct(l,b,PARENTS,NAMES,vr_camera::IDENTITY,half);
    require(h && std::abs(h->after_degrees-6)<0.05f,"half strength removes half the lean");
    vr_upright::Settings capped;capped.max_degrees=5;
    const auto c=vr_upright::correct(l,b,PARENTS,NAMES,vr_camera::IDENTITY,capped);
    require(c && std::abs(c->after_degrees-7)<0.05f,"max_degrees caps the correction");
    const auto small=vr_upright::correct(leaned(1.5f),b,PARENTS,NAMES,vr_camera::IDENTITY,{});
    require(small && !small->corrected && small->locals==leaned(1.5f),"leans inside the deadband are left alone");
    vr_upright::Settings off;off.strength=0;
    const auto o=vr_upright::correct(l,b,PARENTS,NAMES,vr_camera::IDENTITY,off);
    require(o && !o->corrected && o->locals==l,"strength 0 changes nothing");
    vr_upright::Settings bad;bad.strength=2;
    require(!vr_upright::correct(l,b,PARENTS,NAMES,vr_camera::IDENTITY,bad),"invalid settings are refused");
    const auto back=vr_upright::correct(leaned(-10),b,PARENTS,NAMES,vr_camera::IDENTITY,{});
    require(back && back->after_degrees<0.05f,"a backward lean is corrected the same way");
}
void root_frame() {
    // A yawed, translated character: correction works in the root's frame.
    const float yaw=1.1f;auto root=vr_upright::rotation({0,1,0},yaw);vr_arm::position(root,{5,2,-3});
    const auto r=vr_upright::correct(leaned(12),bind(),PARENTS,NAMES,root,{});
    require(r && r->after_degrees<0.05f,"correction is independent of where the character faces");
    const auto after=world(r->locals,root);const auto upright=world(bind(),root);
    require(dist(vr_arm::position(after[6]),vr_arm::position(upright[6]))<1e-4f,"head is upright over the yawed pelvis");
}
void refuses_wrong_rigs() {
    auto names=NAMES;names[2]="SPINE";
    require(!vr_upright::correct(leaned(12),bind(),PARENTS,names,vr_camera::IDENTITY,{}),"rig without BACKdown is refused");
    names=NAMES;names[5]="HEAD";
    require(!vr_upright::correct(leaned(12),bind(),PARENTS,names,vr_camera::IDENTITY,{}),"duplicate names are refused");
    auto parents=PARENTS;parents[5]=1; // NECK not under BACKdown.
    require(!vr_upright::correct(leaned(12),bind(),parents,NAMES,vr_camera::IDENTITY,{}),"NECK must descend from BACKdown");
    auto short_bind=bind();short_bind.pop_back();
    require(!vr_upright::correct(leaned(12),short_bind,PARENTS,NAMES,vr_camera::IDENTITY,{}),"mismatched bind pose is refused");
    // Malformed parent tables must be refused before any traversal (no crash, no hang).
    parents=PARENTS;parents[5]=99;
    require(!vr_upright::chain(NAMES,parents) && !vr_upright::correct(leaned(12),bind(),parents,NAMES,vr_camera::IDENTITY,{}),"out-of-range parent is refused");
    parents=PARENTS;parents[3]=-2;
    require(!vr_upright::chain(NAMES,parents),"negative parent other than -1 is refused");
    parents=PARENTS;parents[2]=5; // BACKdown <- NECK <- ... <- BACKdown: a cycle.
    require(!vr_upright::chain(NAMES,parents) && !vr_upright::lean(leaned(12),bind(),parents,NAMES,vr_camera::IDENTITY),"cyclic parents are refused");
    parents=PARENTS;parents[4]=4;
    require(!vr_upright::chain(NAMES,parents),"a bone that is its own parent is refused");
    auto fewer=PARENTS;fewer.pop_back();
    require(!vr_upright::chain(NAMES,fewer),"names and parents of different sizes are refused");
    require(!vr_upright::chain({},{}),"an empty rig is refused");
    auto short_locals=leaned(12);short_locals.pop_back();
    require(!vr_upright::correct(short_locals,bind(),PARENTS,NAMES,vr_camera::IDENTITY,{}),"locals of the wrong size are refused");
    parents=PARENTS;parents[2]=-1; // BACKdown as a root: nothing to rotate it relative to.
    require(!vr_upright::chain(NAMES,parents),"BACKdown without a parent is refused");
}
void squares_the_shoulders() {
    const auto b=bind();const auto reference=world(b);
    // Fixture check: this turn really puts the left arm root ahead of the right one.
    const auto turned=stance(48.6f);const auto before=world(turned);
    require(vr_arm::position(before[ARM1L])[2]<-0.3f && vr_arm::position(before[ARM1R])[2]>0.3f,"fixture: the stance leads with the left shoulder");
    const auto measured=vr_upright::twist(turned,b,PARENTS,NAMES,vr_camera::IDENTITY);
    require(measured && std::abs(std::abs(*measured)-48.6f)<0.05f,"twist is measured about the spine from the bind shoulder line");
    const auto r=vr_upright::correct(turned,b,PARENTS,NAMES,vr_camera::IDENTITY,{});
    require(r && r->corrected && std::abs(std::abs(r->twist_before_degrees)-48.6f)<0.05f && std::abs(r->twist_after_degrees)<0.05f,"full twist strength squares the shoulders");
    const auto after=world(r->locals);
    for(size_t i:{ARM1L,ARM1R,PAULDRON_L,HEAD,NECK}) require(dist(vr_arm::position(after[i]),vr_arm::position(reference[i]))<1e-4f,"shoulders, pauldron and head return to the bind pose");
    for(size_t i:{size_t(0),size_t(1),size_t(10)}) require(after[i]==before[i],"root, pelvis and legs are untouched");
    for(size_t i=0;i<turned.size();++i) if(i!=2) require(r->locals[i]==turned[i],"only BACKdown's local changes");
    require(dist(vr_arm::position(after[2]),vr_arm::position(before[2]))<1e-6f,"BACKdown pivot stays in place");
    // Joe's campaign 21 Oct, standing: the eye is 0.23 m ahead of and 0.32 m above NECK.
    // With the stance the left arm root is ahead of the eye; squared, it is well behind it.
    const auto eye=vr_arm::add(vr_arm::position(reference[NECK]),Vector{0,0.32f,-0.23f});
    require(vr_arm::position(before[ARM1L])[2]<eye[2],"recorded stance: left shoulder in front of the eyes");
    require(vr_arm::position(after[ARM1L])[2]-eye[2]>0.2f,"squared: left shoulder 20+ cm behind the eyes");
}
void twist_and_lean_together() {
    const auto b=bind();const auto reference=world(b);
    for(float twist:{-63.9f,-20.f,20.f,40.4f,63.9f}) for(float lean:{0.f,12.f,55.87f}) {
        const auto r=vr_upright::correct(stance(twist,lean),b,PARENTS,NAMES,vr_camera::IDENTITY,{});
        require(r && r->after_degrees<0.05f && std::abs(r->twist_after_degrees)<0.05f,"lean and twist are both removed");
        const auto after=world(r->locals);
        for(size_t i:{ARM1L,ARM1R,HEAD}) require(dist(vr_arm::position(after[i]),vr_arm::position(reference[i]))<1e-3f,"upper body ends in the bind pose");
    }
    // The twist turns about the spine line, so a partial lean stays exactly as partial.
    vr_upright::Settings half;half.strength=0.5f;
    const auto h=vr_upright::correct(stance(40,12),b,PARENTS,NAMES,vr_camera::IDENTITY,half);
    require(h && std::abs(h->after_degrees-6)<0.05f && std::abs(h->twist_after_degrees)<0.05f,"twist correction leaves the partial lean alone");
    // Yawed, translated character: the twist is measured in the character's frame.
    auto root=vr_upright::rotation({0,1,0},2.3f);vr_arm::position(root,{5,2,-3});
    const auto y=vr_upright::correct(stance(50,12),b,PARENTS,NAMES,root,{});
    require(y && std::abs(y->twist_after_degrees)<0.05f,"twist is independent of where the character faces");
    require(dist(vr_arm::position(world(y->locals,root)[ARM1L]),vr_arm::position(world(b,root)[ARM1L]))<1e-3f,"left shoulder squared over the yawed pelvis");
}
void twist_settings() {
    const auto b=bind();const auto l=stance(40,12);
    vr_upright::Settings lean_only;lean_only.twist_strength=0;
    const auto o=vr_upright::correct(l,b,PARENTS,NAMES,vr_camera::IDENTITY,lean_only);
    require(o && o->after_degrees<0.05f && std::abs(std::abs(o->twist_after_degrees)-40)<0.05f,"twist strength 0 keeps the 21 Oct lean-only behaviour");
    vr_upright::Settings half;half.twist_strength=0.5f;
    const auto h=vr_upright::correct(l,b,PARENTS,NAMES,vr_camera::IDENTITY,half);
    require(h && std::abs(std::abs(h->twist_after_degrees)-20)<0.05f,"half twist strength removes half the twist");
    vr_upright::Settings capped;capped.max_degrees=15;
    const auto c=vr_upright::correct(stance(40),b,PARENTS,NAMES,vr_camera::IDENTITY,capped);
    require(c && std::abs(std::abs(c->twist_after_degrees)-25)<0.05f,"max_degrees caps the twist too");
    const auto small=vr_upright::correct(stance(1.5f),b,PARENTS,NAMES,vr_camera::IDENTITY,{});
    require(small && !small->corrected && small->locals==stance(1.5f),"twists inside the deadband are left alone");
    vr_upright::Settings bad;bad.twist_strength=1.5f;
    require(!bad.valid() && !vr_upright::correct(l,b,PARENTS,NAMES,vr_camera::IDENTITY,bad),"invalid twist strength is refused");
    // A rig without both arm roots still gets the lean correction, and no twist.
    auto names=NAMES;names[ARM1L]="ARM1L_extra";
    const auto r=vr_upright::correct(l,b,PARENTS,names,vr_camera::IDENTITY,{});
    require(r && r->after_degrees<0.05f && r->twist_before_degrees==0 && !vr_upright::twist(l,b,PARENTS,names,vr_camera::IDENTITY),"no arm roots: lean only");
    names=NAMES;names[ARM1R]="ARM1L";
    require(!vr_upright::twist(l,b,PARENTS,names,vr_camera::IDENTITY),"duplicate arm roots are not guessed between");
    auto parents=PARENTS;parents[11]=1; // Left clavicle under the pelvis: turning BACKdown would not move it.
    require(!vr_upright::twist(l,b,parents,NAMES,vr_camera::IDENTITY),"arm roots must hang below BACKdown");
}
void configuration() {
    using vr_upright::parse_config;
    require(!parse_config(std::nullopt,std::nullopt).enabled && parse_config(std::nullopt,std::nullopt).error.empty(),"unset is off, no error");
    require(parse_config(std::string{"1"},std::nullopt).enabled,"exactly 1 enables");
    require(!parse_config(std::string{"0"},std::nullopt).enabled && parse_config(std::string{"0"},std::nullopt).error.empty(),"0 is off");
    for(const char* junk:{"yes","1x","11","","<too long>"}) {
        const auto c=parse_config(std::string{junk},std::nullopt);
        require(!c.enabled && !c.error.empty(),"malformed enable values stay off and report why");
    }
    const auto half=parse_config(std::string{"1"},std::string{"0.5"});
    require(half.enabled && half.settings.strength==0.5f,"strength parses");
    for(const char* junk:{"abc","0.5x","1.5","-0.1","nan","inf","","<too long>"}) {
        const auto c=parse_config(std::string{"1"},std::string{junk});
        require(!c.enabled && !c.error.empty(),"malformed strength disables rather than becoming 0");
    }
    require(parse_config(std::string{"1"},std::string{"0"}).enabled,"strength 0 is valid (enabled, no correction)");
    require(parse_config(std::string{"1"},std::nullopt).settings.twist_strength==1,"twist is on by default with the upright torso");
    const auto lean_only=parse_config(std::string{"1"},std::nullopt,std::string{"0"});
    require(lean_only.enabled && lean_only.settings.twist_strength==0 && lean_only.error.empty(),"SM2VR_UPRIGHT_TWIST=0 keeps the lean only");
    require(parse_config(std::string{"1"},std::string{"0.5"},std::string{"0.25"}).settings.twist_strength==0.25f,"twist strength parses");
    for(const char* junk:{"abc","0.5x","1.5","-0.1","nan","","<too long>"}) {
        const auto c=parse_config(std::string{"1"},std::nullopt,std::string{junk});
        require(!c.enabled && !c.error.empty(),"malformed twist disables rather than guessing");
    }
}
} // namespace

// Joe 22 Oct: with the twist the pistol fired once per burst. The twist must be off from a
// trigger press until FireHold::HOLD after the last press, then return; it must not come
// back between taps at Joe's 1-3 Hz.
void fire_hold() {
    using namespace std::chrono_literals;vr_upright::FireHold hold;const auto t0=vr_upright::FireHold::Clock::time_point{100s};
    float scale=1;for(int ms=0;ms<=1000;ms+=10) scale=hold.update(false,t0+std::chrono::milliseconds(ms));
    require(scale==1,"no trigger: full twist");
    auto t=t0+1s;require(hold.update(true,t)>.99f,"the press sample itself has no elapsed time");
    for(int ms=10;ms<=vr_upright::FireHold::DOWN.count();ms+=10) scale=hold.update(ms<=200,t+std::chrono::milliseconds(ms));
    require(scale==0,"twist gone within DOWN of the press");
    // Taps every second for 5 s: the twist stays off throughout.
    for(int ms=110;ms<5000;ms+=10) {scale=hold.update(ms%1000<200,t+std::chrono::milliseconds(ms));require(scale==0,"stays off between taps");}
    const auto last=t+4190ms; // last pressed sample of the 4 s tap
    for(int ms=5000;ms<4190+vr_upright::FireHold::HOLD.count();ms+=10) {scale=hold.update(false,t+std::chrono::milliseconds(ms));require(scale==0,"off for HOLD after the last press");}
    for(auto at=last+vr_upright::FireHold::HOLD;at<=last+vr_upright::FireHold::HOLD+vr_upright::FireHold::RETURN+20ms;at+=10ms) scale=hold.update(false,at);
    require(scale==1,"back to full twist RETURN after the hold");
    const float mid=[&]{vr_upright::FireHold h;h.update(true,t0);h.update(true,t0+200ms);float s=0;for(auto at=t0+210ms;at<=t0+200ms+vr_upright::FireHold::HOLD+250ms;at+=10ms) s=h.update(false,at);return s;}();
    require(mid>.4f && mid<.6f,"the return is gradual, not a snap");
}

int main() {
    try { corrects_lean();recorded_sprint_lean();settings();root_frame();refuses_wrong_rigs();squares_the_shoulders();twist_and_lean_together();twist_settings();configuration();fire_hold(); }
    catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
    std::cout<<"vr_upright_torso: all checks passed\n";return 0;
}
