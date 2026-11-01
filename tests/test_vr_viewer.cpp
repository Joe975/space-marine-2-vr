#include "VrViewer.h"
#include "VrTapHold.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Matrix=vr_camera::Matrix;using Vec=vr_viewer::Vec;
void require(bool b,const std::string& message){if(!b)throw std::runtime_error(message);}
bool near(float a,float b,float tolerance=1e-3f){return std::abs(a-b)<=tolerance;}
bool near(const Vec& a,const Vec& b,float tolerance=1e-3f){return near(a[0],b[0],tolerance) && near(a[1],b[1],tolerance) && near(a[2],b[2],tolerance);}
Matrix yawed(float degrees,Vec position,bool native_handedness=false){
    const float a=degrees*.01745329252f,c=std::cos(a),s=std::sin(a);
    // XR-style yaw (det +1); the native camera may mirror its right row (det -1).
    Matrix m{c,0,-s,0, 0,1,0,0, s,0,c,0, position[0],position[1],position[2],1};
    if(native_handedness) for(int j=0;j<3;++j) m[j]=-m[j];
    return m;
}
float determinant(const Matrix& m){return m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);}
Vec at(const Matrix& m){return {m[12],m[13],m[14]};}

void entry_matches_first_person(){
    for(bool native:{false,true}){
        // Pitched first-person anchor: compose levels it, so the entry anchor must too.
        auto first_person=yawed(37,{12,4.4f,-3},native);
        const float p=.3f,c=std::cos(p),s=std::sin(p);
        for(int j=0;j<3;++j){const float up=first_person[4+j],fw=first_person[8+j];first_person[4+j]=c*up-s*fw;first_person[8+j]=s*up+c*fw;}
        const auto relative_head=yawed(-50,{.2f,.1f,-.3f});
        const float height=-.12f,forward=.08f,scale=.9f;
        const auto anchor=vr_viewer::entry_anchor(first_person,relative_head,height,forward,scale);
        require(anchor.has_value(),"entry anchor builds from a pitched first-person anchor");
        require(std::signbit(determinant(*anchor))==native,"entry anchor keeps the native handedness");
        Matrix heading{};vr_camera::level_anchor(relative_head,heading);
        auto eye=yawed(-50,{.23f,.12f,-.31f});auto offset_eye=eye;
        offset_eye[13]+=height;for(int j:{0,2})offset_eye[12+j]-=forward*heading[8+j];
        Matrix fp_world{},fp_view{},viewer_world{},viewer_view{};
        require(vr_camera::compose(first_person,offset_eye,scale,fp_world,fp_view) && vr_camera::compose(*anchor,eye,scale,viewer_world,viewer_view),"both compose");
        for(int i=0;i<16;++i) require(near(fp_world[i],viewer_world[i]),"first viewer frame equals the last first-person frame, element "+std::to_string(i));
    }
}

VrViewerRig::Input still(){VrViewerRig::Input in;in.head=yawed(0,{0,0,0});return in;}

void slide_and_vertical(){
    for(float scale:{.9f,9.f}){
        VrViewerRig rig;const auto anchor=yawed(20,{5,2,7});require(rig.begin(anchor,scale),"begin");
        auto in=still();in.move_y=1;
        const auto before=vr_viewer::world(rig.snapshot().anchor,scale,{0,0,0});
        for(int i=0;i<20;++i) rig.update(in,.05f);
        const auto after=vr_viewer::world(rig.snapshot().anchor,scale,{0,0,0});
        const auto expected=vr_viewer::world(anchor,scale,{0,0,-2.5f}); // 2.5 m/s for one second, room forward
        require(near(after,expected),"full stick slides 2.5 physical metres per second along the head's forward");
        require(near(vr_viewer::length(vr_viewer::sub(after,before)),2.5f*scale,1e-2f),"apparent speed is constant: world speed follows the scale");
    }
    VrViewerRig rig;rig.begin(yawed(0,{0,0,0}),1);
    auto in=still();in.head=yawed(90,{.5f,0,0});in.move_y=1;rig.update(in,.1f);
    // Head turned 90 degrees (yawed() turns left for positive angles): slide follows its forward.
    Matrix level{};vr_camera::level_anchor(in.head,level);
    const Vec forward{-level[8]*.25f,0,-level[10]*.25f};
    require(near(vr_viewer::world(rig.snapshot().anchor,1,{0,0,0}),vr_viewer::world(yawed(0,{0,0,0}),1,forward)),"slide follows head yaw, not the room axis");
    in=still();in.turn_y=1;rig.begin(yawed(0,{0,0,0}),2);rig.update(in,.1f);
    require(near(rig.snapshot().anchor[13],2*1.5f*.1f),"right stick up flies up at 1.5 m/s");
    in.turn_y=.25f;const auto y=rig.snapshot().anchor[13];rig.update(in,.1f);require(near(rig.snapshot().anchor[13],y),"vertical dead zone");
    in=still();in.move_x=.15f;const auto p=at(rig.snapshot().anchor);rig.update(in,.1f);require(near(at(rig.snapshot().anchor),p),"slide dead zone");
    in.move_y=1;rig.update(in,5);require(near(vr_viewer::length(vr_viewer::sub(at(rig.snapshot().anchor),p)),2*2.5f*.1f,1e-3f),"a long frame gap is clamped to 0.1 s");
}

void snap_turn_about_head(){
    for(bool native:{false,true}){
        VrViewerRig rig;const auto anchor=yawed(-30,{3,1,2},native);rig.begin(anchor,1.5f);
        auto in=still();in.head=yawed(10,{.4f,1.6f,-.2f});in.snap_degrees=90;
        const auto head_world=vr_viewer::world(rig.snapshot().anchor,1.5f,at(in.head));
        in.turn_x=1;rig.update(in,.01f);require(!rig.snapshot().snaps,"a stick already pushed when the viewer begins must centre first");
        in.turn_x=0;rig.update(in,.01f);in.turn_x=1;rig.update(in,.01f);
        const auto turned=rig.snapshot().anchor;
        require(rig.snapshot().snaps==1 && near(vr_viewer::world(turned,1.5f,at(in.head)),head_world),"snap turn keeps the head's world position");
        require(near(vr_viewer::direction(turned,{0,0,-1}),vr_viewer::direction(anchor,{1,0,0})),"snap right brings what was on the right to the front");
        require(std::signbit(determinant(turned))==native,"snap keeps handedness");
        rig.update(in,.01f);require(rig.snapshot().snaps==1,"one snap per push");
        in.turn_x=.2f;rig.update(in,.01f);in.turn_x=-1;rig.update(in,.01f);
        require(rig.snapshot().snaps==2 && near(vr_viewer::direction(rig.snapshot().anchor,{1,0,0}),vr_viewer::direction(anchor,{1,0,0})),"released stick re-arms; a left snap undoes it");
    }
}

void one_hand_drag(){
    VrViewerRig rig;rig.begin(yawed(15,{1,2,3}),2);
    auto in=still();const Vec h0{.2f,1.1f,-.4f};in.hands={Vec{-.2f,1.1f,-.4f},h0};in.right_grip=.7f;rig.update(in,.01f);
    const auto grabbed=vr_viewer::world(rig.snapshot().anchor,2,h0);
    const Vec h1{.5f,1.3f,-.1f};in.hands[1]=h1;in.move_y=1;rig.update(in,.01f);
    require(rig.snapshot().grab==VrViewerRig::Grab::right && near(vr_viewer::world(rig.snapshot().anchor,2,h1),grabbed),"the world point grabbed by one hand stays under it; sticks wait");
    in.right_grip=.5f;in.hands[1]=Vec{.6f,1.3f,-.1f};rig.update(in,.01f);require(rig.snapshot().grab==VrViewerRig::Grab::right,"grip hysteresis holds between release and press");
    in.right_grip=.3f;in.move_y=0;const auto released=rig.snapshot().anchor;rig.update(in,.01f);
    require(rig.snapshot().grab==VrViewerRig::Grab::none && near(at(rig.snapshot().anchor),at(released)),"release leaves the world where it was");
    in.hands[1].reset();in.right_grip=1;rig.update(in,.01f);require(rig.snapshot().grab==VrViewerRig::Grab::none,"an untracked hand cannot grab");
}

void two_hand_scale_and_turn(){
    for(bool native:{false,true}){
        VrViewerRig rig;const auto anchor=yawed(70,{-4,3,9},native);rig.begin(anchor,.9f);
        auto in=still();const Vec l0{-.2f,1.2f,-.4f},r0{.2f,1.2f,-.4f};in.hands={l0,r0};in.left_grip=in.right_grip=1;
        rig.update(in,.01f);require(rig.snapshot().grab==VrViewerRig::Grab::both,"both grips grab");
        const auto left_world=vr_viewer::world(anchor,.9f,l0),right_world=vr_viewer::world(anchor,.9f,r0);
        // Stretch to 0.6 m apart, turn the hand line 30 degrees, and carry the midpoint.
        const Vec centre{.1f,1.25f,-.3f};const auto half=vr_viewer::yaw({.3f,0,0},.5235988f);
        const Vec l1=vr_viewer::sub(centre,half),r1{centre[0]+half[0],centre[1]+half[1],centre[2]+half[2]};
        in.hands={l1,r1};in.move_y=1;in.turn_x=1;rig.update(in,.01f);
        const auto s=rig.snapshot();
        require(near(s.units_per_metre,.9f*.4f/.6f),"hands apart make the world bigger: scale follows the distance ratio");
        require(near(vr_viewer::world(s.anchor,s.units_per_metre,l1),left_world) && near(vr_viewer::world(s.anchor,s.units_per_metre,r1),right_world),
            "both grabbed world points stay under their hands through stretch and turn");
        require(near(s.anchor[5],1) && std::signbit(determinant(s.anchor))==native && s.snaps==0,"rotation stays about the vertical axis, keeps handedness, sticks ignored");
        // Releasing one grip hands over to a one-hand drag without a jump.
        in.left_grip=0;rig.update(in,.01f);
        require(rig.snapshot().grab==VrViewerRig::Grab::right && near(vr_viewer::world(rig.snapshot().anchor,rig.snapshot().units_per_metre,r1),right_world),"two to one hand keeps the world still");
    }
    VrViewerRig rig;rig.begin(yawed(0,{0,0,0}),1);
    auto in=still();in.hands={Vec{-.1f,1,-.3f},Vec{.1f,1,-.3f}};in.left_grip=in.right_grip=1;rig.update(in,.01f);
    in.hands={Vec{-3,1,-.3f},Vec{3,1,-.3f}};rig.update(in,.01f); // 0.2 / 6 would be 0.033
    require(near(rig.snapshot().units_per_metre,.2f) && near(vr_viewer::world(rig.snapshot().anchor,.2f,{0,1,-.3f}),vr_viewer::world(yawed(0,{0,0,0}),1,{0,1,-.3f})),
        "scale clamps at the minimum and the midpoint stays put");
    in.hands={Vec{0,.9f,-.3f},Vec{0,1.1f,-.3f}};rig.update(in,.01f);
    require(std::isfinite(rig.snapshot().anchor[0]) && near(rig.snapshot().anchor[5],1),"a vertical hand line keeps the last turn");
    rig.end();const auto frozen=rig.snapshot().anchor;in.hands={Vec{-1,1,0},Vec{1,1,0}};rig.update(in,.01f);
    require(!rig.engaged() && rig.snapshot().anchor==frozen,"an ended rig ignores input");
}

// Grounded scale (docs/VIEWER_MODE.md, 21 Oct playtest): the stereo baseline, head motion and
// eye height above the floor must agree on one scale. Joe's eyes are 1.65 m above his floor;
// the character stands on y=2.1 and its first-person eye is 2.3 units above that.
namespace grounded {
constexpr float LIFE=.9f,EYE=1.65f,FLOOR=2.1f;
const VrViewerRig::Ground GROUND{FLOOR,-EYE};
float eye_height_metres(const VrViewerRig& rig,const Matrix& head){
    const auto s=rig.snapshot();return (vr_viewer::world(s.anchor,s.units_per_metre,at(head))[1]-FLOOR)/s.units_per_metre;}
// Stereo baseline of a 64 mm IPD in world units, from the real composition.
float baseline(const VrViewerRig& rig,const Matrix& head){
    const auto s=rig.snapshot();Matrix l=head,r=head,lw{},lv{},rw{},rv{};
    for(int j=0;j<3;++j){l[12+j]-=.032f*head[j];r[12+j]+=.032f*head[j];}
    require(vr_camera::compose(s.anchor,l,s.units_per_metre,lw,lv) && vr_camera::compose(s.anchor,r,s.units_per_metre,rw,rv),"eyes compose");
    return vr_viewer::length(vr_viewer::sub(at(lw),at(rw)));}
}
void grounded_entry_and_presets(){
    using namespace grounded;
    for(bool native:{false,true}){
        const auto first_person=yawed(0,{12,FLOOR+2.3f,-3},native);
        VrViewerRig rig;require(rig.begin(first_person,LIFE,GROUND),"begin with a ground");
        auto in=still();rig.update(in,.02f);
        auto s=rig.snapshot();
        require(s.grounded && s.preset==0 && near(s.units_per_metre,LIFE),"entry is life size and grounded");
        // Beside the character: 1 m right, 0.5 m back of where its head was, at the player's own height.
        const auto eye=vr_viewer::world(s.anchor,s.units_per_metre,{0,0,0});
        const Vec right=vr_viewer::direction(first_person,{1,0,0}),back=vr_viewer::direction(first_person,{0,0,.5f});
        require(near(eye[0],12+LIFE*(right[0]+back[0])) && near(eye[2],-3+LIFE*(right[2]+back[2])),"entry stands 1 m beside and 0.5 m behind the character");
        require(near(eye_height_metres(rig,in.head),EYE),"entry eye height above the floor is the real eye height");
        // Presets: life, 1:12 action figure, world x4. Each grounded at the real eye height, and
        // the stereo baseline over the eye height is the same physical ratio at every scale.
        const float ratio=baseline(rig,in.head)/(eye_height_metres(rig,in.head)*s.units_per_metre);
        const Vec ahead{0,-EYE,-.8f};
        for(int press=1;press<=4;++press){
            const auto pivot=vr_viewer::world(rig.snapshot().anchor,rig.snapshot().units_per_metre,ahead);
            in.preset=1;rig.update(in,.02f);in.preset=0;rig.update(in,.02f);
            s=rig.snapshot();const int expected=press%3;
            require(s.preset==expected && near(s.units_per_metre,LIFE*std::array{1.f,12.f,.25f}[expected],1e-4f),"the right trigger cycles life, action figure, giant world");
            require(s.grounded && near(eye_height_metres(rig,in.head),EYE),"every preset stands the player on the floor at their real eye height");
            require(near(baseline(rig,in.head)/(eye_height_metres(rig,in.head)*s.units_per_metre),ratio),"stereo baseline and eye height agree at every scale");
            require(near(vr_viewer::world(s.anchor,s.units_per_metre,ahead),pivot,1e-3f*std::max(1.f,s.units_per_metre)),"the floor point just ahead stays put while the world scales");
        }
    }
}
void grounded_moves(){
    using namespace grounded;
    VrViewerRig rig;rig.begin(yawed(0,{0,FLOOR+2.3f,0}),LIFE,GROUND);
    auto in=still();in.preset=1;rig.update(in,.02f); // held through entry: no cycle
    require(rig.snapshot().preset==0 && rig.snapshot().presets==0,"a trigger held at entry does not cycle");
    in.preset=0;rig.update(in,.02f);
    // Two hands apart: scale, but the floor stays at the physical floor.
    in.hands={Vec{-.2f,-.4f,-.4f},Vec{.2f,-.4f,-.4f}};in.left_grip=in.right_grip=1;rig.update(in,.02f);
    in.hands={Vec{-.4f,-.4f,-.4f},Vec{.4f,-.4f,-.4f}};rig.update(in,.02f);
    require(near(rig.snapshot().units_per_metre,LIFE/2) && rig.snapshot().preset==-1,"hands apart still scale freely");
    require(rig.snapshot().grounded && near(eye_height_metres(rig,in.head),EYE),"grounded scaling keeps the floor under the feet");
    // One hand lifted: drags horizontally only.
    in.right_grip=0;rig.update(in,.02f);in.hands[0]=Vec{-.1f,.3f,-.4f};rig.update(in,.02f);
    require(near(eye_height_metres(rig,in.head),EYE),"a grounded drag never lifts the world");
    // Flying up leaves the ground; the next preset puts the player back on it.
    in.left_grip=0;in.hands={};rig.update(in,.02f);in.turn_y=1;rig.update(in,.1f);in.turn_y=0;
    require(!rig.snapshot().grounded && !near(eye_height_metres(rig,in.head),EYE),"flying up leaves the ground");
    in.preset=1;rig.update(in,.02f);in.preset=0;rig.update(in,.02f);
    require(rig.snapshot().grounded && rig.snapshot().preset==0 && near(eye_height_metres(rig,in.head),EYE),"a preset re-grounds (from a free scale, the cycle restarts at life size)");
    // No ground (body unknown): the old continuous entry, ungrounded.
    VrViewerRig loose;const auto fp=yawed(0,{0,FLOOR+2.3f,0});loose.begin(fp,LIFE,VrViewerRig::Ground{FLOOR,-.1f});loose.update(still(),.02f);
    require(!loose.snapshot().grounded && near(vr_viewer::world(loose.snapshot().anchor,LIFE,{0,0,0}),at(fp)),"an implausible floor is ignored: entry stays where first person left off");
}
void sequencer(){
    using S=VrViewerSequencer;using namespace std::chrono;
    const auto t0=steady_clock::time_point{seconds(100)};
    S seq;
    require(!seq.request(std::nullopt,t0) && !seq.request(0x108,t0) && !seq.request(0x10,t0),"refuses without flags, in the player's own photo mode and in cinematics");
    require(seq.request(0,t0) && seq.state()==S::State::releasing,"gameplay request first releases the character");
    auto step=seq.update(0,true,t0,false);require(!step.buttons && seq.state()==S::State::releasing,"no pause until the game republishes its own pose");
    require(!seq.request(0,t0+milliseconds(10)),"a second request mid-transition is ignored");
    step=seq.update(0,true,t0,true);require(step.buttons==S::START && step.until==t0+S::PULSE && seq.unreleased()==0,"released character: Start pulse");
    step=seq.update(0x908,true,t0+milliseconds(100));require(seq.state()==S::State::opening_pause && step.buttons==S::START,"pause seen while Start is held: wait for release and gap");
    step=seq.update(0x908,true,t0+milliseconds(299));require(!step.buttons && seq.state()==S::State::opening_pause,"gap before the next press");
    step=seq.update(0x908,true,t0+milliseconds(300));require(seq.state()==S::State::opening_photo && step.buttons==S::X,"then hold X on the pause menu");
    step=seq.update(0x908,true,t0+milliseconds(1500));require(step.buttons==S::X,"X stays held until photo mode shows");
    step=seq.update(0x108,true,t0+milliseconds(1600));require(seq.state()==S::State::active && step.outcome==S::Outcome::entered && !step.buttons,"photo flags: active, X released");
    step=seq.update(0x108,false,t0+seconds(5));require(seq.state()==S::State::active,"focus loss does not end an active viewer");
    const auto t1=t0+seconds(10);
    require(seq.request(0x108,t1) && seq.state()==S::State::closing_photo,"second request leaves");
    step=seq.update(0x108,true,t1);require(step.buttons==S::B,"B leaves photo mode");
    step=seq.update(0x908,true,t1+milliseconds(310));require(seq.state()==S::State::closing_pause && step.buttons==S::START,"then Start closes the pause menu");
    step=seq.update(0,true,t1+milliseconds(400));require(seq.state()==S::State::closing_pause,"gameplay seen before the gap: wait");
    step=seq.update(0,true,t1+milliseconds(620));require(seq.state()==S::State::off && step.outcome==S::Outcome::exited && !step.buttons,"back to gameplay");
    // From the pause menu the character froze with first-person edits: resume, release, pause again.
    const auto t2=t1+seconds(1);
    require(seq.request(0x908,t2) && seq.update(0x908,true,t2).buttons==S::START && seq.state()==S::State::resuming,"request on the pause menu first resumes the game");
    seq.update(0,true,t2+milliseconds(200),false);require(seq.state()==S::State::resuming,"wait for the release and gap");
    seq.update(0,true,t2+milliseconds(300),false);require(seq.state()==S::State::releasing,"back in gameplay: release the character");
    step=seq.update(0,true,t2+milliseconds(300)+S::RELEASE_LIMIT,false);
    require(step.buttons==S::START && seq.unreleased()==1,"no publication within the limit: pause anyway, counted");
    step=seq.update(0x908,true,t2+seconds(1));require(step.buttons==S::X,"then X on the pause menu");
    step=seq.update(0x908,true,t2+seconds(1)+S::HOLD_LIMIT+S::WAIT);require(seq.state()==S::State::off && step.outcome==S::Outcome::failed && seq.failures()==1,"photo mode never showing fails after the hold limit and wait");
    require(seq.request(0,t2+seconds(10)) && seq.update(0x908,true,t2+seconds(10),false).outcome==S::Outcome::failed,"the player pausing during the release fails cleanly");
    require(seq.request(0,t1+seconds(20)),"restart");
    require(seq.update(std::nullopt,true,t1+seconds(20)).outcome==S::Outcome::failed,"losing the flags fails a transition");
    require(seq.request(0,t1+seconds(30)) && seq.update(0,false,t1+seconds(30)).outcome==S::Outcome::failed,"focus loss fails a transition");
    require(seq.request(0,t1+seconds(40)),"enter");seq.update(0,true,t1+seconds(40));
    seq.update(0x908,true,t1+seconds(40)+milliseconds(400));seq.update(0x108,true,t1+seconds(41));require(seq.state()==S::State::active,"active");
    step=seq.update(0x908,true,t1+seconds(42));require(seq.state()==S::State::off && step.outcome==S::Outcome::exited,"the game leaving photo mode by itself ends the viewer");
    require(seq.request(0,t1+seconds(50)),"start");seq.update(0,true,t1+seconds(50));
    step=seq.update(0,true,t1+seconds(50)+S::PULSE+S::WAIT);require(step.outcome==S::Outcome::failed,"no pause menu after Start fails");
}

// Joe 26 Oct: photo mode should keep the pose he held (arms, hands, fingers). The arm driver
// keeps posing until the freeze; the head is never hidden in any viewer phase.
void pose_kept_until_the_freeze(){
    using S=VrViewerSequencer;using E=vr_viewer::PoseEdits;using namespace std::chrono;
    require(vr_viewer::pose_edits(true,S::State::off,true)==E{true,true} && vr_viewer::pose_edits(true,S::State::off,false)==E{true,true},"first person: arms and head hiding");
    require(vr_viewer::pose_edits(false,S::State::off,true)==E{},"other views: no first-person edits");
    for(const auto state:{S::State::resuming,S::State::releasing,S::State::opening_pause,S::State::opening_photo}){
        require(vr_viewer::pose_edits(true,state,true)==E{true,false},std::string{"player pose kept, head full size: "}+S::name(state));
        require(vr_viewer::pose_edits(true,state,false)==E{},std::string{"native pose chosen: nothing driven: "}+S::name(state));
    }
    for(const auto state:{S::State::active,S::State::closing_photo,S::State::closing_pause})
        require(vr_viewer::pose_edits(true,state,true)==E{},std::string{"frozen or leaving: nothing driven: "}+S::name(state));
    // Through a real entry: the last publication before the pause (after the Start press, as
    // in the 26 Oct log) is still posed; photo mode is not.
    const auto t0=steady_clock::time_point{seconds(100)};S seq;
    require(seq.request(0,t0),"enter");
    require(vr_viewer::pose_edits(true,seq.state(),true)==E{true,false},"releasing: arms posed, head restored");
    seq.update(0,true,t0,true);require(seq.state()==S::State::opening_pause && vr_viewer::pose_edits(true,seq.state(),true).arms,"Start pressed: still posed");
    seq.update(0x908,true,t0+milliseconds(300));require(seq.state()==S::State::opening_photo && vr_viewer::pose_edits(true,seq.state(),true).arms,"pause menu: still posed");
    seq.update(0x108,true,t0+milliseconds(400));require(seq.state()==S::State::active && !vr_viewer::pose_edits(true,seq.state(),true).arms,"photo mode: frozen, nothing driven");
}

void tap_hold(){
    using namespace std::chrono;VrTapHold b;const auto t=steady_clock::time_point{seconds(5)};
    require(!b.update(true,true,t).tap,"no tap while down");
    auto r=b.update(false,true,t+milliseconds(300));require(r.tap && !r.hold,"release before a second taps");
    b.update(true,true,t+seconds(2));r=b.update(true,true,t+seconds(3));require(r.hold && !r.tap,"one second holds");
    require(!b.update(false,true,t+seconds(3)+milliseconds(10)).tap,"a hold never taps");
    // A minimum press (Left B pausing, 31 Oct): a brush sends nothing, a deliberate tap still taps.
    const auto min=milliseconds(150);
    b.update(true,true,t+seconds(10),min);require(!b.update(false,true,t+seconds(10)+milliseconds(40),min).tap,"a 40 ms brush is not a tap");
    b.update(true,true,t+seconds(11),min);require(b.update(false,true,t+seconds(11)+milliseconds(200),min).tap,"a 200 ms press taps");
    // end_pulse cuts the pulse short and leaves a held press alone.
    b.end_pulse();require(!b.update(false,true,t+seconds(11)+milliseconds(220),min).tap,"end_pulse ends the pulse");
    b.update(true,true,t+seconds(12));b.end_pulse();require(b.update(true,true,t+seconds(13)).hold,"end_pulse while held keeps the press: it still reaches a hold");
}
// Right A in the viewer (Joe, 30 Oct): the game runs under the free camera. B leaves photo mode to
// gameplay; on the player's Right A, Start then X freeze it again, as on entry.
void play_sequence(){
    using S=VrViewerSequencer;using O=S::Outcome;using namespace std::chrono;
    const auto t0=steady_clock::time_point{seconds(100)};
    const auto enter=[](S& seq,steady_clock::time_point t){
        require(seq.request(0,t),"enter");seq.update(0,true,t,true);seq.update(0x908,true,t+milliseconds(300));
        require(seq.update(0x108,true,t+milliseconds(400)).outcome==O::entered && seq.state()==S::State::active,"active");
    };
    S seq;
    require(!seq.play(t0) && !seq.refreeze(t0),"no play outside photo mode");
    enter(seq,t0);
    require(!S::play_phase(S::State::active) && S::rig_engaged(S::State::active),"photo mode: the rig, no play");
    const auto t1=t0+seconds(5);
    require(!seq.refreeze(t1),"nothing to freeze while frozen");
    require(seq.play(t1) && seq.state()==S::State::play_closing_photo && seq.plays()==1,"Right A: play");
    require(!seq.play(t1) && !seq.request(0x108,t1+milliseconds(10)),"a second press or Left B mid-transition is ignored");
    auto step=seq.update(0x108,true,t1);require(step.buttons==S::B && step.until==t1+S::PULSE,"B leaves photo mode");
    step=seq.update(0,true,t1+milliseconds(200));require(seq.state()==S::State::play_closing_photo && !step.buttons,"gameplay seen before the gap: wait");
    step=seq.update(0,true,t1+milliseconds(310));
    require(seq.state()==S::State::playing && step.outcome==O::playing && !step.buttons && seq.play_started()==t1+milliseconds(310),"gameplay: playing, the pad neutral");
    require(S::rig_engaged(seq.state()) && S::play_phase(seq.state()),"playing keeps the free camera");
    step=seq.update(0,false,t1+seconds(1));require(seq.state()==S::State::playing && !step.buttons,"focus loss does not end a play");
    require(seq.update(0,true,t1+seconds(3)).outcome==O::none && seq.state()==S::State::playing,"the game runs until the play ends or Right A freezes it");
    const auto t2=t1+seconds(4);
    require(seq.refreeze(t2) && seq.state()==S::State::play_opening_pause,"refreeze");
    step=seq.update(0,true,t2);require(step.buttons==S::START,"Start pauses");
    require(S::play_phase(seq.state()) && S::rig_engaged(seq.state()),"the free camera stays through the freeze");
    step=seq.update(0x908,true,t2+milliseconds(300));require(seq.state()==S::State::play_opening_photo && step.buttons==S::X,"X on the pause menu");
    step=seq.update(0x108,true,t2+milliseconds(400));
    require(seq.state()==S::State::active && step.outcome==O::refrozen && !step.buttons && seq.refreezes()==1,"photo mode again: frozen");
    // The pause menu on the way out: Start closes it, then play.
    const auto t3=t2+seconds(2);
    require(seq.play(t3),"play again");seq.update(0x108,true,t3);
    step=seq.update(0x908,true,t3+milliseconds(310));require(seq.state()==S::State::play_closing_pause && step.buttons==S::START,"pause menu: Start");
    step=seq.update(0,true,t3+milliseconds(620));require(seq.state()==S::State::playing && step.outcome==O::playing && seq.plays()==2,"then playing");
    // Left B while playing, or the play running its course: out of the viewer at once, the game already runs.
    require(seq.request(0,t3+seconds(1)) && seq.state()==S::State::off && !S::rig_engaged(seq.state()),"leaving a play ends the viewer");
    step=seq.update(0,true,t3+seconds(1));require(step.outcome==O::none && !step.buttons && seq.failures()==0,"nothing left to press");
    require(vr_viewer::pose_edits(true,seq.state(),true)==vr_viewer::PoseEdits{true,true},"first person's arms and head again");
    // The game opening a cutscene or a menu by itself ends the viewer.
    for(const uint32_t flags:{0x10u,0x908u,0x108u}){
        const auto t=t3+seconds(10+flags);S s;enter(s,t);s.play(t+seconds(1));s.update(0x108,true,t+seconds(1));s.update(0,true,t+seconds(2));
        require(s.state()==S::State::playing,"playing");
        step=s.update(flags,true,t+seconds(3));
        require(s.state()==S::State::off && step.outcome==O::exited && s.failures()==0,"the game leaving gameplay by itself ends the viewer: "+std::to_string(flags));
    }
    // A freeze that never reaches photo mode fails cleanly (the pause menu stays, as on entry).
    {const auto t=t3+seconds(100);S s;enter(s,t);s.play(t+seconds(1));s.update(0x108,true,t+seconds(1));s.update(0,true,t+seconds(2));
     s.refreeze(t+seconds(3));s.update(0,true,t+seconds(3));
     step=s.update(0,true,t+seconds(3)+S::PULSE+S::WAIT);require(s.state()==S::State::off && step.outcome==O::failed && s.failures()==1,"no pause menu after Start fails");
     S u;enter(u,t);u.play(t+seconds(1));u.update(0x108,true,t+seconds(1));u.update(0,true,t+seconds(2));u.refreeze(t+seconds(3));u.update(0,true,t+seconds(3));
     u.update(0x908,true,t+seconds(4));
     step=u.update(0x908,true,t+seconds(4)+S::HOLD_LIMIT+S::WAIT);require(u.state()==S::State::off && step.outcome==O::failed,"photo mode refusing fails after the hold limit");
     S v;enter(v,t);v.play(t+seconds(1));step=v.update(0x108,true,t+seconds(1)+S::PULSE+S::WAIT);require(v.state()==S::State::off && step.outcome==O::failed,"B that never reaches gameplay fails");
     S w;enter(w,t);w.play(t+seconds(1));require(w.update(0x108,false,t+seconds(1)).outcome==O::failed,"focus loss fails the transition out of photo mode");}
}

using MotionSample=vr_viewer::Motion::Sample;
MotionSample at_time(std::chrono::steady_clock::time_point t,float hand_x,float root_x=0,uintptr_t actor=1){
    MotionSample s;s.time=t;s.actor=actor;s.points={Vec{root_x,0,0},Vec{root_x,2,0},Vec{hand_x,1,.3f},Vec{-hand_x,1,-.3f}};return s;
}
void motion_and_play_end(){
    using namespace std::chrono;using vr_viewer::PlayEnd;using vr_viewer::Play;
    const auto t0=steady_clock::time_point{seconds(50)};const float units=.9f,still=.35f;
    vr_viewer::Motion m;m.reset(t0);
    m.add(at_time(t0-milliseconds(500),5),units,still);
    require(!m.still_for(t0) && m.measured()==0,"the publication held from before the freeze is ignored");
    // Idle: hands drift 1 cm/s with publications every 16 ms (each speed spans >= 50 ms).
    for(int i=0;i<=40;++i) m.add(at_time(t0+milliseconds(16*i),.009f*.016f*i),units,still);
    require(!m.moved() && m.measured()>=10 && m.speed()<.05f,"idle breathing is not motion");
    require(m.still_for(t0+milliseconds(650)) && *m.still_for(t0+milliseconds(650))==milliseconds(640),"still up to the latest publication");
    require(!m.still_for(t0+milliseconds(640)+vr_viewer::Motion::GAP+milliseconds(1)),"stale publications: not known to be still");
    // Jitter: a 1 mm step 4 ms after the reference is not measured on its own.
    {vr_viewer::Motion j;j.reset(t0);j.add(at_time(t0,0),units,still);j.add(at_time(t0+milliseconds(4),.001f),units,still);
     require(j.measured()==0 && !j.moved(),"no speed over less than the interval");}
    // A swing: the hands at 2 m/s.
    const auto t1=t0+seconds(1);
    m.add(at_time(t1,0),units,still);m.add(at_time(t1+milliseconds(60),2*.06f*units),units,still);
    require(m.moved() && m.speed()>1.9f && *m.still_for(t1+milliseconds(60))==milliseconds(0),"a swing moves");
    m.add(at_time(t1+milliseconds(120),2*.06f*units),units,still);m.add(at_time(t1+milliseconds(400),2*.06f*units),units,still);
    require(*m.still_for(t1+milliseconds(400))==milliseconds(340),"then still since the swing's last fast interval");
    // A gap in the publications or another actor restarts the still time.
    m.add(at_time(t1+milliseconds(800),2*.06f*units),units,still);require(*m.still_for(t1+milliseconds(800))==milliseconds(0),"a gap restarts the still time");
    m.add(at_time(t1+milliseconds(820),0,0,2),units,still);require(*m.still_for(t1+milliseconds(820))==milliseconds(0),"another actor restarts it");
    m.reset(t1+seconds(1));require(!m.moved() && !m.still_for(t1+seconds(1)),"reset");

    // When a play ends, by mode.
    Play play;require(play.valid() && play.mode()==Play::Mode::animation,"defaults: animation");
    const auto s=[](double v){return duration_cast<steady_clock::duration>(duration<double>(v));};
    require(vr_viewer::play_end(play,s(2),false,true,s(.4))==PlayEnd::none,"animation: still for less than still_seconds");
    require(vr_viewer::play_end(play,s(2),false,true,s(.5))==PlayEnd::settled,"animation: moved, then still for still_seconds");
    require(vr_viewer::play_end(play,s(2),false,true,std::nullopt)==PlayEnd::none,"animation: no fresh publications, keep playing");
    require(vr_viewer::play_end(play,s(2.9),false,false,s(2.9))==PlayEnd::none && vr_viewer::play_end(play,s(3),false,false,s(3))==PlayEnd::clip,"animation: never moved, a clip");
    require(vr_viewer::play_end(play,s(10),false,true,s(0))==PlayEnd::limit,"animation: capped");
    play.values.values[Play::MODE]=2;
    require(vr_viewer::play_end(play,s(2),false,true,s(1))==PlayEnd::none && vr_viewer::play_end(play,s(3),false,true,s(0))==PlayEnd::clip,"fixed: the clip");
    play.values.values[Play::MODE]=3;
    require(vr_viewer::play_end(play,s(5),true,true,s(1))==PlayEnd::none && vr_viewer::play_end(play,s(.1),false,false,{})==PlayEnd::released,"hold: while held");
    require(vr_viewer::play_end(play,s(10),true,false,{})==PlayEnd::limit,"hold: capped");
    // Joe, 31 Oct: the player's own Right A freezes; a play that runs its course ends in the game.
    for(const auto end:{PlayEnd::cancelled,PlayEnd::released}) require(vr_viewer::freezes(end),"Right A freezes");
    for(const auto end:{PlayEnd::none,PlayEnd::settled,PlayEnd::clip,PlayEnd::limit,PlayEnd::left}) require(!vr_viewer::freezes(end),"the rest end in the game");
    play.values.values[Play::MODE]=1.5f;require(!play.valid() && play.mode()==Play::Mode::off,"a fractional mode is invalid: off");
    play=Play{};play.values.values[Play::CLIP_SECONDS]=20;require(!play.valid(),"a clip longer than the limit is invalid");
}

// The stillness rule on an execution-shaped motion: a lunge, strikes with short pauses in which the
// hands still move faster than 0.35 m/s, then the idle stance (30 Oct playtest trace, finisher at
// 448.10 s: the Marine stopped about 5.0 s in, the rule held at 5.56 s).
void execution_settles(){
    using namespace std::chrono;
    const auto t0=steady_clock::time_point{seconds(200)};const float units=.9f;
    const vr_viewer::Play play;vr_viewer::Motion m;m.reset(t0);
    float root=0,hand=0;std::optional<vr_viewer::PlayEnd> ended;double ended_at{};
    for(int i=1;i<=60*9;++i){
        const double t=i/60.;
        // m/s: root lunge for 0.6 s; hands sweep at 1-12 m/s with dips to 0.5 m/s until 5.0 s; then 0.02 m/s.
        const float root_speed=t<.6?10.f:0.f;
        const float hand_speed=t<5.?(std::fmod(t,.8)<.15?.5f:std::fmod(t,.8)<.4?12.f:1.2f):.02f;
        root+=root_speed*units/60;hand+=hand_speed*units/60;
        const auto now=t0+duration_cast<steady_clock::duration>(duration<double>(t));
        m.add(at_time(now,hand,root),units,play.still_speed());
        const auto end=vr_viewer::play_end(play,now-t0,false,m.moved(),m.still_for(now));
        if(end!=vr_viewer::PlayEnd::none && !ended){ended=end;ended_at=t;}
    }
    require(ended==vr_viewer::PlayEnd::settled,"the execution ends by settling");
    require(ended_at>=5.5 && ended_at<=5.6,"half a second after the Marine stops (at 5.0 s), never mid-execution: "+std::to_string(ended_at));
}

void play_settings_file(const char* path){
    const auto play=vr_viewer::Play::load(path);
    require(play && *play==vr_viewer::Play{},"config/index_viewer_play.calibration holds the defaults");
}
} // namespace

int main(int argc,char** argv){
    try{
        entry_matches_first_person();slide_and_vertical();snap_turn_about_head();one_hand_drag();two_hand_scale_and_turn();grounded_entry_and_presets();grounded_moves();sequencer();pose_kept_until_the_freeze();tap_hold();
        play_sequence();motion_and_play_end();execution_settles();
        if(argc>1) play_settings_file(argv[1]);
    }catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
    std::cout<<"vr_viewer tests passed\n";return 0;
}
