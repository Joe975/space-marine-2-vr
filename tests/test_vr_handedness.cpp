#include "VrHandedness.h"
#include "OpenXrInput.h"
#include "VrWeaponAim.h"
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
// Left-handed play (docs/LEFT_HANDED.md). The session and driver suites run the same swaps through
// the production pipeline; this one pins the geometry and the bindings they rest on.
namespace {
using vr_arm::Matrix;
using vr_arm::Vector;
void require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
bool same(const Matrix& a,const Matrix& b,float tolerance=1e-4f) { for(size_t i=0;i<a.size();++i) if(std::abs(a[i]-b[i])>tolerance) return false;return true; }
Matrix at(float x,float y,float z) { auto m=vr_camera::IDENTITY;vr_arm::position(m,{x,y,z});return m; }
// Turned about +Y, then moved: a head or hand in standing space.
Matrix yawed(float degrees,float x,float y,float z) {
    const float a=degrees*0.01745329252f;auto m=vr_camera::IDENTITY;
    m[0]=m[10]=std::cos(a);m[2]=-std::sin(a);m[8]=std::sin(a);vr_arm::position(m,{x,y,z});return m;
}
// An arbitrary rigid pose, so nothing passes by way of axis alignment.
Matrix tilted(float pitch,float yaw,float roll,Vector p) {
    vr_weapon::Settings s;s.values={0,0,0,pitch,yaw,roll};auto m=s.matrix();vr_arm::position(m,p);return m;
}
Matrix euler(float x,float y,float z) { return tilted(x,y,z,{0,0,0}); }
float degrees(const Matrix& a,const Matrix& b) { return vr_weapon::turn_degrees(a,b); }
float metres(const Matrix& a,const Matrix& b) { return vr_arm::length(vr_arm::sub(vr_arm::position(a),vr_arm::position(b))); }
// config/index_hands_accepted.calibration.
const Matrix LEFT_GRIP=euler(90,45,-90),RIGHT_GRIP=euler(-90,-45,90);

void mirror_is_a_true_reflection() {
    const auto pose=tilted(20,35,-50,{.3f,1.2f,-.4f});
    require(vr_camera::rigid(vr_hand::mirror(pose)),"a mirrored pose stays rigid, so every gesture's own check accepts it");
    require(same(vr_hand::mirror(vr_hand::mirror(pose)),pose),"mirroring twice is the original");
    const auto mirrored=vr_hand::mirror(pose);
    require(mirrored[12]==-pose[12] && mirrored[13]==pose[13] && mirrored[14]==pose[14],"only the side changes: height and depth hold");
}
// lateral as the gestures read it: along the head's right axis, level.
float lateral(const Matrix& head,const Matrix& hand) {
    const Vector right{head[0],0,head[2]};
    return vr_arm::dot(vr_arm::sub(vr_arm::position(hand),vr_arm::position(head)),vr_arm::scale(right,1/vr_arm::length(right)));
}
void the_player_mirrors_whole() {
    const auto head=yawed(70,.4f,1.7f,.2f);
    const std::array<std::optional<Matrix>,2> hands{yawed(10,.1f,1,.1f),yawed(-20,.8f,1,-.3f)};
    const auto same=vr_hand::logical(head,hands,false);
    require(same.head==head && same.hands==hands,"right-handed play passes every pose through untouched");
    const auto player=vr_hand::logical(head,hands,true);
    // Facing any way, the real left hand lands where the weapon hand's zones expect the right hand.
    const float real=lateral(head,*hands[0]);
    require(real<0,"the fixture's left hand is on the player's left");
    require(std::abs(lateral(player.head,*player.hands[1])+real)<1e-4f,"the physical left hand reads as the weapon hand, on the weapon hand's side");
    require(std::abs(metres(player.head,*player.hands[1])-metres(head,*hands[0]))<1e-4f,"the hand keeps its distance from the head");
    require(std::abs(metres(*player.hands[0],*player.hands[1])-metres(*hands[0],*hands[1]))<1e-4f,"the hands keep their distance from each other");
    const auto one=vr_hand::logical(head,{std::nullopt,hands[1]},true);
    require(one.hands[0] && !one.hands[1],"a lost left controller is a lost weapon hand, never a stale one");
    // A head turn must not move a still hand: the head mirrors in the same fixed plane.
    const auto before=vr_hand::logical(yawed(0,0,1.7f,0),{hands[0],std::nullopt},true),after=vr_hand::logical(yawed(30,0,1.7f,0),{hands[0],std::nullopt},true);
    require(metres(*before.hands[1],*after.hands[1])==0,"a 30 degree head turn adds no speed to a still hand, so it can't read as a swing");
}
void swap_mirrors_a_hold_through_the_controllers() {
    const auto swap=vr_hand::Swap::make(LEFT_GRIP,RIGHT_GRIP,.9f);
    require(swap.has_value(),"the swap builds from the accepted grips");
    const auto scaled=[](Matrix m){for(int i=0;i<3;++i)m[12+i]*=.9f;return m;};
    // The rigid chain the arm driver sees: wrist=grip*controller, weapon=relative*wrist.
    const auto relative=tilted(12,-40,70,{-.27f,.13f,.03f});
    const auto left_controller=tilted(30,-20,10,{-.3f,1.1f,-.5f});
    const auto left_wrist=vr_camera::multiply(scaled(LEFT_GRIP),left_controller);
    // What the mirrored player's right hand would hold, seen back in the mirror.
    const auto virtual_wrist=vr_camera::multiply(scaled(RIGHT_GRIP),vr_hand::mirror(left_controller));
    const auto expected=vr_hand::mirror(vr_camera::multiply(relative,virtual_wrist));
    const auto moved=swap->moved(relative,1);
    require(vr_camera::rigid(moved),"the moved fit is a proper rotation: the mesh is never turned inside out");
    require(same(vr_camera::multiply(moved,left_wrist),expected),"the weapon sits in the left hand as the mirror image of the right-hand hold");
    require(same(swap->moved(swap->moved(relative,1),0),relative),"moving a hold there and back is the original");
    auto offset=LEFT_GRIP;vr_arm::position(offset,{.01f,.02f,-.03f});
    const auto tuned=vr_hand::Swap::make(offset,RIGHT_GRIP,.9f);
    require(tuned && !same(tuned->moved(relative,1),moved),"grip offsets count: a retuned hand moves the weapon with it");
    require(!vr_hand::Swap::make(LEFT_GRIP,RIGHT_GRIP,0) && !vr_hand::Swap::make(LEFT_GRIP,RIGHT_GRIP,std::nanf("")),"no fit from unusable units");
}
Matrix rows(std::array<float,3> x,std::array<float,3> y,std::array<float,3> z,std::array<float,3> p) {
    return {x[0],x[1],x[2],0, y[0],y[1],y[2],0, z[0],z[1],z[2],0, p[0],p[1],p[2],1};
}
Matrix orthonormal(Matrix m) { // The dumps carry eight digits: square them up for rigid().
    Vector x{m[0],m[1],m[2]},y{m[4],m[5],m[6]};x=vr_arm::scale(x,1/vr_arm::length(x));
    auto z=vr_arm::cross(x,y);z=vr_arm::scale(z,1/vr_arm::length(z));y=vr_arm::cross(z,x);
    for(int i=0;i<3;++i){m[i]=x[i];m[4+i]=y[i];m[8+i]=z[i];}return m;
}
// Real bone dumps from the 31 Oct 20:29 playtest. Gun_pose on GUN_R (20:39:25) and on GUN_L
// (20:33:48), each relative to its native hand, and GUN_R carrying the chainsword (20:33:48).
void the_game_agrees() {
    const auto bolter=orthonormal(rows({-.05594f,.09113f,.99427f},{.02176f,.9957f,-.09004f},{-.9982f,.0166f,-.05768f},{-.26628f,.12895f,.03362f}));
    const auto pistol=orthonormal(rows({.21542f,.09392f,.97199f},{.1084f,-.99151f,.07178f},{.97049f,.0899f,-.22377f},{.26028f,-.11244f,-.06605f}));
    const auto sword=orthonormal(rows({-.12834f,-.26581f,.95544f},{.80921f,.52889f,.25584f},{-.57333f,.80599f,.14722f},{-.08456f,-.02277f,.04772f}));
    const auto swap=vr_hand::Swap::make(LEFT_GRIP,RIGHT_GRIP,1);
    require(swap.has_value(),"swap");
    // The game's own left-hand hold is the check: the same reflection of the bolter's right-hand
    // fit lands within 17.4 degrees and 3.7 cm of it, and they are different guns. A wrong axis
    // or a wrong side lands 90 or 180 degrees away.
    const auto moved=swap->moved(bolter,1);
    require(degrees(moved,pistol)<18,"the mirrored bolter lies along the game's own left-hand hold");
    require(metres(moved,pistol)<.04f,"the mirrored bolter sits in the game's own left-hand grip");
    const auto back=swap->moved(pistol,0);
    require(degrees(back,bolter)<18 && metres(back,bolter)<.04f,"the mirrored pistol lies along the game's own right-hand hold");
    // Seen from the controller the sword keeps its blade and flips sideways, nothing else.
    const auto held=vr_camera::multiply(sword,RIGHT_GRIP),swapped=vr_camera::multiply(swap->moved(sword,1),LEFT_GRIP);
    require(same(swapped,vr_hand::mirror(held)),"the sword points the same way out of the left controller");
    require(std::abs(held[0])>.95f && std::abs(vr_camera::multiply(bolter,RIGHT_GRIP)[0])>.95f && std::abs(vr_camera::multiply(pistol,LEFT_GRIP)[0])>.95f,
        "X is the sideways axis of all three: it lies along the controller's X");
}
struct Rig {
    std::vector<std::string> names{"ROOT","BACKup","ARM2L","HANDL","GUN_L","ARM2R","HANDR","GUN_R","GUN_ANIM","Gun_pose"};
    std::vector<int32_t> parents{-1,0,1,2,3,1,5,6,1,8};
    std::vector<Matrix> locals{at(0,0,0),at(0,1,0),at(-.3f,.3f,0),at(-.4f,0,0),tilted(5,10,15,{-.1f,0,0}),at(.3f,.3f,0),at(.4f,0,0),tilted(-5,20,-15,{.1f,0,0}),at(.1f,.2f,.3f),at(.2f,0,0)};
    enum { HANDL=3,GUNL=4,HANDR=6,GUNR=7,ANIM=8,GUN=9 };
    // Put Gun_pose on a socket, as the animation does.
    void seat(size_t socket) {
        std::vector<Matrix> w;require(vr_arm::hierarchy(locals,parents,vr_camera::IDENTITY,w),"rig");
        locals[GUN]=vr_camera::multiply(w[socket],vr_camera::inverse(w[ANIM]));
    }
};
void weapons_change_wrists() {
    const auto swap=vr_hand::Swap::make(LEFT_GRIP,RIGHT_GRIP,1);
    for(const size_t socket:{size_t{Rig::GUNR},size_t{Rig::GUNL}}) {
        Rig rig;rig.seat(socket);
        const bool primary=socket==Rig::GUNR;
        auto solved=rig.locals;solved[Rig::HANDL]=tilted(10,20,30,{-.5f,.1f,.2f});solved[Rig::HANDR]=tilted(-30,40,10,{.5f,-.1f,.2f});
        std::vector<Matrix> before,after;
        require(vr_arm::hierarchy(rig.locals,rig.parents,vr_camera::IDENTITY,before) && vr_arm::hierarchy(solved,rig.parents,vr_camera::IDENTITY,after),"rig solves");
        const auto plain=vr_weapon::retarget(rig.locals,solved,rig.parents,rig.names,vr_camera::IDENTITY,vr_camera::IDENTITY,1);
        const auto moved=vr_weapon::retarget(rig.locals,solved,rig.parents,rig.names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,std::nullopt,std::nullopt,std::nullopt,nullptr,{},true,&*swap);
        require(plain && moved && moved->hand==plain->hand && moved->grip==plain->grip && same(moved->relative,plain->relative),"the reading and the fit stay the game's: routing never learns of the swap");
        require(moved->hand==(primary?1u:0u),"the logical hand is the game's own");
        const size_t native=primary?Rig::HANDR:Rig::HANDL,other=primary?Rig::HANDL:Rig::HANDR;
        const auto relative=vr_camera::multiply(before[socket],vr_camera::inverse(before[native]));
        require(same(moved->world,vr_camera::multiply(swap->moved(relative,primary?1:0),after[other])),primary?"the primary rides the left wrist":"the pistol rides the right wrist");
        auto applied=solved;applied[moved->index]=moved->local;std::vector<Matrix> committed;
        require(vr_arm::hierarchy(applied,rig.parents,vr_camera::IDENTITY,committed) && same(committed[Rig::GUN],moved->world),"the local reconstructs the moved gun");
        // A live grip tweak still means the weapon's own axes, mirrored with it.
        vr_weapon::Settings tune;tune.values={.01f,.02f,0,5,0,0};
        const auto tuned=vr_weapon::retarget(rig.locals,solved,rig.parents,rig.names,vr_camera::IDENTITY,tune.matrix(),1,std::nullopt,std::nullopt,std::nullopt,nullptr,{},true,&*swap);
        require(tuned && same(tuned->world,vr_camera::multiply(swap->moved(vr_camera::multiply(tune.matrix(),relative),primary?1:0),after[other])),"weapon tuning is mirrored with the fit");
    }
    Rig rig;rig.seat(Rig::GUNR);
    std::vector<std::string> names=rig.names;names[Rig::HANDL]="HAND_OTHER";
    require(!vr_weapon::retarget(rig.locals,rig.locals,rig.parents,names,vr_camera::IDENTITY,vr_camera::IDENTITY,1,1,std::nullopt,std::nullopt,nullptr,{},true,&*swap),"a rig with no left hand carries no swapped gun");
}
void sword_socket_moves() {
    const auto swap=vr_hand::Swap::make(LEFT_GRIP,RIGHT_GRIP,1);
    Rig rig;
    rig.locals[Rig::HANDL]=tilted(10,20,30,{-.5f,.1f,.2f});rig.locals[Rig::HANDR]=tilted(-30,40,10,{.5f,-.1f,.2f});
    const auto socket=vr_hand::sword_to_left(rig.locals,rig.parents,rig.names,vr_camera::IDENTITY,*swap);
    require(socket && socket->index==Rig::GUNR,"GUN_R is the socket that moves");
    auto applied=rig.locals;applied[socket->index]=socket->local;
    std::vector<Matrix> before,after;
    require(vr_arm::hierarchy(rig.locals,rig.parents,vr_camera::IDENTITY,before) && vr_arm::hierarchy(applied,rig.parents,vr_camera::IDENTITY,after),"rig");
    require(same(after[Rig::GUNR],vr_camera::multiply(swap->moved(rig.locals[Rig::GUNR],1),before[Rig::HANDL]),1e-3f),"the sword rides the left wrist, held as the right hand held it");
    require(metres(after[Rig::GUNR],before[Rig::HANDL])<.2f && metres(after[Rig::GUNR],before[Rig::HANDR])>.5f,"GUN_R has left the right hand");
    require(applied[Rig::HANDR]==rig.locals[Rig::HANDR] && applied[Rig::GUN]==rig.locals[Rig::GUN],"only the socket is rewritten");
    auto parents=rig.parents;parents[Rig::GUNR]=Rig::ANIM;
    require(!vr_hand::sword_to_left(rig.locals,parents,rig.names,vr_camera::IDENTITY,*swap),"a GUN_R not under HANDR is left alone");
    auto names=rig.names;names[Rig::GUNR]="OTHER";
    require(!vr_hand::sword_to_left(rig.locals,rig.parents,names,vr_camera::IDENTITY,*swap),"a rig with no sword socket is left alone");
}
std::map<std::string,std::string> table(const openxr_input::Profile& profile,vr_hand::Settings hand) {
    std::map<std::string,std::string> out;
    for(const auto& b:openxr_input::bindings(profile,hand)) out[openxr_input::ACTIONS[b.action].name]=b.path;
    return out;
}
void viewer_grips() {
    require(vr_hand::physical_grips(.2f,.8f,false)==std::array{.2f,.8f},"right-handed: parry is the left hand's grip");
    require(vr_hand::physical_grips(.2f,.8f,true)==std::array{.8f,.2f},"left-handed: melee is the left hand's grip");
}
void bindings() {
    using openxr_input::PROFILES;
    const vr_hand::Settings LEFT{true,false},KEEP{true,true};
    for(const auto& profile:PROFILES) {
        const auto right=table(profile,{});
        require(right.size()==profile.bindings.size(),"every action keeps one binding");
        for(const auto& b:profile.bindings) require(right.at(openxr_input::ACTIONS[b.action].name)==b.path,"right-handed play keeps the table exactly");
        const auto left=table(profile,LEFT);
        for(const auto& [action,path]:left) {
            const auto was=right.at(action);
            require(path!=was && was.starts_with("/user/hand/left/")==path.starts_with("/user/hand/right/"),"every action moves to the other hand");
            require(openxr_input::mirrored(path,profile.touch_layout)==was,"swapping twice is the original");
        }
    }
    const auto index=table(PROFILES[0],LEFT);
    require(index.at("fire")=="/user/hand/left/input/trigger/value" && index.at("fire_touch")=="/user/hand/left/input/trigger/touch","the gun's trigger is the left trigger");
    require(index.at("melee")=="/user/hand/left/input/squeeze/force" && index.at("parry")=="/user/hand/right/input/squeeze/force","the weapon grip is the left grip");
    require(index.at("utility")=="/user/hand/right/input/trackpad" && index.at("grenade")=="/user/hand/left/input/trackpad/force","the trackpads change over, axes and all");
    require(index.at("confirm")=="/user/hand/left/input/a/click" && index.at("menu")=="/user/hand/right/input/b/click","Index has A and B on both hands");
    require(index.at("move")=="/user/hand/right/input/thumbstick" && index.at("turn")=="/user/hand/left/input/thumbstick","the sticks change over by default");
    const auto touch=table(PROFILES[1],LEFT);
    require(touch.at("confirm")=="/user/hand/left/input/x/click" && touch.at("interact")=="/user/hand/left/input/y/click","Touch's right A and B are X and Y on the left");
    require(touch.at("recenter")=="/user/hand/right/input/a/click" && touch.at("menu")=="/user/hand/right/input/b/click","Touch's left X and Y are A and B on the right");
    for(const auto& profile:PROFILES) {
        const auto right=table(profile,{}),kept=table(profile,KEEP),left=table(profile,LEFT);
        for(const auto& [action,path]:kept) {
            const bool stick=action=="move" || action=="sprint" || action=="turn" || action=="finisher";
            require(path==(stick?right:left).at(action),"keep_sticks leaves the sticks and their clicks, and swaps everything else");
        }
    }
    require(openxr_input::mirrored("/interaction_profiles/valve/index_controller",false)=="/interaction_profiles/valve/index_controller","only a hand path mirrors");
}
}
int main() {try {
    mirror_is_a_true_reflection();the_player_mirrors_whole();swap_mirrors_a_hold_through_the_controllers();the_game_agrees();
    weapons_change_wrists();sword_socket_moves();viewer_grips();bindings();
    std::cout<<"vr_handedness passed\n";return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
