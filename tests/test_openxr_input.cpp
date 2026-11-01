#include "OpenXrHands.h"
#include "OpenXrInput.h"
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
openxr_input::Values neutral() {
    openxr_input::Values v{};for(auto& x:v) x={true,0,0};return v;
}
void sample_rules() {
    using namespace openxr_input;
    auto v=neutral();
    auto s=sample(v);require(s && s->complete && !s->sample.fire_touch && s->sample.fire_touch_valid,"neutral complete bindings");
    v[move]={true,.3f,.4f};v[turn]={true,-.9f,.5f};v[confirm]={true,1};v[menu]={true,1};v[aim]={true,.25f};v[parry]={true,1.f};
    v[utility]={true,-.5f,.75f};v[utility_press]={true,.6f};v[grenade]={true,.9f};
    s=sample(v);
    require(s && s->sample.move_x==.3f && s->sample.move_y==.4f && s->sample.turn_x==-.9f && s->sample.turn_y==.5f && s->sample.confirm && s->sample.menu &&
        s->sample.aim==.25f && s->sample.parry==1.f && s->sample.utility_x==-.5f && s->sample.utility_y==.75f && s->sample.utility_press==.6f && s->sample.grenade==.9f,
        "values map to the OpenVR sample fields");
    for(const auto a:{move,turn}) { auto w=v;w[a].active=false;require(!sample(w),"both sticks are mandatory"); }
    { auto w=v;w[move].x=std::nanf("");require(!sample(w),"non-finite stick is rejected"); }
    { auto w=v;w[confirm].active=false;w[confirm].x=1;auto r=sample(w);require(r && !r->sample.confirm && r->complete,"inactive buttons read released"); }
    { auto w=v;w[fire]={true,1.5f};auto r=sample(w);require(r && r->sample.fire==0 && !r->complete,"out-of-range analog releases itself"); }
    { auto w=v;w[melee].active=false;w[melee].x=1;auto r=sample(w);require(r && r->sample.melee==0 && !r->complete && r->sample.parry==1,"inactive analog releases only itself"); }
    { auto w=v;w[utility]={true,2,0};auto r=sample(w);require(r && r->sample.utility_x==0 && !r->complete,"out-of-range trackpad releases itself"); }
    { auto w=v;w[finisher].active=false;auto r=sample(w);require(r && !r->sample.finisher && !r->complete,"missing finisher marks bindings incomplete"); }
    { auto w=v;w[fire_touch].active=false;auto r=sample(w);require(r && !r->sample.fire_touch_valid && r->complete,"missing trigger touch falls back to pull, not incomplete"); }
    require(!sample(v)->sample.touch_layout && sample(v,true)->sample.touch_layout,"the Touch layout is the caller's, per profile");
}
void profiles() {
    using namespace openxr_input;
    std::set<std::string> names;
    for(const auto& a:ACTIONS) require(names.insert(a.name).second,"action names are unique");
    for(const auto& p:PROFILES) {
        std::set<size_t> actions;
        for(const auto& b:p.bindings) {
            const std::string path=b.path;
            require(path.starts_with("/user/hand/left/input/") || path.starts_with("/user/hand/right/input/"),"bindings are hand input paths");
            require(actions.insert(b.action).second,"each action is bound once per profile");
        }
        for(const auto a:{move,turn,confirm,interact,menu,recenter,fire,aim,melee,parry}) require(actions.contains(a),"every profile binds locomotion, buttons, triggers and grips");
        require(profile(p.path)==&p,"profiles are found by path");
    }
    require(PROFILES[0].bindings.size()==COUNT,"Index binds every action");
    // docs/QUEST_STEAM_LINK.md: Touch has no trackpad, so its samples take the Touch routes.
    require(!profile("/interaction_profiles/valve/index_controller")->touch_layout && profile("/interaction_profiles/oculus/touch_controller")->touch_layout,
        "only Touch uses the Touch layout");
    for(const auto& b:profile("/interaction_profiles/oculus/touch_controller")->bindings) require(b.action!=utility && b.action!=utility_press && b.action!=grenade,"Touch binds no trackpad action");
    require(!profile("/interaction_profiles/htc/vive_controller"),"unsupported profiles are reported as such");
}
void grip_offset() {
    const auto offset=openxr_hands::GripOffset::index_render_model();
    require(offset.valid() && !offset.measured,"the render-model candidate is valid but unmeasured");
    for(size_t hand=0;hand<2;++hand) {
        auto grip=openxr_hands::GripOffset::rotation({10,20,30});grip[12]=.1f;grip[13]=1.2f;grip[14]=-.4f;
        const auto raw=offset.raw(hand,grip);require(raw && vr_camera::rigid(*raw),"raw origin is rigid");
        const auto back=vr_camera::multiply(offset.grip_in_raw(hand),*raw);
        for(int n=0;n<16;++n) require(std::abs(back[n]-grip[n])<1e-5f,"grip_in_raw * raw restores the grip pose");
    }
    auto identity=vr_camera::IDENTITY;const auto raw=offset.raw(0,identity);
    const auto expected=vr_camera::inverse(offset.grip_in_raw(0));
    for(int n=0;n<16;++n) require(std::abs((*raw)[n]-expected[n])<1e-6f,"identity grip yields the inverse component frame");
    std::istringstream good{"# measured\nleft_grip_in_raw_metres=0 -0.015 0.13\nleft_grip_in_raw_degrees=15 -2 0.3\n"
        "right_grip_in_raw_metres=0 -0.015 0.13\nright_grip_in_raw_degrees=15 2 -0.3\nmeasured=1\n"};
    const auto read=openxr_hands::GripOffset::read(good);require(read && read->measured && read->degrees[1][1]==2,"calibration file parses");
    for(const char* bad:{"left_grip_in_raw_metres=0 0\n","measured=2\n","bogus=1\n","left_grip_in_raw_metres=0 0 0\nleft_grip_in_raw_metres=0 0 0\n"}) {
        std::istringstream in{bad};require(!openxr_hands::GripOffset::read(in),"malformed, unknown or duplicate keys reject the file");
    }
    std::istringstream distant{"left_grip_in_raw_metres=0 0 2\nleft_grip_in_raw_degrees=0 0 0\nright_grip_in_raw_metres=0 0 0\nright_grip_in_raw_degrees=0 0 0\nmeasured=0\n"};
    require(!openxr_hands::GripOffset::read(distant),"offsets beyond 0.5 m are rejected");
}
std::array<XrHandJointLocationEXT,XR_HAND_JOINT_COUNT_EXT> hand(float finger_step,float thumb_step,float yaw=0) {
    std::array<XrHandJointLocationEXT,XR_HAND_JOINT_COUNT_EXT> joints{};
    // A whole-hand yaw must not change curls: flexion is measured between joints.
    const XrQuaternionf turn{0,std::sin(yaw/2),0,std::cos(yaw/2)};
    const auto mul=[](XrQuaternionf a,XrQuaternionf b){return XrQuaternionf{a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};};
    for(uint32_t n=0;n<joints.size();++n) {
        float angle{};
        if(n>=XR_HAND_JOINT_THUMB_METACARPAL_EXT && n<=XR_HAND_JOINT_THUMB_TIP_EXT) angle=-thumb_step*float(std::min(n-uint32_t(XR_HAND_JOINT_THUMB_METACARPAL_EXT),2u));
        else if(n>=XR_HAND_JOINT_INDEX_METACARPAL_EXT) angle=-finger_step*float(std::min((n-uint32_t(XR_HAND_JOINT_INDEX_METACARPAL_EXT))%5,3u));
        joints[n].locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        joints[n].pose.orientation=mul(turn,{std::sin(angle/2),0,0,std::cos(angle/2)});
    }
    return joints;
}
void curls() {
    const auto full=openxr_hands::FULL_CURL_RADIANS;
    auto c=openxr_hands::curls(hand(0,0));require(c && std::ranges::all_of(*c,[](float v){return v<1e-5f;}),"open hand reads zero");
    c=openxr_hands::curls(hand(full[1]/3,full[0]/2,1.2f));require(c && std::ranges::all_of(*c,[](float v){return v>.999f;}),"fist reads one regardless of hand yaw");
    c=openxr_hands::curls(hand(full[1]/6,full[0]/4));require(c && std::ranges::all_of(*c,[](float v){return std::abs(v-.5f)<1e-3f;}),"half flexion reads one half");
    c=openxr_hands::curls(hand(-.3f,-.3f));require(c && std::ranges::all_of(*c,[](float v){return v==0;}),"hyperextension clamps to open");
    auto bad=hand(0,0);bad[7].locationFlags=0;require(!openxr_hands::curls(bad),"invalid joint orientation rejects the hand");
    bad=hand(0,0);bad[7].pose.orientation={0,0,0,2};require(!openxr_hands::curls(bad),"non-unit quaternion rejects the hand");
    require(!openxr_hands::curls(std::span<const XrHandJointLocationEXT>{bad.data(),10}),"partial joint sets are rejected");
}
}
// The launcher's shipped calibration must parse and, until measured, equal the candidate.
void shipped_calibration(const char* path) {
    std::ifstream input{path};const auto offset=openxr_hands::GripOffset::read(input);
    require(offset.has_value(),"config/openxr_index_grip.calibration must parse");
    if(!offset->measured) require(*offset==openxr_hands::GripOffset::index_render_model(),"an unmeasured file must hold the render-model candidate");
}
int main(int argc,char** argv) {
    try { require(argc==2,"usage: openxr_input_tests <openxr_index_grip.calibration>");
        sample_rules(); profiles(); grip_offset(); curls(); shipped_calibration(argv[1]); std::cout<<"OpenXR input mapping, profiles, grip offset and curls passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
