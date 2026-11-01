#include "VrMeleeSweep.h"
#include "VrArmPose.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono;
using vr_melee_sweep::Mode;
using vr_melee_sweep::Sweep;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
const auto START=steady_clock::time_point{seconds(10)};
steady_clock::time_point at(int ms) { return START+milliseconds(ms); }
vr_camera::Matrix sword(float x) { auto m=vr_camera::IDENTITY;m[12]=x;return m; }
bool near(float a,float b) { return std::abs(a-b)<1e-3f; }

// A stroke recorded every 10 ms: the hand sword moves +1 unit per 10 ms from 0 to 600 ms and then
// rests at 60; the animation's sword sits at -5. The swing commits at 300 ms.
void stroke(Sweep& s,Mode mode) {
    s.set_mode(mode);
    for(int ms=0;ms<=900;ms+=10) s.record(at(ms),sword(float(std::min(ms,600))/10),sword(-5));
    s.swing(at(300));
}

void replay() {
    Sweep s;stroke(s,Mode::replay);
    // The window opens 500 ms after the swing, with the hand sword resting at 60.
    const auto first=s.pose(1,at(800));
    require(first && near((*first)[12],5),"the window starts at the stroke 250 ms before the commit");
    const auto mid=s.pose(1,at(860));
    require(mid && near((*mid)[12],27.5f),"the path is swept over DURATION: halfway at 60 ms");
    const auto last=s.pose(1,at(950));
    require(last && near((*last)[12],50),"the path ends 200 ms after the commit and holds there");
    require(s.snapshot().replays==1 && s.snapshot().windows==1 && s.snapshot().last_path==46,"one window replays the 46 samples from -250 to +200 ms");
    // Reads more than GAP apart are a new window: the same swing is replayed from its start again.
    const auto again=s.pose(1,at(1110));
    require(again && near((*again)[12],5) && s.snapshot().windows==2,"a later window of the same attack replays the stroke again");
}

void no_swing() {
    Sweep s;stroke(s,Mode::replay);
    require(!s.pose(1,at(1600)),"a window 1.3 s after the swing leaves the native read alone");
    require(s.snapshot().no_swing==1 && s.snapshot().replays==0,"and counts as no swing");
    Sweep fresh;fresh.set_mode(Mode::replay);fresh.record(at(0),sword(1),sword(2));
    require(!fresh.pose(1,at(10)),"no swing at all: native read");
}

void modes() {
    Sweep hand;stroke(hand,Mode::hand);
    require(!hand.pose(1,at(800)),"hand mode never replaces the detector's read");
    Sweep animation;stroke(animation,Mode::animation);
    const auto a=animation.pose(1,at(800));
    require(a && near((*a)[12],-5),"animation mode hands the detector the animation's sword");
    require(vr_melee_sweep::parse("animation\r\n")==Mode::animation && vr_melee_sweep::parse("replay")==Mode::replay &&
        vr_melee_sweep::parse("hand")==Mode::hand && !vr_melee_sweep::parse("native"),"control file words");
}

void detectors() {
    Sweep s;stroke(s,Mode::replay);
    require(s.pose(1,at(800)) && s.pose(1,at(860)),"detector 1 window");
    const auto other=s.pose(2,at(870));
    require(other && near((*other)[12],5) && s.snapshot().windows==2,"another detector reading starts its own window");
}

void blend_is_rigid() {
    auto a=vr_camera::IDENTITY,b=vr_camera::IDENTITY;
    // b: a quarter turn about y.
    b[0]=0;b[2]=-1;b[8]=1;b[10]=0;b[12]=4;
    const auto m=vr_melee_sweep::blend(a,b,.5f);
    require(vr_camera::rigid(m),"a blend of two rotations stays a rotation");
    require(near(m[12],2),"positions blend linearly");
    auto mirrored=vr_camera::IDENTITY;mirrored[10]=-1;
    const auto n=vr_melee_sweep::blend(mirrored,mirrored,.3f);
    require(near(n[10],-1) && near(n[5],1),"a left-handed input keeps its handedness");
}

void chain() {
    // root(0) <- arm(1) <- sword(2), and a second root (3): the relative transform times the root's
    // world must equal the full hierarchy's world, as the game evaluates it.
    std::vector<vr_camera::Matrix> locals(4,vr_camera::IDENTITY);
    locals[0][12]=10;locals[1][13]=2;locals[2][0]=0;locals[2][1]=1;locals[2][4]=-1;locals[2][5]=0;locals[2][14]=3;
    const std::vector<int32_t> parents{-1,0,1,-1};
    auto world=vr_camera::IDENTITY;world[12]=100;
    std::vector<vr_camera::Matrix> all;require(vr_arm::hierarchy(locals,parents,world,all),"hierarchy");
    const auto c=vr_melee_sweep::below_root(locals,parents,2);
    require(c && c->root==0,"the sword's root is bone 0");
    const auto rebuilt=vr_camera::multiply(c->relative,all[0]);
    for(int i=0;i<16;++i) require(near(rebuilt[i],all[2][i]),"relative * root world = world");
    require(!vr_melee_sweep::below_root(locals,parents,9),"a missing bone has no chain");
    // The rig's root has no evaluated node: the anchor is the topmost ancestor that has one.
    const auto unmapped=vr_melee_sweep::below_root(locals,parents,2,[](size_t i){return i!=0;});
    require(unmapped && unmapped->root==1,"an unmapped root anchors on the arm");
    const auto on_arm=vr_camera::multiply(unmapped->relative,all[1]);
    for(int i=0;i<16;++i) require(near(on_arm[i],all[2][i]),"relative * anchor world = world");
    require(!vr_melee_sweep::below_root(locals,parents,2,[](size_t i){return i==2;}),"no evaluated ancestor: no chain");
}

void world_from_native_read() {
    // A root world with rotation, scale and translation, as a character's evaluated root can be.
    auto root=vr_camera::IDENTITY;root[0]=0;root[1]=1.1f;root[4]=-1.1f;root[5]=0;root[10]=1.1f;root[12]=50;root[13]=-7;root[14]=3;
    const auto inverse=Sweep::invert(root);
    require(inverse.has_value(),"a scaled root inverts");
    const auto identity=vr_camera::multiply(root,*inverse);
    for(int i=0;i<16;++i) require(near(identity[i],vr_camera::IDENTITY[i]),"root * inverse(root) = identity");
    require(!Sweep::invert(vr_camera::Matrix{}),"a singular matrix has no inverse");
    // The detector reads the hand's sword = latest relative * root. The replay must come out as
    // the path's relative * root, from that read alone.
    Sweep s;stroke(s,Mode::replay);
    const auto native=vr_camera::multiply(sword(60),root);
    const auto replayed=s.world(1,at(800),native);
    const auto expected=vr_camera::multiply(sword(5),root);
    require(replayed.has_value(),"the window replays");
    for(int i=0;i<16;++i) require(near((*replayed)[i],expected[i]),"replayed sword = path relative * root world");
    Sweep hand;stroke(hand,Mode::hand);
    require(!hand.world(1,at(800),native),"hand mode leaves the read alone");
    require(s.snapshot().refused==0,"nothing refused");
    // The arm driver stopped committing: the newest sample is too old to stand for the sword now.
    Sweep stale;stale.set_mode(Mode::replay);
    for(int ms=0;ms<=400;ms+=10) stale.record(at(ms),sword(float(ms)/10),sword(-5));
    stale.swing(at(300));
    require(!stale.world(1,at(600),native) && stale.snapshot().refused==1,"a sample older than LATEST leaves the native read alone");
}

void old_samples_drop() {
    Sweep s;s.set_mode(Mode::replay);
    for(int ms=0;ms<=5000;ms+=10) s.record(at(ms),sword(float(ms)),sword(0));
    s.swing(at(1000));
    require(!s.pose(1,at(1500)),"history older than KEEP is gone, so an old stroke cannot be replayed");
}
}

int main() {
    try {
        replay();no_swing();modes();detectors();blend_is_rigid();chain();world_from_native_read();old_samples_drop();
        std::cout<<"melee sweep tests passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
