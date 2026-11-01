#include "VrTrace.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
using namespace std::chrono_literals;
using vr_trace::Clock;
const auto T0=Clock::time_point{50s};
bool close_to(float a,float b,float tolerance=1e-3f) { return std::abs(a-b)<=tolerance; }
bool contains(const std::string& text,const char* part) { return text.find(part)!=std::string::npos; }
bool balanced(const std::string& text) {
    int depth=0;bool quoted=false;
    for(size_t i=0;i<text.size();++i) { const char c=text[i];
        if(quoted) { if(c=='\\') ++i;else if(c=='"') quoted=false;continue; }
        if(c=='"') quoted=true;else if(c=='{' || c=='[') ++depth;else if(c=='}' || c==']') { if(--depth<0) return false; } }
    return depth==0 && !quoted && text.front()=='{' && text.back()=='}' && !contains(text,",,") && !contains(text,"{,") && !contains(text,",}");
}
// Character root yawed 90 degrees: its row 2 (z axis) points along world +X.
vr_trace::Character character(float units) {
    vr_trace::Character c;c.time=T0;c.actor=0xabc;c.units_per_metre=units;
    c.root={0,0,-1,0, 0,1,0,0, 1,0,0,0, 10,0,20,1};
    c.head=vr_arm::Vector{10,1.8f*units,20};
    c.native_hands[1]=vr_arm::Vector{10+0.3f*units,1.2f*units,20};
    c.targets[1]=vr_arm::Vector{10+0.4f*units,1.2f*units,20};
    c.solved_hands[1]=vr_arm::Vector{10+0.38f*units,1.2f*units,20};
    return c;
}

void metrics() {
    for(float units:{1.0f,10.0f}) {
        auto c=character(units);vr_trace::Camera cam;cam.time=T0;
        // Eyes 0.064 m apart (along world Z), centred 0.1 m in front of the head bone along the root's z axis (world +X) and 0.05 m up.
        cam.eyes[0]=vr_arm::Vector{10+0.1f*units,1.85f*units,20-0.032f*units};cam.eye_times[0]=T0;
        cam.eyes[1]=vr_arm::Vector{10+0.1f*units,1.85f*units,20+0.032f*units};cam.eye_times[1]=T0;
        const auto m=vr_trace::metrics(c,cam,T0+5ms);
        require(m.head_offset && close_to((*m.head_offset)[2],0.1f) && close_to((*m.head_offset)[1],0.05f) && close_to((*m.head_offset)[0],0),"head offset is in metres in the character root's axes");
        require(m.hand_target_offset[1] && close_to((*m.hand_target_offset[1])[2],0.1f),"tracked target versus the animated hand");
        require(m.hand_residual[1] && close_to(*m.hand_residual[1],0.02f),"IK residual is target to solved hand in metres");
        require(!m.hand_target_offset[0] && !m.hand_residual[0],"untracked left hand has no metrics");
        cam.eye_times[1]=T0-500ms;
        const auto stale=vr_trace::metrics(c,cam,T0+5ms);
        require(stale.head_offset && close_to((*stale.head_offset)[0],0.032f),"a stale eye is ignored; the fresh one is used");
        cam.eye_times[0]=T0-500ms;
        require(!vr_trace::metrics(c,cam,T0+5ms).head_offset,"no fresh eye, no head offset");
    }
}

vr_trace::Input input(double ms) { vr_trace::Input i;i.time=T0+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double,std::milli>{ms});i.epoch=3;i.armed=true;return i; }
void events() {
    vr_trace::Events e;auto a=input(0);
    require(e.input(a,0).empty(),"the first record only primes the detector");
    auto b=input(11);b.pad.wButtons=XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_LEFT_SHOULDER;b.pad.bRightTrigger=200;
    b.guard.guards=1;b.guard.pose=true;b.guard.hand={0.3f,-0.1f,-0.2f};b.guard.angle_degrees=80;
    const auto first=e.input(b,0.011);
    require(first.size()==4,"dodge, parry, fire and guard start are separate events");
    require(contains(first[0],"\"A_dodge_confirm\"") && contains(first[0],"\"down\":true"),"dodge edge is named");
    require(contains(first[1],"\"LB_parry_block\""),"parry edge is named");
    require(contains(first[2],"\"RT_fire\""),"trigger crossing half travel is an edge");
    require(contains(first[3],"guard_start") && contains(first[3],"\"angle_degrees\":80.0"),"guard start carries its pose");
    for(const auto& line:first) require(balanced(line),"event lines are well-formed JSON objects");
    auto c=b;c.time+=200ms;c.pad.wButtons=0;c.guard.pose=false;c.guard.last_duration_ms=210;
    c.grenade.grabs=1;c.grenade.throws=1;c.grenade.last=VrGrenadeThrow::Outcome::thrown;c.grenade.last_release_speed=3.2f;c.grenade.last_forward_fraction=0.8f;
    c.holster.misses=1;c.snaps=1;c.epoch=4;c.swing.swings=1;
    const auto second=e.input(c,0.211);
    const auto has=[&](const char* part){return std::ranges::any_of(second,[&](const auto& l){return contains(l,part);});};
    require(has("\"guard_end\"") && has("\"duration_ms\":210.0"),"guard end carries its duration");
    require(has("grenade_grab") && has("\"outcome\":\"thrown\"") && has("\"speed\":3.200"),"grenade grab and throw outcome");
    require(has("holster_miss") && has("swing") && has("snap_turn"),"holster miss, swing and snap events");
    require(!has("tracking_reset"),"an epoch change caused by a snap is not reported as a tracking reset");
    auto d=c;d.epoch=5;const auto reset=e.input(d,0.3);
    require(reset.size()==1 && contains(reset[0],"tracking_reset"),"recenter reports a tracking reset");
    // Melee arbitration (playtest 20 Oct): the swing says light or power, a left-led
    // motion is a veto with the other hand's speed, and a guard released for an attack is named.
    auto m=d;m.swing.swings=2;m.swing.power=true;m.swing.other_hand_speed=0.4f;
    const auto power=e.input(m,0.4);
    require(power.size()==1 && contains(power[0],"\"swing\"") && contains(power[0],"\"power\":true") && contains(power[0],"\"other_hand_speed\":0.400"),"swing event names the attack kind");
    auto v=m;v.swing.vetoes=1;v.swing.other_hand_speed=5.8f;v.guard.yields=1;v.swing.flicks=1;
    const auto veto=e.input(v,0.5);
    require(veto.size()==3 && contains(veto[0],"swing_vetoed") && contains(veto[0],"\"other_hand_speed\":5.800") && contains(veto[1],"guard_yield") && contains(veto[2],"swing_flick"),"veto, guard yield and flick are events");
    for(const auto& line:veto) require(balanced(line),"melee events are well-formed JSON objects");
    // Left-arm push (21 Oct evening): one event per push with where the hand ended and the thrust.
    auto lp=v;lp.left_push.pushes=1;lp.left_push.forward=.6f;lp.left_push.travel=.5f;lp.left_push.peak=3.5f;
    const auto pushed=e.input(lp,0.6);
    require(pushed.size()==1 && contains(pushed[0],"\"left_push\"") && contains(pushed[0],"\"travel\":0.500") && contains(pushed[0],"\"peak\":3.500") && balanced(pushed[0]),"a left push is an event");
    vr_trace::Character ch;ch.actor=0x10;
    require(e.character(ch,1).size()==1 && e.character(ch,1.1).empty(),"actor changes once");
    vr_trace::Camera cam;cam.camera_mode=1;cam.anchored=true;
    require(e.camera(cam,1).size()==1 && e.camera(cam,1.1).empty(),"camera mode reported on change only");
    cam.camera_mode=3;const auto scripted=e.camera(cam,1.2);
    require(scripted.size()==1 && contains(scripted[0],"\"scripted\""),"cutscene camera handover is an event");
    // Riding along with the jump pack (VrBodyFollowing ride_along): a start and an end event, once each.
    cam.camera_mode=1;e.camera(cam,1.3);cam.riding=true;cam.travel_speed=16.2f;const auto started=e.camera(cam,1.4);
    require(started.size()==1 && contains(started[0],"\"ride_start\"") && contains(started[0],"\"travel_speed_metres_per_second\":16.2") && balanced(started[0]) && e.camera(cam,1.5).empty(),"a ride's start is an event, once");
    cam.riding=false;cam.travel_speed=0;const auto ended=e.camera(cam,1.9);
    require(ended.size()==1 && contains(ended[0],"\"ride_end\""),"and its end");
    // A takeover's early return (docs/TAKEOVER_RETURN_2026-10-26.md): its own start and end events.
    cam.released=true;const auto released=e.camera(cam,2.0);
    require(released.size()==1 && contains(released[0],"\"takeover_release\"") && balanced(released[0]) && e.camera(cam,2.1).empty(),"an early return is an event, once");
    cam.released=false;const auto settled=e.camera(cam,2.4);
    require(settled.size()==1 && contains(settled[0],"\"release_end\""),"and its end");
    // A melee combo past the envelope (docs/ANIMATION_CAMERA.md): the same.
    cam.combo_camera=true;const auto combo=e.camera(cam,2.5);
    require(combo.size()==1 && contains(combo[0],"\"combo_camera_start\"") && balanced(combo[0]) && e.camera(cam,2.6).empty(),"a combo past the envelope is an event, once");
    cam.combo_camera=false;const auto combo_end=e.camera(cam,2.9);
    require(combo_end.size()==1 && contains(combo_end[0],"\"combo_camera_end\""),"and its end");
}
void sample_line() {
    auto i=input(0);i.head=vr_camera::IDENTITY;i.head->at(13)=1.7f;i.hands[1]=vr_camera::IDENTITY;
    VrControllerSample s;s.fire=1;s.utility_x=.8f;s.utility_y=-.2f;s.utility_press=.7f;i.sample=s;i.pad.bRightTrigger=255;i.guard_enabled=true;
    VrControllerSample raw;raw.aim=1;i.raw_sample=raw;i.supporting=true;
    i.grenade.recovering=true;
    const auto cam=[]{vr_trace::Camera c;c.time=T0;c.camera_mode=1;c.anchored=true;c.eyes[0]=vr_arm::Vector{10.1f,1.85f,20};c.eye_times[0]=T0;c.eye_forward=vr_arm::Vector{0,0,-1};return c;}();
    i.fingers[0]=vr_finger::Curl{0,.05f,.1f,.6f,.5f};
    const auto line=vr_trace::sample(1.5,i,character(1),cam,T0+10ms);
    require(balanced(line),"sample line is a well-formed JSON object");
    require(contains(line,"\"left_push\":{\"enabled\":false,\"ready\":false") && contains(line,"\"left_curl\":[0.000,0.050,0.100,0.600,0.500],\"right_curl\":null"),
        "trace carries the push state and the finger curls (a push needs an open hand)");
    require(contains(line,"\"left_trackpad\":[0.800,-0.200,0.700]"),"trace shows where the left trackpad was pressed and how hard: a stim that missed its quadrant sends no button");
    require(contains(line,"\"recovering\":true"),"trace exposes grenade follow-through ownership without claiming native inventory");
    require(contains(line,"\"raw_controls\":{\"right_trigger\":0.000,\"left_trigger\":1.000") && contains(line,"\"support_aim\":true"),"trace distinguishes physical trigger hand from routed native fire and reports support aiming");
    for(const char* key:{"\"type\":\"sample\"","\"head\":[0.0000,1.7000,0.0000]","\"left\":null","\"rt\":255","\"guard\":{\"enabled\":true",
        "\"actor\":\"abc\"","\"mode\":\"body\"","\"melee_facing\":false","\"ride\":false","\"vignette\":0.000","\"released\":false","\"combo_camera\":false","\"travel_speed_metres_per_second\":0.0","\"eye_forward\":[0.0000,0.0000,-1.0000]","\"head_offset_metres\":[","\"right_residual_metres\":0.0200"}) require(contains(line,key),key);
    // The interaction camera: the title camera's basis (right +X, forward -Z), gaze 90 degrees
    // to its right and slightly down. The trace names the native forward and the misalignment.
    auto turned=cam;turned.game={1,0,0,0, 0,1,0,0, 0,0,-1,0, 3,2,1,1};turned.eye_forward=vr_arm::Vector{.9848f,-.1736f,0};
    const auto aligned=vr_trace::sample(1.55,i,character(1),turned,T0+10ms);
    require(balanced(aligned) && contains(aligned,"\"game_forward\":[0.0000,0.0000,-1.0000]") &&
        contains(aligned,"\"gaze_native\":{\"yaw_degrees\":90.0,\"pitch_degrees\":-10.0}"),"trace reports where the native interaction camera points relative to the gaze");
    require(contains(aligned,"\"rx\":0,\"ry\":0"),"trace records the native right stick (gaze camera steering)");
    turned.eye_forward.reset();
    require(contains(vr_trace::sample(1.56,i,character(1),turned,T0+10ms),"\"gaze_native\":null"),"no composed eye, no alignment claim");
    const auto empty=vr_trace::sample(2,std::nullopt,std::nullopt,std::nullopt,T0);
    require(balanced(empty) && contains(empty,"\"input\":null") && contains(empty,"\"head_offset_metres\":null"),"missing producers are null, not omitted");
    require(contains(line,"\"loadout\":null,\"triggers_rearmed\":false,\"rev_rearmed\":false,\"rev_holdover\":false") && contains(line,"\"chainsword_rev\":null"),
        "unconfirmed loadout and an unrecorded rev are null");
    i.loadout_secondary=true;i.rev_rearmed=true;i.rev_holdover=true;
    const vr_trace::ChainswordRev rev{T0,true,true,3,2,1,1.05f,.58f};
    const auto revving=vr_trace::sample(1.6,i,std::nullopt,std::nullopt,T0+10ms,{},{},rev);
    require(balanced(revving) && contains(revving,"\"loadout\":\"secondary\",\"triggers_rearmed\":false,\"rev_rearmed\":true,\"rev_holdover\":true"),"trace names the routed loadout and rev arming");
    require(contains(revving,"\"chainsword_rev\":{\"age_ms\":10.0,\"installed\":true,\"active\":true,\"starts\":3,\"stops\":2,\"refused\":1,\"play_rate\":1.050,\"gain\":0.580}"),"trace carries native rev state");
    i.loadout_secondary=false;require(contains(vr_trace::sample(1.7,i,std::nullopt,std::nullopt,T0),"\"loadout\":\"primary\""),"primary loadout is named");
    vr_trace::Character nan=character(1);nan.head=vr_arm::Vector{std::nanf(""),0,0};
    require(balanced(vr_trace::sample(3,i,nan,cam,T0)),"non-finite values become null");
}
void weapons() {
    auto invalidated=character(1);invalidated.arm_pose_invalidation=vr_trace::ArmPoseInvalidation{12,1,true,true,0,1,"invalidated"};
    const auto cache_line=vr_trace::sample(1,std::nullopt,invalidated,std::nullopt,T0);
    require(balanced(cache_line)&&contains(cache_line,"\"arm_pose_invalidation\":{\"calls\":12,\"refused\":1,\"called\":true,\"valid\":true,\"status\":\"invalidated\",\"dirty_before\":0,\"dirty_after\":1,"),"trace distinguishes post-write native invalidation from pose/world evidence");
    auto& resolution=*invalidated.arm_pose_invalidation;resolution.resolution_enabled=true;resolution.flush_called=true;resolution.resolved=true;resolution.verified_hands=2;resolution.world_error=.000123f;resolution.recovery_calls=1;resolution.recovery_failures=1;resolution.recovery_failed=true;
    const auto resolved_line=vr_trace::sample(1,std::nullopt,invalidated,std::nullopt,T0);
    require(balanced(resolved_line)&&contains(resolved_line,"\"resolution_enabled\":true,\"flush_called\":true,\"resolved\":true,\"verified_hands\":2,\"world_error\":0.000123")&&contains(resolved_line,"\"recovery_calls\":1,\"recovery_failures\":1,\"recovery_failed\":true"),"trace reports actual resolution, mapped verification and poisoned recovery separately");
    invalidated.arm_pose_invalidation->before.reset();invalidated.arm_pose_invalidation->after.reset();invalidated.arm_pose_invalidation->valid=false;
    require(contains(vr_trace::sample(1,std::nullopt,invalidated,std::nullopt,T0),"\"dirty_before\":null,\"dirty_after\":null"),"unavailable invalidation dirty reads remain null");
    auto rejected=character(1);rejected.body_placement=vr_trace::BodyPlacement{.2f,10,1,0,"body_placement","output"};
    const auto receipt=vr_trace::sample(1,std::nullopt,rejected,std::nullopt,T0);
    require(balanced(receipt) && contains(receipt,"\"body_placement\":{\"back_metres\":0.2000,\"updates\":10,\"rejected\":1,\"suspended\":0,\"stage\":\"body_placement\",\"refusal\":\"output\",\"applied_back_metres\":0.0000"),"trace reports bounded per-publication placement refusal and actual commit counters");
    const std::vector<std::string> endpoints{"ROOT","ARM1L","ARM2L","forearm_L","ARM1R","ARM2R","forearm_R"};
    require((vr_trace::body_bones(endpoints)==std::vector<size_t>{1,4,2,5,3,6}),"live body trace includes both native/published elbow and armour endpoints for reach validation");
    const std::vector<std::string> placement{"foot_targ_R","FOOT1R","cam_root","Trajectory","CENTRE","FOOT1L","foot_targ_L"};
    require((vr_trace::body_bones(placement)==std::vector<size_t>{4,3,2,5,1,6,0}),"body trace exposes shifted pelvis/feet separately from native camera/contact targets");
    const std::vector<std::string> alternate{"HANDR_glove","ROOT","HANDR","ARM2R_glove","HANDL","shoulder_skinned_R","shoulder_skinned_L"};
    require((vr_trace::body_bones(alternate)==std::vector<size_t>{4,2,3,0,6,5}),"trace selects existing main/alternate wrist and shoulder skin nodes without inventing missing bones");
    const std::vector<std::string> fingers{"R3_fing3_glove","R3_fing1","HANDR","R3_fing1_glove","R3_fing3","unrelated"};
    require((vr_trace::body_bones(fingers)==std::vector<size_t>{2,1,4,3,0}),"bounded finger base/tip probes select actual normal/glove nodes in stable trace order");
    const std::vector<std::string> partial_fingers{"HANDR","R3_fing1","R3_fing2"};
    require((vr_trace::body_bones(partial_fingers)==std::vector<size_t>{0,1}),"absent glove/tip nodes are not fabricated from finger-like names");
    auto finger_character=character(1);
    for(size_t index:vr_trace::body_bones(fingers)) {
        vr_trace::WeaponBone joint;joint.name=fingers[index];joint.skin_index=static_cast<int16_t>(250+index);
        joint.native=vr_camera::IDENTITY;(*joint.native)[12]=float(index);(*joint.native)[13]=2;(*joint.native)[14]=3;
        joint.final=joint.native;(*joint.final)[13]=5;finger_character.body.push_back(joint);
    }
    const auto finger_line=vr_trace::sample(1,std::nullopt,finger_character,std::nullopt,T0);
    require(balanced(finger_line)&&contains(finger_line,"\"name\":\"R3_fing1\",\"native\":[1.000,2.000,3.000],\"final\":[1.000,5.000,3.000],\"skin_index\":251")&&
        contains(finger_line,"\"name\":\"R3_fing3_glove\",\"native\":[0.000,2.000,3.000],\"final\":[0.000,5.000,3.000],\"skin_index\":250"),"finger trace serializes actual native/published positions and mapped slots for eye projection");
    auto identified=character(1);identified.publication_instance=0x123;identified.publication_buffer=0x456;identified.publication_rig=0x789;identified.publication_mapping=0xabc;
    const auto identity=vr_trace::sample(1,std::nullopt,identified,std::nullopt,T0);
    require(balanced(identity) && contains(identity,"\"publication_instance\":\"123\",\"publication_buffer\":\"456\",\"publication_rig\":\"789\",\"publication_mapping\":\"abc\""),"publication source identities are explicitly serialized alongside joint evidence");
    vr_trace::WeaponBone glove;glove.name="HANDR_glove";glove.skin_index=264;identified.body.push_back(glove);
    const auto unavailable=vr_trace::sample(1,std::nullopt,identified,std::nullopt,T0);
    require(contains(unavailable,"\"name\":\"HANDR_glove\",\"native\":null,\"final\":null,\"skin_index\":264"),"body trace reports actual mapped skin slots without fabricating missing positions");
    const std::vector<std::string> names{"ROOT","HANDR","GUN_R","Gun_pose","GUN_ANIM","first_weapon_offset|pivot","sword_R","HEAD","LEG1R"};
    const auto picked=vr_trace::weapon_bones(names);
    require((picked==std::vector<size_t>{2,3,4,5,6}),"weapon-like bones are picked case-insensitively; body bones are not");
    std::vector<std::string> many(40,"GUN_x");require(vr_trace::weapon_bones(many).size()==vr_trace::MAX_WEAPON_BONES,"weapon bones are capped");
    auto c=character(10);c.animset=7;
    vr_trace::WeaponBone pistol;pistol.name="GUN_R";pistol.parent="HANDR";
    pistol.native=vr_camera::IDENTITY;(*pistol.native)[12]=10+0.38f*10;(*pistol.native)[13]=1.2f*10;(*pistol.native)[14]=20;
    vr_trace::WeaponBone floating;floating.name="Gun_pose";floating.parent="GUN_ANIM";
    floating.native=vr_camera::IDENTITY;(*floating.native)[12]=10-0.5f*10;(*floating.native)[13]=1.2f*10;(*floating.native)[14]=20;
    floating.final=floating.native;(*floating.final)[12]=10+0.38f*10;
    c.weapons={pistol,floating};
    require(close_to(*vr_trace::weapon_to_hand(c,pistol,1),0),"a weapon on the solved right hand is 0 m from it");
    vr_trace::WeaponBone unmoved=floating;unmoved.final.reset();
    require(close_to(*vr_trace::weapon_to_hand(c,unmoved,1),0.88f),"an unmoved weapon is measured from where the animation left it");
    vr_trace::WeaponRay ray;ray.time=T0;ray.owned=true;ray.origin={1,2,3};ray.forward={0,0,1};ray.target={1,2,203};ray.max_distance=200;
    const auto line=vr_trace::sample(1,std::nullopt,c,std::nullopt,T0,ray);
    require(balanced(line),"sample with weapons is well-formed");
    for(const char* part:{"\"animset\":7","\"weapons\":[{\"name\":\"GUN_R\"","},{\"name\":\"Gun_pose\"","\"final\":null","\"to_right_hand_metres\":0.000",
        "\"native_z_axis\":[0.000,0.000,1.000]","\"weapon_ray\":{","\"forward\":[0.0000,0.0000,1.0000]","\"aim_target\":null"}) require(contains(line,part),part);
    auto empty=character(1);require(contains(vr_trace::sample(1,std::nullopt,empty,std::nullopt,T0),"\"weapons\":[]"),"no weapon bones is an empty list");
    vr_trace::Events e;
    const auto first=e.character(c,1);
    require(std::ranges::any_of(first,[](const auto& l){return contains(l,"\"event\":\"animset\"");}),"animset change is an event");
    const auto bones=std::ranges::find_if(first,[](const auto& l){return contains(l,"weapon_bones");});
    require(bones!=first.end() && contains(*bones,"{\"name\":\"GUN_R\",\"parent\":\"HANDR\"}") && balanced(*bones),"weapon bone list with parents is an event");
    require(e.character(c,2).empty(),"unchanged weapons and animset are not repeated");
    c.weapons.pop_back();require(e.character(c,3).size()==1,"a changed weapon bone set is reported again");
}
// Character at (x,0,z) facing yaw, head `head` metres in its own frame (x right, y up, z forward).
vr_trace::Character body(Clock::time_point t,float x,float z,float yaw,vr_arm::Vector head,uintptr_t actor=1) {
    vr_trace::Character c;c.time=t;c.actor=actor;c.units_per_metre=1;
    const float s=std::sin(yaw),k=std::cos(yaw);
    c.root={k,0,-s,0, 0,1,0,0, s,0,k,0, x,0,z,1};
    c.head=vr_arm::Vector{x+head[0]*k+head[2]*s,head[1],z-head[0]*s+head[2]*k};
    c.native_hands[1]=vr_arm::Vector{x+0.3f*k,1.2f,z-0.3f*s};
    return c;
}
void takeover() {
    vr_trace::Events e;auto t=T0;
    const auto run=[&](std::chrono::milliseconds duration,auto head_at,float walk=0,float turn=0) {
        std::vector<std::string> out;
        for(auto end=t+duration;t<end;t+=33ms) {
            const float s=std::chrono::duration<float>(t-T0).count();
            auto c=body(t,walk*s,0,turn*s,head_at(s));
            for(auto& l:e.character(c,s)) out.push_back(l);
        }
        return out;
    };
    const auto upright=[](float s){return vr_arm::Vector{0,1.8f+0.02f*std::sin(6*s),0.05f};}; // Walking bob.
    const auto has=[](const std::vector<std::string>& lines,const char* part){return std::ranges::any_of(lines,[&](const auto& l){return contains(l,part);});};
    require(!has(run(3000ms,upright),"animation_takeover"),"ordinary head bob is not a takeover");
    require(!has(run(3000ms,upright,1.5f,0.8f),"animation_takeover"),"walking and turning move the root, not the head within it");
    const auto pounced=[](float){return vr_arm::Vector{0.1f,1.4f,-0.25f};}; // Knocked back and down.
    const auto start=run(1000ms,pounced);
    require(has(start,"animation_takeover_start") && has(start,"\"inferred\":true"),"the head leaving its baseline is an inferred takeover");
    const auto end=run(1000ms,upright);
    require(has(end,"animation_takeover_end") && has(end,"\"duration_ms\":10"),"recovery ends the takeover with its duration (about 1 s)");
    require(!has(run(150ms,pounced),"animation_takeover_start"),"a blip shorter than 200 ms is not a takeover");
    run(1000ms,upright);
    // A new character resets the baseline; nothing fires during its warm-up.
    vr_trace::Events fresh;
    for(int i=0;i<20;++i) { auto c=body(T0+i*33ms,0,0,0,{0.1f,1.4f,-0.25f},2);require(fresh.character(c,i*0.033).size()<=3,"warm-up");
        require(!c.animation_takeover,"no takeover during warm-up"); }
    auto c=body(t,0,0,0,{0,1.8f,0.05f});e.character(c,0);
    require(c.animation_deviation[0] && *c.animation_deviation[0]<0.05f && c.animation_deviation[2],"per-sample head and hand deviations are filled in");
    require(contains(vr_trace::sample(1,std::nullopt,c,std::nullopt,t),"\"animation_deviation_metres\":{\"head\":"),"deviations are in the sample");
}
void game_state() {
    vr_trace::Events e;vr_trace::GameState g{T0,true,false,false,0,0};
    require(e.game(g,0).size()==1 && e.game(g,0.1).empty(),"first state is reported, repeats are not");
    g.scripted=true;const auto cinematic=e.game(g,0.2);
    require(cinematic.size()==1 && contains(cinematic[0],"\"cinematic\":true") && balanced(cinematic[0]),"cinematic start is an event");
    g.paused=true;require(e.game(g,0.3).size()==1,"pause is an event");
    g.published=0x410;const auto line=vr_trace::sample(1,std::nullopt,std::nullopt,std::nullopt,T0,std::nullopt,g);
    require(balanced(line) && contains(line,"\"game\":{\"available\":true,\"cinematic\":true,\"paused\":true,\"flags\":1040"),"game flags are in every sample");
}
void part_visibility() {
    auto c=character(1);vr_trace::PartVisibility state;state.requested="left_shoulder";state.applied="left_shoulder";
    state.status="applied";state.control_status="valid";state.revision=2;state.updates=1;state.writes=6;state.instance=0x12345;state.resource=0x67890;
    state.changes.push_back({476,256,2048});c.part_visibility=state;
    auto line=vr_trace::sample(1,std::nullopt,c,std::nullopt,T0);
    require(balanced(line)&&contains(line,"\"part_visibility\":{\"requested\":\"left_shoulder\",\"applied\":\"left_shoulder\"")&&contains(line,"\"last_changes\":[{\"id\":476,\"before\":256,\"after\":2048}]"),"part diagnostic applied mode and actual native IDs/flags survive production trace");
    c.part_visibility->applied.reset();c.part_visibility->status="owned_mask_conflict";c.part_visibility->conflicts=1;
    line=vr_trace::sample(1,std::nullopt,c,std::nullopt,T0);
    require(balanced(line)&&contains(line,"\"applied\":null")&&contains(line,"\"conflicts\":1"),"refused part diagnostic never reports an applied preset");
}
void recorder() {
    const auto path=std::filesystem::temp_directory_path()/"sm2vr_trace_test.jsonl";
    vr_trace::Recorder r;
    require(!r.open(path,0.5,T0),"rate below 1 Hz is refused");
    require(r.open(path,20,T0),"opens");
    r.tick(T0);r.tick(T0+10ms);r.tick(T0+50ms);
    require(r.samples()==2,"20 Hz writes one sample per 50 ms");
    r.tick(T0+2s);r.tick(T0+2s+10ms);
    require(r.samples()==3,"a stall does not cause a burst of catch-up samples");
    auto a=input(0);r.input(a);auto b=input(5);b.pad.wButtons=XINPUT_GAMEPAD_B;r.input(b);
    r.note("playtest","marker");r.tick(T0+2s+20ms);r.close();
    std::vector<std::string> lines;
    { std::ifstream in{path};std::string line;while(std::getline(in,line)) lines.push_back(line); }
    require(lines.size()==6 && contains(lines[0],"\"format\":\"sm2vr-trace\""),"header, samples and events are written");
    require(contains(lines[4],"B_interact_reload") || contains(lines[5],"B_interact_reload"),"button edge event reaches the file");
    for(const auto& l:lines) require(balanced(l),"every line is a JSON object");
    std::filesystem::remove(path);
}
// docs/SPRINT_CAMERA.md: the applied sprint offset is in every input sample, with edge events.
void sprint_camera() {
    auto i=input(0);
    VrSprintCamera::Snapshot sc;sc.requested=sc.native_known=sc.confirmed=sc.active=true;sc.weight=1;sc.offset_metres=0.15f;sc.speed_metres_per_second=8.6f;
    i.sprint_camera=sc;
    const auto line=vr_trace::sample(1,i,std::nullopt,std::nullopt,T0);
    require(balanced(line) && contains(line,"\"sprint_camera\":{\"requested\":true,\"native_known\":true,\"confirmed\":true,\"active\":true,\"weight\":1.000,\"offset_metres\":0.1500,\"speed_metres_per_second\":8.60}"),
        "sample carries the sprint camera state and applied offset");
    i.sprint_camera={};
    require(contains(vr_trace::sample(1,i,std::nullopt,std::nullopt,T0),"\"speed_metres_per_second\":null"),"unobserved native speed is null");
    vr_trace::Events e;auto a=input(0);e.input(a,0);
    auto b=a;b.sprint_camera=sc;const auto start=e.input(b,0.1);
    require(start.size()==1 && contains(start[0],"sprint_camera_start") && contains(start[0],"\"confirmed\":true") && balanced(start[0]),"engaging is an event");
    require(e.input(b,0.2).empty(),"no repeat while active");
    auto c=b;c.sprint_camera.active=false;const auto end=e.input(c,0.3);
    require(end.size()==1 && contains(end[0],"sprint_camera_end"),"release of the target is an event");
}
} // namespace

int main(int argc,char** argv) {
    // --print-sample: emit one real header+sample for scripts/analyze_vr_trace.py schema checks.
    if(argc>1 && std::string{argv[1]}=="--print-sample") {
        auto i=input(0);i.head=vr_camera::IDENTITY;i.head->at(13)=1.7f;i.hands[1]=vr_camera::IDENTITY;
        vr_trace::Camera cam;cam.time=T0;cam.camera_mode=1;cam.anchored=true;cam.eyes[0]=vr_arm::Vector{10.1f,1.85f,20};cam.eye_times[0]=T0;
        std::cout<<"{\"t\":0,\"type\":\"header\",\"format\":\"sm2vr-trace\",\"version\":1,\"hz\":30}\n"<<vr_trace::sample(1.5,i,character(1),cam,T0+10ms)<<'\n';
        return 0;
    }
    try { metrics();events();sample_line();weapons();takeover();game_state();part_visibility();sprint_camera();recorder(); }
    catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
    std::cout<<"vr_trace: all checks passed\n";return 0;
}
