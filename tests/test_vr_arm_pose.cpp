#include "VrArmPose.h"
#include <iostream>
#include <stdexcept>
#include <limits>
namespace {
void require(bool b,const char* text) { if (!b) { throw std::runtime_error{text}; } }
bool close(float a,float b,float tolerance=1e-4f) { return std::abs(a-b)<tolerance; }
vr_arm::Matrix pose(vr_arm::Vector p) { auto m=vr_camera::IDENTITY; vr_arm::position(m,p); return m; }
void check(vr_arm::Vector a,vr_arm::Vector b,const char* why) { require(vr_arm::length(vr_arm::sub(a,b))<1e-4f,why); }
void armour_endpoint() {
    // Observed rig: forearm_L/R and HANDL/R are siblings under ARM2, with
    // matching endpoint translations; ARM2R_glove/HANDR_glove are separate.
    // Actual segment lengths from the owned 282-bone rig capture, game units.
    constexpr float upper=.422250628f,lower=.422810793f;
    for(size_t hand=0;hand<2;++hand) for(float yaw:{0.f,1.5707963f,3.1415926f}) {
        const float side=hand==0?1.f:-1.f;
        const std::string suffix=hand==0?"L":"R";
        const std::vector<std::string> names{"ROOT","ARM1"+suffix,"ARM2"+suffix,"forearm_"+suffix,"HAND"+suffix,"OTHER"};
        const std::vector<int32_t> parents{-1,0,1,2,2,0};
        const std::vector<vr_arm::Matrix> locals{pose({0,0,0}),pose({side*.2f,1.5f,0}),pose({side*upper,0,0}),pose({side*lower,0,0}),pose({side*lower,0,0}),pose({0,0,3})};
        auto root=vr_camera::IDENTITY;root[0]=root[10]=std::cos(yaw);root[2]=-std::sin(yaw);root[8]=std::sin(yaw);vr_arm::position(root,{5,2,-3});
        for(float reach:{.5f,1.1f}) {
            auto target=vr_camera::multiply(pose({side*.2f,1.5f,-reach}),root);
            const auto solved=vr_arm::solve_named(locals,parents,names,root,hand,target,vr_arm::rotate({0,-1,0},root),true);
            require(solved && solved->target_error<1e-4f,"armour correction preserves exact physical wrist");
            std::vector<vr_arm::Matrix> world;
            require(vr_arm::hierarchy(solved->locals,parents,root,world),"armour endpoint correction uses rigid transforms");
            check(vr_arm::position(world[3]),vr_arm::position(world[4]),"forearm armour endpoint remains attached to wrist even beyond native reach");
            require(solved->locals[0]==locals[0] && solved->locals[5]==locals[5],"root and unrelated body remain native");
            check(vr_arm::position(world[1]),vr_arm::position(vr_camera::multiply(locals[1],root)),"shoulder pivot remains native");
            for(size_t i=0;i<12;++i)require(close(world[4][i],target[i]),"palm rotation remains tracked exactly");
            // Rolling only the palm must not rotate the elbow-mounted bracer.
            auto rolled=target;rolled[0]*=-1;rolled[1]*=-1;rolled[2]*=-1;rolled[4]*=-1;rolled[5]*=-1;rolled[6]*=-1;
            const auto roll=vr_arm::solve_named(locals,parents,names,root,hand,rolled,vr_arm::rotate({0,-1,0},root),true);
            std::vector<vr_arm::Matrix> rolled_world;require(roll && vr_arm::hierarchy(roll->locals,parents,root,rolled_world),"palm roll solve");
            for(size_t i=0;i<12;++i)require(close(rolled_world[3][i],world[3][i]),"armour retains forearm rotation instead of inheriting palm roll");
            if(reach<upper+lower)require(solved->locals[3]==locals[3],"reachable native forearm helper is unchanged");
            else require(solved->locals[3]!=locals[3],"unreachable endpoint reproduces and repairs stale armour translation");
        }
        auto bad=parents;bad[3]=1;
        require(!vr_arm::solve_named(locals,bad,names,root,hand,pose({0,0,0}),{0,-1,0},true),"unexpected forearm hierarchy refuses rather than moving arbitrary armour");
    }
}
// Row-vector rotation (v' = v*M) by `degrees` about unit axis u through point p.
vr_arm::Matrix about(vr_arm::Vector u,float degrees,vr_arm::Vector p={0,0,0}) {
    const float t=degrees*0.0174532925f,c=std::cos(t),s=std::sin(t);
    const float k[3][3]{{0,-u[2],u[1]},{u[2],0,-u[0]},{-u[1],u[0],0}}; // Column-convention cross matrix.
    auto r=vr_camera::IDENTITY;
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) r[i*4+j]=(i==j?c:0)+s*k[j][i]+(1-c)*u[i]*u[j];
    return vr_camera::multiply(vr_camera::multiply(pose(vr_arm::scale(p,-1)),r),pose(p));
}
bool same(const vr_arm::Matrix& a,const vr_arm::Matrix& b,float tolerance=2e-4f) { for(size_t i=0;i<16;++i) if(!close(a[i],b[i],tolerance)) return false;return true; }
// Playtest 26 Oct (Bulwark): the storm shield hangs off shield_pos under ARM2L, so it kept the
// animation's forearm roll whatever the wrist did. It now turns with the tracked hand's roll
// about the forearm; wrist bend, the bracer and the hand itself are unchanged.
void shield_twist() {
    constexpr float upper=.422250628f,lower=.422810793f;
    const std::vector<std::string> names{"ROOT","ARM1L","ARM2L","forearm_L","HANDL","shield_pos","OTHER"};
    const std::vector<int32_t> parents{-1,0,1,2,2,2,0};
    auto hand=vr_camera::multiply(about({0,1,0},25),pose({lower,0,0}));            // The animation's own hand roll/bend.
    auto shield=vr_camera::multiply(about({0,0.6f,0.8f},40),pose({.12f,.04f,-.09f})); // Mount beside the forearm.
    const std::vector<vr_arm::Matrix> locals{pose({0,0,0}),pose({.2f,1.5f,0}),pose({upper,0,0}),pose({lower,0,0}),hand,shield,pose({0,0,3})};
    auto root=about({0,1,0},70);vr_arm::position(root,{5,2,-3});
    const auto pole=vr_arm::rotate({0,-1,0},root);
    const auto target_at=[&](const vr_arm::Matrix& rotation){auto m=rotation;vr_arm::position(m,vr_arm::position(vr_camera::multiply(pose({.2f,1.2f,-.5f}),root)));return m;};
    const auto solve=[&](const vr_arm::Matrix& target,bool twist=true){
        const auto s=vr_arm::solve_named(locals,parents,names,root,0,target,pole,true,twist);
        require(s && s->target_error<1e-4f,"shielded arm solves to the tracked wrist");
        std::vector<vr_arm::Matrix> world;require(vr_arm::hierarchy(s->locals,parents,root,world),"shielded hierarchy is rigid");
        for(size_t i=0;i<12;++i) require(close(world[4][i],target[i]),"palm stays exactly on the controller");
        return std::pair{*s,world};};
    // The hand held at the animation's roll relative to the solved forearm: the mount is native.
    const auto probe=solve(target_at(vr_camera::IDENTITY)).second;
    auto native_hand=hand,elbow=probe[2];vr_arm::position(native_hand,{0,0,0});vr_arm::position(elbow,{0,0,0});
    const auto aligned=target_at(vr_camera::multiply(native_hand,elbow));
    const auto [base,base_world]=solve(aligned);
    require(same(base.locals[5],shield),"hand at the animation's roll leaves the shield mount native");
    const auto axis_world=vr_arm::scale(vr_arm::sub(vr_arm::position(base_world[4]),vr_arm::position(base_world[2])),
        1/vr_arm::length(vr_arm::sub(vr_arm::position(base_world[4]),vr_arm::position(base_world[2]))));
    const auto wrist=vr_arm::position(base_world[4]),elbow_at=vr_arm::position(base_world[2]);
    for(float degrees:{-80.f,35.f,120.f}) {
        // Roll the controller about the forearm: the shield turns with it, about the same line.
        const auto rolled=vr_camera::multiply(aligned,about(axis_world,degrees,wrist));
        const auto [roll,roll_world]=solve(rolled);
        require(same(roll_world[2],base_world[2]) && same(roll_world[3],base_world[3]),"a roll moves neither the forearm nor the bracer");
        require(same(roll_world[5],vr_camera::multiply(base_world[5],about(axis_world,degrees,elbow_at))),"the shield turns with the wrist roll about the forearm");
        require(!same(roll_world[5],base_world[5]),"the roll reaches the shield");
        const auto [off,off_world]=solve(rolled,false);
        require(same(off.locals[5],shield),"without the twist the mount keeps the animation's forearm roll");
    }
    // Bending the wrist (about an axis across the forearm) is not a roll: the shield stays.
    const auto across=vr_arm::scale(vr_arm::cross(axis_world,{0,1,0}),1/vr_arm::length(vr_arm::cross(axis_world,{0,1,0})));
    const auto [bend,bend_world]=solve(vr_camera::multiply(aligned,about(across,30,wrist)));
    require(same(bend.locals[5],shield),"wrist bend does not swing the shield");
    // A mount anywhere else is left to the animation; the arm still solves.
    auto moved=parents;moved[5]=4;
    const auto elsewhere=vr_arm::solve_named(locals,moved,names,root,0,vr_camera::multiply(aligned,about(axis_world,60,wrist)),pole,true);
    require(elsewhere && elsewhere->locals[5]==shield,"a shield_pos outside the forearm is untouched and the arm solves");
    // The right arm has no shield: a shield_pos name never moves with it.
    auto right_names=names;right_names[1]="ARM1R";right_names[2]="ARM2R";right_names[3]="forearm_R";right_names[4]="HANDR";
    const auto right=vr_arm::solve_named(locals,parents,right_names,root,1,vr_camera::multiply(aligned,about(axis_world,60,wrist)),pole,true);
    require(right && right->locals[5]==shield,"the right arm never turns a shield mount");
}
// The takeover's head visibility reads the HEAD bone through its chain only.
void chain_position() {
    const std::vector<int32_t> parents{-1,0,1,0,3};
    std::vector<vr_arm::Matrix> locals{pose({0,1,0}),pose({0,.5f,0}),pose({0,.3f,.1f}),pose({.2f,0,0}),pose({.4f,0,0})};
    locals[1][0]=locals[1][10]=0;locals[1][2]=-1;locals[1][8]=1; // a 90-degree turn at the neck
    auto root=pose({5,2,-3});root[0]=root[10]=0;root[2]=1;root[8]=-1;
    std::vector<vr_arm::Matrix> world;require(vr_arm::hierarchy(locals,parents,root,world),"reference hierarchy");
    for(size_t i=0;i<locals.size();++i) {
        const auto p=vr_arm::chain_position(parents,root,i,[&](size_t j)->const vr_arm::Matrix&{return locals[j];});
        require(p.has_value(),"chain evaluates");check(*p,vr_arm::position(world[i]),"chain position equals the full hierarchy");
    }
    // Only the chain is read: an invalid matrix on another branch does not matter.
    auto broken=locals;broken[4][0]=std::numeric_limits<float>::quiet_NaN();
    require(vr_arm::chain_position(parents,root,2,[&](size_t j)->const vr_arm::Matrix&{return broken[j];}).has_value(),"other branches are not evaluated");
    require(!vr_arm::chain_position(parents,root,4,[&](size_t j)->const vr_arm::Matrix&{return broken[j];}),"a non-rigid local on the chain rejects");
    require(!vr_arm::chain_position(parents,root,9,[&](size_t j)->const vr_arm::Matrix&{return locals[j];}),"an index past the rig rejects");
    auto cycle=parents;cycle[1]=2;
    require(!vr_arm::chain_position(cycle,root,2,[&](size_t j)->const vr_arm::Matrix&{return locals[j];}),"parents must precede children");
}
}
int main() {
    try {
        armour_endpoint();
        shield_twist();
        chain_position();
        auto anchor=pose({10,20,30}); anchor[0]=-1;
        auto controller=pose({0.3f,-0.4f,-0.5f});
        auto wrist=vr_arm::wrist_world(anchor,controller,vr_camera::IDENTITY,0.9f);
        require(wrist && vr_camera::rigid(*wrist),"wrist orientation must be a proper native rotation");
        check(vr_arm::position(*wrist),{9.73f,19.64f,30.45f},"map reference metres once with shared camera origin");
        auto grip=pose({0.1f,0,0}); grip[0]=0;grip[2]=-1;grip[8]=1;grip[10]=0;
        auto calibrated=vr_arm::wrist_world(anchor,controller,grip,0.9f);
        require(calibrated && !close((*calibrated)[0],(*wrist)[0]),"grip orientation must reach native wrist rotation");
        check(vr_arm::position(*calibrated),{9.64f,19.64f,30.45f},"local grip offset uses physical metres exactly once");
        auto invalid=controller;invalid[0]=std::numeric_limits<float>::quiet_NaN();
        require(!vr_arm::wrist_world(anchor,invalid,grip,1) && !vr_arm::wrist_world(vr_camera::IDENTITY,controller,grip,1),"bad tracking and camera handedness must reject");
        std::vector<vr_arm::Matrix> locals{pose({0,0,0}),pose({0,1,0}),pose({0.4f,0,0}),pose({0.4f,0,0}),pose({0.4f,0,0}),pose({0.3f,0,0}),pose({0,0.2f,0}),pose({-0.4f,0,0})};
        const std::vector<int32_t> parents{-1,0,1,2,1,4,2,0};
        const auto original=locals;
        const std::array<vr_arm::Companion,2> helpers{{{4,2},{5,3}}};
        auto target=pose({0.45f,1.25f,-0.3f}); target[0]=0;target[2]=-1;target[8]=1;target[10]=0;
        const auto result=vr_arm::solve(locals,parents,vr_camera::IDENTITY,{1,2,3},target,{0,-1,0},helpers);
        require(result && !result->reach_limited && result->target_error<1e-4f,"reachable wrist target must solve");
        std::vector<std::string> names{"ROOT","ARM1R","ARM2R","HANDR","ARM2R_glove","HANDR_glove","forearm_R","OTHER"};
        const auto named=vr_arm::solve_named(locals,parents,names,vr_camera::IDENTITY,1,target,{0,-1,0});
        require(named && named->locals==result->locals,"actual named rig binding must solve the separate gauntlet branch");
        names[5]="OTHER_GLOVE"; require(!vr_arm::solve_named(locals,parents,names,vr_camera::IDENTITY,1,target,{0,-1,0}),"partial gauntlet hierarchy must reject"); names[5]="HANDR_glove";
        names[7]="HANDR"; require(!vr_arm::solve_named(locals,parents,names,vr_camera::IDENTITY,1,target,{0,-1,0}),"ambiguous hand names must reject");
        require(locals==original && result->locals[0]==original[0] && result->locals[6]==original[6] && result->locals[7]==original[7],"input, torso, forearm helper and other arm locals remain unchanged");
        std::vector<vr_arm::Matrix> world,before;
        require(vr_arm::hierarchy(result->locals,parents,vr_camera::IDENTITY,world) && vr_arm::hierarchy(original,parents,vr_camera::IDENTITY,before),"actual final locals produce a valid hierarchy");
        check(vr_arm::position(world[3]),vr_arm::position(target),"published final wrist position tracks target");
        for (int i=0;i<12;++i) { require(close(world[3][i],target[i]),"final wrist orientation tracks target"); }
        require(close(vr_arm::length(vr_arm::sub(vr_arm::position(world[2]),vr_arm::position(world[1]))),0.4f) && close(vr_arm::length(vr_arm::sub(vr_arm::position(world[3]),vr_arm::position(world[2]))),0.4f),"bone lengths remain constant");
        const auto glove_before=vr_camera::multiply(before[5],vr_camera::inverse(before[3]));
        const auto glove_after=vr_camera::multiply(world[5],vr_camera::inverse(world[3]));
        for (int i=0;i<16;++i) { require(close(glove_before[i],glove_after[i]),"separate glove branch preserves its wrist attachment"); }
        for (auto p:{vr_arm::Vector{5,1,0},vr_arm::Vector{0,1,0},vr_arm::Vector{-0.6f,1,0}}) {
            auto reach=vr_arm::solve(locals,parents,vr_camera::IDENTITY,{1,2,3},pose(p),{1,0,0});
            require(reach && std::isfinite(reach->target_error),"far, zero-distance and antiparallel cases stay finite");
        }
        auto bad_parents=parents;bad_parents[2]=2;
        {
            const auto far=pose({.4f,1.2f,-1.2f});
            const auto clamped=vr_arm::solve_named(locals,parents,names,vr_camera::IDENTITY,1,far,{0,-1,0});
            // Restore the deliberately duplicated name used by the ambiguity test above.
            auto tracked_names=names;tracked_names[7]="OTHER";
            const auto tracked=vr_arm::solve_named(locals,parents,tracked_names,vr_camera::IDENTITY,1,far,{0,-1,0},true);
            require(tracked && tracked->reach_limited && tracked->target_error<1e-4f,"tracked wrist has priority over native arm reach");
            require(vr_arm::hierarchy(tracked->locals,parents,vr_camera::IDENTITY,world),"extended wrist publication remains rigid");
            check(vr_arm::position(world[3]),vr_arm::position(far),"unreachable physical hand no longer clamps towards face");
            std::vector<vr_arm::Matrix> native;require(vr_arm::hierarchy(locals,parents,vr_camera::IDENTITY,native),"native arm hierarchy");
            const auto segment=[](const std::vector<vr_arm::Matrix>& w,size_t a,size_t b){return vr_arm::length(vr_arm::sub(vr_arm::position(w[a]),vr_arm::position(w[b])));};
            const float stretch=segment(world,1,2)/segment(native,1,2);
            require(stretch>1.01f && std::abs(stretch-segment(world,2,3)/segment(native,2,3))<1e-3f,"a long reach stretches both arm segments alike, not the forearm alone");
            const auto glove=vr_camera::multiply(world[5],vr_camera::inverse(world[3]));
            for(int i=0;i<16;++i) require(close(glove[i],glove_before[i]),"extended wrist also carries its glove");
            require(tracked->locals[0]==locals[0] && tracked->locals[7]==locals[7],"reach fix leaves body and unrelated locals native");
        }
        require(!vr_arm::solve(locals,bad_parents,vr_camera::IDENTITY,{1,2,3},target,{0,-1,0}),"cycles reject whole solve");
        auto bad_locals=locals;bad_locals[2]=pose({0,0,0});
        require(!vr_arm::solve(bad_locals,parents,vr_camera::IDENTITY,{1,2,3},target,{0,-1,0}),"zero-length chain rejects");
        auto moved_root=pose({10,0,3}); moved_root[0]=0;moved_root[2]=-1;moved_root[8]=1;moved_root[10]=0;
        auto moved_target=vr_camera::multiply(target,moved_root);
        auto moved=vr_arm::solve(locals,parents,moved_root,{1,2,3},moved_target,vr_arm::rotate({0,-1,0},moved_root),helpers);
        require(moved && vr_arm::hierarchy(moved->locals,parents,moved_root,world),"translated/turned character solve remains valid");
        check(vr_arm::position(world[3]),vr_arm::position(moved_target),"world target follows character transform without double root motion");
        std::cout << "native wrist mapping, reachable/clamped arm IK, orientation, helpers and isolation passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
