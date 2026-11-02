#pragma once
#include "VrArmPose.h"
#include "VrControllerInput.h"
#include "VrGrenadeThrow.h"
#include "VrGazeCamera.h"
#include "VrGuard.h"
#include "VrHolster.h"
#include "VrFingerPose.h"
#include "VrLeftPush.h"
#include "VrSprintCamera.h"
#include "VrSwingMelee.h"
#include <atomic>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// Playtest trace (SM2VR_TRACE=1): a JSON-lines file an agent can read without the
// headset. Fixed-rate "sample" lines hold the player's tracked head/hands, the actions
// sent to the game and the character's own head/hand bones, so the player-to-character
// offset can be followed over a whole session. "event" lines record every action edge
// and gesture outcome as it happens. scripts/analyze_vr_trace.py summarises a file.
namespace vr_trace {
using Clock=std::chrono::steady_clock;
using Matrix=vr_camera::Matrix;
using Vector=vr_arm::Vector;

// Controller poll (the Present thread): raw standing-space tracking (metres) plus what was sent to the game.
struct Input {
    Clock::time_point time{};
    uint64_t epoch{}; bool armed{}; const char* reason{""};
    // docs/CONTROL_CUTOUT_2026-10-26.md: controls held at arming that still wait for release
    // (VrControllerInput::Control bits), stalls resumed without a wait, and the last disarm cause.
    uint32_t blocked{}; uint64_t stall_resumes{}; double last_stall_ms{}; const char* disarm{""};
    std::optional<Matrix> head; std::array<std::optional<Matrix>,2> hands;
    std::optional<VrControllerSample> sample; // After gestures/holsters/weapon-hand routing.
    std::optional<VrControllerSample> raw_sample;
    bool supporting{},two_handed{};
    // Trigger routing (VrCombatRouting): confirmed gun hand (true = pistol/chainsword out),
    // whether fire and rev are re-armed, and whether a short attachment loss is held over.
    std::optional<bool> loadout_secondary; bool triggers_rearmed{},rev_rearmed{},rev_holdover{};
    XINPUT_GAMEPAD pad{}; uint64_t snaps{};
    bool guard_enabled{},grenade_enabled{},holster_enabled{},swing_enabled{};
    VrGuard::Snapshot guard; VrGrenadeThrow::Snapshot grenade; VrHolster::Snapshot holster; VrSwingMelee::Snapshot swing;
    bool left_push_enabled{}; VrLeftPush::Snapshot left_push;
    std::array<std::optional<vr_finger::Curl>,2> fingers; // Left, right (thumb..pinky, 0 open), when valid.
    VrSprintCamera::Snapshot sprint_camera; // docs/SPRINT_CAMERA.md
    // docs/BODY_SCALE.md: first-person scale multiplier, the held eye height and the latest
    // explicit-recenter reading (readings counts them, for the body_scale event).
    float body_factor{1}; std::optional<float> player_eye_height; uint64_t body_readings{}; const char* body_reading{""};
    bool menu{}; // The poll took the menu branch: pad is what a native menu was sent.
};
// Animation publication: the character's native (pre-IK) head/hands and what the arm
// driver made of them, in game world units.
// A weapon attachment bone: world matrix as animated (native) and as published after the
// arm IK and weapon retarget (final). Which one a weapon model hangs from is not known per
// weapon, so every weapon-like bone is traced (see weapon_bones()).
struct WeaponBone { std::string name,parent; std::optional<Matrix> native,final; std::optional<int16_t> skin_index; };
struct BodyPlacement { float back_metres{}; uint64_t updates{},rejected{},suspended{}; std::string stage,refusal;
    float applied_back_metres{};uint64_t control_revision{};std::string control_status; };
struct ArmPoseInvalidation {uint64_t calls{},refused{};bool called{},valid{};std::optional<uint32_t> before,after;std::string status;
    bool resolution_enabled{},flush_called{},resolved{},recovery_failed{};uint64_t recovery_calls{},recovery_failures{};std::optional<float> world_error;uint32_t verified_hands{};};
struct PartVisibility {
    struct Change {uint16_t id{};uint32_t before{},after{};};
    std::string requested,status,control_status;std::optional<std::string> applied;
    uintptr_t instance{},resource{};uint64_t revision{},updates{},writes{},restored{},refused{},lost_lease{},conflicts{},failures{},rollback_failures{};
    std::vector<Change> changes;
};
struct Character {
    Clock::time_point time{};
    uintptr_t actor{}; uint16_t animset{}; Matrix root{vr_camera::IDENTITY};
    uintptr_t publication_instance{},publication_buffer{},publication_rig{},publication_mapping{};
    std::optional<Vector> head,neck;
    std::array<std::optional<Vector>,2> native_hands,solved_hands,targets;
    std::vector<WeaponBone> weapons;
    // Spine, clavicle and shoulder bones (body_bones()), native and as published, plus the
    // spine-line lean from the bind pose and the head as published (after upright correction).
    std::vector<WeaponBone> body;
    std::optional<float> lean_native,lean_final; std::optional<Vector> head_final;
    // Shoulder twist from the bind pose about the spine (vr_upright::twist), signed degrees.
    std::optional<float> twist_native,twist_final;
    float units_per_metre{1}; bool driven{};
    std::optional<BodyPlacement> body_placement;
    std::optional<PartVisibility> part_visibility;
    std::optional<ArmPoseInvalidation> arm_pose_invalidation;
    // Filled by the recorder (AnimationTakeover), not the producer.
    std::array<std::optional<float>,3> animation_deviation; bool animation_takeover{};
};
// Native chainsword rev (EngineChainswordRev): whether the hook is installed, whether the
// teeth are revving now, cumulative native starts/stops/refusals, teeth play rate, loop gain.
struct ChainswordRev { Clock::time_point time{}; bool installed{},active{}; uint64_t starts{},stops{},refused{}; float play_rate{1},gain{-1}; };
// The game's own firearm aim ray (EngineWeaponProbe CtrlTarget) and aim controller target.
struct WeaponRay {
    Clock::time_point time{};
    bool owned{},overridden{}; Vector origin{},forward{},target{}; float max_distance{};
    std::optional<Vector> aim_target; bool aimed{};
};
// A world-space delta in metres along the character root's axes (rows 0, 1, 2).
inline Vector in_root(const Matrix& root,Vector world_delta,float units_per_metre) {
    Vector out{};
    for(int axis=0;axis<3;++axis) {
        const Vector row{root[axis*4],root[axis*4+1],root[axis*4+2]};const float length=vr_arm::length(row);
        out[axis]=length>1e-6f?vr_arm::dot(world_delta,row)/length/units_per_metre:0;
    }
    return out;
}
// Native game-state flags (EngineCinematicState): cinematic shot and pause.
struct GameState { Clock::time_point time{}; bool available{},scripted{},paused{}; uint32_t published{},requested{}; };

// Inferred "an animation took over the character" (pounces, grabs, knockbacks, executions).
// The game's own animated head/hand bones, in the character root's frame, normally stay
// near where locomotion and weapon animations keep them. A slow baseline follows that;
// when the animated HEAD leaves its baseline by more than START metres for HOLD, a
// takeover starts, and it ends once it is back within END for SETTLE. Hands are reported
// alongside but do not trigger (reloads and melee swing them a long way). This is a
// heuristic until the animation state machine is hooked; the per-sample deviations are
// traced so the thresholds can be retuned from real data.
struct Deviation { std::optional<float> head,left,right; };
class AnimationTakeover {
public:
    static constexpr float START=0.25f,END=0.15f,BASELINE_SECONDS=2;
    static constexpr auto HOLD=std::chrono::milliseconds(200),SETTLE=std::chrono::milliseconds(300),WARMUP=std::chrono::seconds(1),GAP=std::chrono::milliseconds(500);
    struct Result { Deviation deviation; bool started{},ended{}; float peak{},duration_ms{}; };
    Result update(const Character& c) {
        Result r;
        if(c.actor!=m_actor || (m_last!=Clock::time_point{} && (c.time<m_last || c.time-m_last>GAP)) || !(c.units_per_metre>0.01f)) reset(c);
        const auto dt=std::chrono::duration<float>(c.time-m_last).count();m_last=c.time;
        const std::array<std::optional<Vector>,3> parts{local(c,c.head),local(c,c.native_hands[0]),local(c,c.native_hands[1])};
        std::array<std::optional<float>,3> deviation;
        for(size_t i=0;i<3;++i) if(parts[i] && m_baseline[i]) deviation[i]=vr_arm::length(vr_arm::sub(*parts[i],*m_baseline[i]));
        r.deviation={deviation[0],deviation[1],deviation[2]};
        const bool warm=c.time-m_started>=WARMUP;
        const float head=deviation[0].value_or(0);
        if(!m_active) {
            if(warm && head>=START) { if(m_above==Clock::time_point{}) m_above=c.time;
                if(c.time-m_above>=HOLD) { m_active=true;m_since=m_above;m_peak=head;r.started=true; } }
            else m_above={};
        } else {
            m_peak=std::max(m_peak,head);
            if(head<=END) { if(m_below==Clock::time_point{}) m_below=c.time;
                if(c.time-m_below>=SETTLE) { m_active=false;r.ended=true;r.duration_ms=std::chrono::duration<float,std::milli>(m_below-m_since).count();m_below={};m_above={}; } }
            else m_below={};
        }
        r.peak=m_peak;
        // The baseline only learns ordinary motion: frozen while a takeover is pending or active.
        if(!m_active && m_above==Clock::time_point{}) {
            const float a=dt>0?1-std::exp(-dt/BASELINE_SECONDS):1;
            for(size_t i=0;i<3;++i) if(parts[i]) m_baseline[i]=m_baseline[i]?vr_arm::add(*m_baseline[i],vr_arm::scale(vr_arm::sub(*parts[i],*m_baseline[i]),a)):*parts[i];
        }
        return r;
    }
    bool active() const { return m_active; }
private:
    // Root-frame metres, independent of where the character stands or faces.
    static std::optional<Vector> local(const Character& c,const std::optional<Vector>& world) {
        if(!world) return std::nullopt;return in_root(c.root,vr_arm::sub(*world,{c.root[12],c.root[13],c.root[14]}),c.units_per_metre);
    }
    void reset(const Character& c) { m_actor=c.actor;m_baseline={};m_active=false;m_above=m_below={};m_started=c.time;m_last={};m_peak=0; }
    uintptr_t m_actor{}; std::array<std::optional<Vector>,3> m_baseline;
    bool m_active{}; float m_peak{};
    Clock::time_point m_last{},m_started{},m_above{},m_below{},m_since{};
};

// Body bones each sample carries: spine, arms, feet, hands and gloves. The last four, bounded
// middle-finger base/tip probes, distinguish wrist-origin placement from the native normal/glove
// finger chains. Missing named nodes remain unavailable.
inline constexpr std::array<const char*,31> BODY_BONES{"BACKdown","BACKmiddle","BACKup","NECK","clavicleL","clavicleR","shoulder_L","shoulder_R","ARM1L","ARM1R","ARM2L","ARM2R","forearm_L","forearm_R","CENTRE","Trajectory","cam_root","FOOT1L","FOOT1R","foot_targ_L","foot_targ_R","HANDL","HANDR","ARM2R_glove","HANDR_glove","shoulder_skinned_L","shoulder_skinned_R","R3_fing1","R3_fing3","R3_fing1_glove","R3_fing3_glove"};
// Rig indices of the traced body bones that exist in this rig, in BODY_BONES order.
inline std::vector<size_t> body_bones(std::span<const std::string> names) {
    std::vector<size_t> out;
    for(const char* name:BODY_BONES) { const auto it=std::find(names.begin(),names.end(),name); if(it!=names.end()) out.push_back(static_cast<size_t>(it-names.begin())); }
    return out;
}
inline constexpr size_t MAX_WEAPON_BONES=24;
// Rig indices of bones whose names look like weapon attachments or holster sockets.
inline std::vector<size_t> weapon_bones(std::span<const std::string> names) {
    std::vector<size_t> out;
    for(size_t i=0;i<names.size() && out.size()<MAX_WEAPON_BONES;++i) {
        std::string lower=names[i];for(auto& c:lower) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for(const char* part:{"gun","weapon","sword","pistol","holster"}) if(lower.find(part)!=std::string::npos) { out.push_back(i);break; }
    }
    return out;
}
// Camera anchor: game camera, the anchor after body following and the rendered eyes.
struct Camera {
    Clock::time_point time{};
    Matrix game{vr_camera::IDENTITY},anchor{vr_camera::IDENTITY};
    std::array<std::optional<Vector>,2> eyes; std::array<Clock::time_point,2> eye_times{};
    std::optional<Vector> eye_forward; // Actual composed eye orientation, including external-camera fallback.
    float units_per_metre{1};
    uintptr_t actor{}; int camera_mode{}; bool anchored{}; float body_yaw{},facing_error_degrees{},room_error{};
    // Walking in the play space (VrBodyFollowing::Snapshot): steps sent, a wall in the way, head movement
    // the view held back at walls (metres), returns that put the body under the head and the last one's
    // size (metres), and the head movement the body has taken over (world units).
    uint64_t room_steps{},room_rebases{}; bool room_blocked{}; float room_held{},room_rebased{}; Vector room_consumed{};
    bool melee_facing{}; // The game owns the body's facing after a melee press (VrBodyFollowing::melee).
    // First person rides along with fast travel (VrBodyFollowing ride_along: the jump pack), the
    // followed body's speed (physical m/s) and the comfort vignette's strength [0,1].
    bool riding{}; float travel_speed{},vignette{};
    // First person after a takeover's early return, while the game's camera is still out
    // (VrBodyFollowing::Snapshot::released, docs/TAKEOVER_RETURN_2026-10-26.md).
    bool released{};
    // A melee combo keeps first person past the envelope (VrBodyFollowing::Snapshot::combo_camera).
    bool combo_camera{};
};
// Derived player-to-character offsets in metres, in the character root's axes
// (x = root row 0, y = root row 1, z = root row 2), so they are independent of world heading.
struct Metrics {
    std::optional<Vector> head_offset; // Rendered eye centre minus the native HEAD bone.
    std::optional<Vector> head_offset_final; // Eye centre minus the HEAD bone as published (after upright correction).
    std::array<std::optional<Vector>,2> hand_target_offset; // Tracked wrist target minus the native (animated) hand.
    std::array<std::optional<float>,2> hand_residual; // Tracked target to the solved (displayed) hand.
};

inline std::optional<Vector> eye_centre(const Camera& camera,Clock::time_point now) {
    std::vector<Vector> fresh;
    for(size_t i=0;i<2;++i) if(camera.eyes[i] && camera.eye_times[i]<=now && now-camera.eye_times[i]<=std::chrono::milliseconds(100)) fresh.push_back(*camera.eyes[i]);
    if(fresh.empty()) return std::nullopt;
    if(fresh.size()==1) return fresh[0];
    return vr_arm::scale(vr_arm::add(fresh[0],fresh[1]),0.5f);
}
inline std::optional<float> eye_separation(const Camera& camera,Clock::time_point now) {
    for(size_t i=0;i<2;++i) if(!camera.eyes[i] || camera.eye_times[i]>now || now-camera.eye_times[i]>std::chrono::milliseconds(100)) return std::nullopt;
    return vr_arm::length(vr_arm::sub(*camera.eyes[0],*camera.eyes[1]));
}
inline Metrics metrics(const std::optional<Character>& character,const std::optional<Camera>& camera,Clock::time_point now) {
    Metrics m;
    if(!character || !(character->units_per_metre>0.01f)) return m;
    const auto& c=*character;
    if(camera && c.head) if(const auto eye=eye_centre(*camera,now)) m.head_offset=in_root(c.root,vr_arm::sub(*eye,*c.head),c.units_per_metre);
    if(camera && c.head_final) if(const auto eye=eye_centre(*camera,now)) m.head_offset_final=in_root(c.root,vr_arm::sub(*eye,*c.head_final),c.units_per_metre);
    for(size_t h=0;h<2;++h) {
        if(c.targets[h] && c.native_hands[h]) m.hand_target_offset[h]=in_root(c.root,vr_arm::sub(*c.targets[h],*c.native_hands[h]),c.units_per_metre);
        if(c.targets[h] && c.solved_hands[h]) m.hand_residual[h]=vr_arm::length(vr_arm::sub(*c.targets[h],*c.solved_hands[h]))/c.units_per_metre;
    }
    return m;
}

// Minimal JSON writer: fixed precision, stable key order. It builds strings (one per number), so
// Recorder formats samples outside its record lock.
// Objects only: every value follows a key (or is the outermost object), so a comma is
// needed exactly before a key that does not start its object.
class Json {
public:
    Json& key(const char* k) { if(!m_text.empty() && m_text.back()!='{') m_text+=',';m_text+='"';m_text+=k;m_text+="\":";return *this; }
    Json& number(double v,int digits=4) { m_text+=format(v,digits);return *this; }
    Json& integer(long long v) { m_text+=std::to_string(v);return *this; }
    Json& boolean(bool v) { m_text+=v?"true":"false";return *this; }
    Json& string(const std::string& v) { m_text+='"';for(char c:v) { if(c=='"' || c=='\\') m_text+='\\';if(static_cast<unsigned char>(c)>=0x20) m_text+=c; }m_text+='"';return *this; }
    Json& null() { m_text+="null";return *this; }
    Json& vector(const Vector& v,int digits=4) { m_text+='['+format(v[0],digits)+','+format(v[1],digits)+','+format(v[2],digits)+']';return *this; }
    Json& maybe(const std::optional<Vector>& v,int digits=4) { return v?vector(*v,digits):null(); }
    Json& maybe(const std::optional<float>& v,int digits=4) { return v?number(*v,digits):null(); }
    template<size_t N> Json& maybe(const std::optional<std::array<float,N>>& v,int digits=3) {
        if(!v) return null();
        m_text+='[';for(size_t i=0;i<N;++i) { if(i) m_text+=',';m_text+=format((*v)[i],digits); }m_text+=']';return *this; }
    Json& open() { m_text+='{';return *this; }
    Json& close() { m_text+='}';return *this; }
    // Arrays of objects: list() after a key, element() before each object, end_list().
    Json& list() { m_text+='[';return *this; }
    Json& element() { if(m_text.back()!='[') m_text+=',';return open(); }
    Json& end_list() { m_text+=']';return *this; }
    const std::string& text() const { return m_text; }
private:
    static std::string format(double v,int digits) { if(!std::isfinite(v)) return "null";char b[48];std::snprintf(b,sizeof b,"%.*f",digits,v);return b; }
    std::string m_text;
};
inline Vector position(const Matrix& m) { return {m[12],m[13],m[14]}; }
inline Vector forward(const Matrix& m) { return {-m[8],-m[9],-m[10]}; } // OpenVR -Z.

struct ButtonName { WORD bit; const char* name; };
// Native Type A meanings as the controls pack moves them (COMBAT_CONTROLS.md); the game context
// decides the final action. On a menu B is back and X is left A.
inline constexpr std::array<ButtonName,14> BUTTONS{{
    {XINPUT_GAMEPAD_A,"A_dodge_confirm"},{XINPUT_GAMEPAD_B,"B_interact_reload"},{XINPUT_GAMEPAD_X,"X_equipment_grenade"},{XINPUT_GAMEPAD_Y,"Y_ability"},
    {XINPUT_GAMEPAD_LEFT_SHOULDER,"LB_parry_block"},{XINPUT_GAMEPAD_RIGHT_SHOULDER,"RB_melee"},{XINPUT_GAMEPAD_LEFT_THUMB,"LS_sprint"},{XINPUT_GAMEPAD_RIGHT_THUMB,"RS_finisher"},
    {XINPUT_GAMEPAD_START,"start_menu"},{XINPUT_GAMEPAD_BACK,"back_scoreboard"},{XINPUT_GAMEPAD_DPAD_UP,"up_power_melee"},{XINPUT_GAMEPAD_DPAD_DOWN,"down_switch_weapon"},
    {XINPUT_GAMEPAD_DPAD_LEFT,"left_emote"},{XINPUT_GAMEPAD_DPAD_RIGHT,"right_stim"}}};
// "move,fire": the VrControllerInput::Control bits in mask, empty when none.
inline std::string control_names(uint32_t mask) {
    std::string out;
    for(size_t c=0;c<VrControllerInput::CONTROLS;++c) if(mask&(1u<<c)) { if(!out.empty()) out+=',';out+=VrControllerInput::CONTROL_NAMES[c]; }
    return out;
}
inline const char* outcome(VrGrenadeThrow::Outcome o) {
    switch(o) { case VrGrenadeThrow::Outcome::grab:return "grab";case VrGrenadeThrow::Outcome::thrown:return "thrown";case VrGrenadeThrow::Outcome::cancelled:return "cancelled";
        case VrGrenadeThrow::Outcome::timeout:return "timeout";case VrGrenadeThrow::Outcome::lost:return "lost";default:return "none"; }
}

// Event detection between consecutive input/character/camera records. Pure: tests drive it.
class Events {
public:
    std::vector<std::string> input(const Input& now,double t) {
        std::vector<std::string> out;
        if(m_input) {
            const auto& was=*m_input;
            const WORD changed=was.pad.wButtons^now.pad.wButtons;
            for(const auto& b:BUTTONS) if(changed&b.bit) out.push_back(menu(event(t,"button").key("name").string(b.name).key("down").boolean(now.pad.wButtons&b.bit),now).close().text());
            for(auto [name,before,after]:{std::tuple{"RT_fire",was.pad.bRightTrigger,now.pad.bRightTrigger},std::tuple{"LT_aim",was.pad.bLeftTrigger,now.pad.bLeftTrigger}})
                if((before>=128)!=(after>=128)) out.push_back(menu(event(t,"button").key("name").string(name).key("down").boolean(after>=128),now).close().text());
            if(now.guard.guards!=was.guard.guards) out.push_back(hand_event(event(t,"guard_start"),now.guard.hand).key("angle_degrees").number(now.guard.angle_degrees,1).close().text());
            if(was.guard.pose && !now.guard.pose) out.push_back(event(t,"guard_end").key("duration_ms").number(now.guard.last_duration_ms,1).close().text());
            if(now.guard.angle_rejections!=was.guard.angle_rejections) out.push_back(hand_event(event(t,"guard_rejected_angle"),now.guard.hand).key("angle_degrees").number(now.guard.angle_degrees,1).close().text());
            const auto& g=now.grenade;const auto& gw=was.grenade;
            if(g.grabs!=gw.grabs) out.push_back(hand_event(event(t,"grenade_grab"),g.hand).close().text());
            if(g.throws!=gw.throws || g.cancels!=gw.cancels)
                out.push_back(event(t,"grenade_release").key("outcome").string(outcome(g.last)).key("speed").number(g.last_release_speed,3).key("forward_fraction").number(g.last_forward_fraction,3)
                    .key("velocity").vector({g.velocity.lateral,g.velocity.height,g.velocity.forward},3).close().text());
            if(g.throws!=gw.throws) // Gaze-relative launch direction chosen for the native override (VrGrenadeAim).
                out.push_back(event(t,"grenade_aim").key("yaw_degrees").number(g.last_aim.yaw_degrees,1).key("pitch_degrees").number(g.last_aim.pitch_degrees,1)
                    .key("hand_yaw_degrees").number(g.last_aim.hand_yaw_degrees,1).key("hand_pitch_degrees").number(g.last_aim.hand_pitch_degrees,1)
                    .key("hand_speed").number(g.last_aim.hand_speed,3).key("speed_scale").number(g.last_aim.speed_scale,3).close().text());
            const auto& h=now.holster;const auto& hw=was.holster;
            if(h.salutes!=hw.salutes) out.push_back(event(t,"raised_fist_emote").close().text());
            if(h.primary!=hw.primary || h.secondary!=hw.secondary) out.push_back(event(t,"holster_select").key("slot").string(h.primary!=hw.primary?"primary":"secondary").close().text());
            if(h.misses!=hw.misses) out.push_back(event(t,"holster_miss").key("hand").vector({h.last_press[1],h.last_press[0],h.last_press[2]},3).close().text());
            if(now.swing.swings!=was.swing.swings) out.push_back(event(t,"swing").key("speed").number(now.swing.speed,3).key("travel").number(now.swing.travel,3)
                .key("power").boolean(now.swing.power).key("other_hand_speed").number(now.swing.other_hand_speed,3).close().text());
            if(now.swing.vetoes!=was.swing.vetoes) out.push_back(event(t,"swing_vetoed").key("speed").number(now.swing.speed,3).key("other_hand_speed").number(now.swing.other_hand_speed,3).close().text());
            if(now.guard.yields!=was.guard.yields) out.push_back(event(t,"guard_yield").close().text());
            if(now.left_push.pushes!=was.left_push.pushes) out.push_back(event(t,"left_push").key("hand").vector({now.left_push.lateral,now.left_push.height,now.left_push.forward},3)
                .key("travel").number(now.left_push.travel,3).key("peak").number(now.left_push.peak,3).close().text());
            if(now.swing.flicks!=was.swing.flicks) out.push_back(event(t,"swing_flick").close().text());
            if(now.swing.windups!=was.swing.windups) out.push_back(event(t,"swing_windup").close().text());
            if(now.snaps!=was.snaps) out.push_back(event(t,"snap_turn").key("count").integer(static_cast<long long>(now.snaps)).close().text());
            if(now.epoch!=was.epoch && now.snaps==was.snaps) out.push_back(event(t,"tracking_reset").key("epoch").integer(static_cast<long long>(now.epoch)).close().text());
            if(now.armed!=was.armed) out.push_back(event(t,"input_armed").key("armed").boolean(now.armed).key("reason").string(now.reason).key("cause").string(now.disarm).close().text());
            if(now.stall_resumes!=was.stall_resumes) out.push_back(event(t,"input_resumed").key("stall_ms").number(now.last_stall_ms,1).key("cause").string(now.disarm).close().text());
            if(now.blocked!=was.blocked) out.push_back(event(t,"input_held").key("controls").string(control_names(now.blocked)).key("cause").string(now.disarm).close().text());
            const auto& sc=now.sprint_camera;
            if(sc.active!=was.sprint_camera.active) out.push_back(event(t,sc.active?"sprint_camera_start":"sprint_camera_end").key("confirmed").boolean(sc.confirmed)
                .key("weight").number(sc.weight,3).key("speed_metres_per_second").maybe(sc.speed_metres_per_second,2).close().text());
            if(now.body_readings!=was.body_readings) out.push_back(event(t,"body_scale").key("reading").string(now.body_reading)
                .key("player_eye_height_metres").maybe(now.player_eye_height,3).key("factor").number(now.body_factor,4).close().text());
        }
        m_input=now;return out;
    }
    // A button the game got while a menu was up (Input::menu) is marked, so play statistics can leave it out.
    static Json& menu(Json& j,const Input& now) { return now.menu?j.key("menu").boolean(true):j; }
    std::vector<std::string> character(Character& now,double t) {
        std::vector<std::string> out;
        const auto takeover=m_takeover.update(now);
        now.animation_deviation={takeover.deviation.head,takeover.deviation.left,takeover.deviation.right};now.animation_takeover=m_takeover.active();
        if(takeover.started) out.push_back(event(t,"animation_takeover_start").key("head_deviation_metres").maybe(takeover.deviation.head,3)
            .key("left_deviation_metres").maybe(takeover.deviation.left,3).key("right_deviation_metres").maybe(takeover.deviation.right,3).key("inferred").boolean(true).close().text());
        if(takeover.ended) out.push_back(event(t,"animation_takeover_end").key("duration_ms").number(takeover.duration_ms,0).key("peak_head_deviation_metres").number(takeover.peak,3).close().text());
        if(m_actor!=now.actor) { out.push_back(event(t,"actor").key("actor").string(hex(now.actor)).close().text());m_actor=now.actor; }
        // Weapon switches may swap the animation set; log it so a floating weapon can be
        // tied to the switch that caused it.
        if(m_animset!=now.animset) { out.push_back(event(t,"animset").key("animset").integer(now.animset).close().text());m_animset=now.animset; }
        std::string bones;for(const auto& w:now.weapons) bones+=w.name+'<'+w.parent+';';
        if(bones!=m_weapon_bones) {
            auto j=event(t,"weapon_bones");j.key("bones").list();
            for(const auto& w:now.weapons) j.element().key("name").string(w.name).key("parent").string(w.parent).close();
            out.push_back(j.end_list().close().text());m_weapon_bones=bones;
        }
        return out;
    }
    std::vector<std::string> camera(const Camera& now,double t) {
        std::vector<std::string> out;
        if(!m_camera_seen || m_camera_mode!=now.camera_mode || m_anchored!=now.anchored)
            out.push_back(event(t,"camera_mode").key("mode").string(camera_mode(now.camera_mode)).key("anchored").boolean(now.anchored).close().text());
        if(m_camera_seen && m_riding!=now.riding)
            out.push_back(event(t,now.riding?"ride_start":"ride_end").key("travel_speed_metres_per_second").number(now.travel_speed,1).close().text());
        // A takeover's early return (first person while the game's camera is still out) and its end.
        if(m_camera_seen && m_released!=now.released) out.push_back(event(t,now.released?"takeover_release":"release_end").close().text());
        // A melee combo keeping first person past the envelope, and its end.
        if(m_camera_seen && m_combo_camera!=now.combo_camera) out.push_back(event(t,now.combo_camera?"combo_camera_start":"combo_camera_end").close().text());
        m_camera_seen=true;m_camera_mode=now.camera_mode;m_anchored=now.anchored;m_riding=now.riding;m_released=now.released;m_combo_camera=now.combo_camera;
        return out;
    }
    std::vector<std::string> game(const GameState& now,double t) {
        std::vector<std::string> out;
        if(!m_game || m_game->available!=now.available || m_game->scripted!=now.scripted || m_game->paused!=now.paused)
            out.push_back(event(t,"game_state").key("available").boolean(now.available).key("cinematic").boolean(now.scripted).key("paused").boolean(now.paused).close().text());
        m_game=now;return out;
    }
    static const char* camera_mode(int mode) { switch(mode) { case 1:return "body";case 2:return "external";case 3:return "scripted";case 4:return "takeover";case 5:return "viewer";case 6:return "ride";default:return "unavailable"; } }
    static std::string hex(uintptr_t v) { char b[24];std::snprintf(b,sizeof b,"%llx",static_cast<unsigned long long>(v));return b; }
private:
    static Json event(double t,const char* type) { Json j;j.open().key("t").number(t,3).key("type").string("event").key("event").string(type);return j; }
    static Json hand_event(Json j,const vr_body::Offset& o) { j.key("hand").vector({o.lateral,o.height,o.forward},3);return j; }
    std::optional<Input> m_input; uintptr_t m_actor{}; int m_camera_mode{}; bool m_anchored{},m_camera_seen{},m_riding{},m_released{},m_combo_camera{};
    uint16_t m_animset{}; std::string m_weapon_bones;
    AnimationTakeover m_takeover; std::optional<GameState> m_game;
};
inline Vector x_axis(const Matrix& m) { return {m[0],m[1],m[2]}; }
inline Vector z_axis(const Matrix& m) { return {m[8],m[9],m[10]}; }
// Distance from a weapon bone (as published) to a hand (as displayed), in metres.
inline std::optional<float> weapon_to_hand(const Character& c,const WeaponBone& w,size_t hand) {
    const auto& bone=w.final?w.final:w.native;const auto& h=c.solved_hands[hand]?c.solved_hands[hand]:c.native_hands[hand];
    if(!bone || !h || !(c.units_per_metre>0.01f)) return std::nullopt;
    return vr_arm::length(vr_arm::sub(position(*bone),*h))/c.units_per_metre;
}

// One fixed-rate sample line. Raw tracking is standing-space metres; character/camera
// positions are game world units with units_per_metre alongside; metrics are metres.
inline std::string sample(double t,const std::optional<Input>& input,const std::optional<Character>& character,const std::optional<Camera>& camera,Clock::time_point now,
    const std::optional<WeaponRay>& ray={},const std::optional<GameState>& game={},const std::optional<ChainswordRev>& rev={}) {
    Json j;j.open().key("t").number(t,3).key("type").string("sample");
    const auto age=[&](Clock::time_point when){return std::chrono::duration<double,std::milli>(now-when).count();};
    j.key("input");
    if(input) {
        const auto& i=*input;j.open().key("age_ms").number(age(i.time),1).key("epoch").integer(static_cast<long long>(i.epoch)).key("armed").boolean(i.armed).key("blocked").string(control_names(i.blocked)).key("menu").boolean(i.menu);
        j.key("head").maybe(i.head?std::optional{position(*i.head)}:std::nullopt).key("head_forward").maybe(i.head?std::optional{forward(*i.head)}:std::nullopt);
        for(size_t h=0;h<2;++h) {
            j.key(h?"right":"left").maybe(i.hands[h]?std::optional{position(*i.hands[h])}:std::nullopt);
            j.key(h?"right_forward":"left_forward").maybe(i.hands[h]?std::optional{forward(*i.hands[h])}:std::nullopt);
        }
        if(i.sample) { const auto& s=*i.sample;
            j.key("controls").open().key("move").vector({s.move_x,s.move_y,0},3).key("turn").number(s.turn_x,3).key("turn_y").number(s.turn_y,3).key("fire").number(s.fire,3).key("aim").number(s.aim,3)
                .key("right_grip").number(s.melee,3).key("left_grip").number(s.parry,3).key("trackpad").number(s.grenade,3).key("left_trackpad").vector({s.utility_x,s.utility_y,s.utility_press},3).key("fire_touch").boolean(s.fire_touch).key("chainsword_rev").number(s.chainsword_rev,3).close(); }
        if(i.raw_sample){const auto& s=*i.raw_sample;j.key("raw_controls").open().key("right_trigger").number(s.fire,3).key("left_trigger").number(s.aim,3).key("right_grip").number(s.melee,3).key("left_grip").number(s.parry,3)
            .key("left_a").boolean(s.recenter).key("left_b").boolean(s.menu).close();} // Raw Left A/B: was Left B pressed, and for how long.
        j.key("support_aim").boolean(i.supporting).key("two_handed").boolean(i.two_handed);
        j.key("loadout");if(i.loadout_secondary)j.string(*i.loadout_secondary?"secondary":"primary");else j.null();
        j.key("triggers_rearmed").boolean(i.triggers_rearmed).key("rev_rearmed").boolean(i.rev_rearmed).key("rev_holdover").boolean(i.rev_holdover);
        j.key("swing_melee").boolean(i.sample && i.sample->swing_melee).key("swing_power").boolean(i.sample && i.sample->swing_power);
        j.key("pad").open().key("buttons").integer(i.pad.wButtons).key("lt").integer(i.pad.bLeftTrigger).key("rt").integer(i.pad.bRightTrigger)
            .key("lx").integer(i.pad.sThumbLX).key("ly").integer(i.pad.sThumbLY).key("rx").integer(i.pad.sThumbRX).key("ry").integer(i.pad.sThumbRY).close();
        j.key("guard").open().key("enabled").boolean(i.guard_enabled).key("held").boolean(i.guard.held).key("in_zone").boolean(i.guard.in_zone)
            .key("hand").vector({i.guard.hand.lateral,i.guard.hand.height,i.guard.hand.forward},3).key("angle_degrees").number(i.guard.angle_degrees,1).close();
        j.key("grenade").open().key("enabled").boolean(i.grenade_enabled).key("holding").boolean(i.grenade.holding).key("in_zone").boolean(i.grenade.in_zone).key("recovering").boolean(i.grenade.recovering)
            .key("hand").vector({i.grenade.hand.lateral,i.grenade.hand.height,i.grenade.hand.forward},3).key("speed").number(i.grenade.speed,3).close();
        j.key("holster_zone").integer(static_cast<int>(i.holster.zone)).key("swing_speed").number(i.swing.speed,3);
        j.key("left_push").open().key("enabled").boolean(i.left_push_enabled).key("ready").boolean(i.left_push.ready).key("travel").number(i.left_push.travel,3).key("peak").number(i.left_push.peak,3).close();
        j.key("left_curl").maybe(i.fingers[0]).key("right_curl").maybe(i.fingers[1]);
        const auto& sc=i.sprint_camera;
        j.key("sprint_camera").open().key("requested").boolean(sc.requested).key("native_known").boolean(sc.native_known).key("confirmed").boolean(sc.confirmed)
            .key("active").boolean(sc.active).key("weight").number(sc.weight,3).key("offset_metres").number(sc.offset_metres,4)
            .key("speed_metres_per_second").maybe(sc.speed_metres_per_second,2).close();
        j.key("body_scale").open().key("factor").number(i.body_factor,4).key("player_eye_height_metres").maybe(i.player_eye_height,3).close();
        j.close();
    } else j.null();
    j.key("character");
    if(character) {
        const auto& c=*character;
        j.open().key("age_ms").number(age(c.time),1).key("actor").string(Events::hex(c.actor)).key("driven").boolean(c.driven).key("units_per_metre").number(c.units_per_metre,4)
            .key("root").vector(position(c.root),3).key("root_z_axis").vector({c.root[8],c.root[9],c.root[10]},4).key("head").maybe(c.head,3);
        j.key("publication_instance").string(Events::hex(c.publication_instance)).key("publication_buffer").string(Events::hex(c.publication_buffer))
            .key("publication_rig").string(Events::hex(c.publication_rig)).key("publication_mapping").string(Events::hex(c.publication_mapping));
        j.key("body_placement");
        if(c.body_placement) {const auto& p=*c.body_placement;
            j.open().key("back_metres").number(p.back_metres,4).key("updates").integer(p.updates).key("rejected").integer(p.rejected).key("suspended").integer(p.suspended)
                .key("stage").string(p.stage).key("refusal").string(p.refusal).key("applied_back_metres").number(p.applied_back_metres,4)
                .key("control_revision").integer(p.control_revision).key("control_status").string(p.control_status).close();
        } else j.null();
        j.key("arm_pose_invalidation");
        if(c.arm_pose_invalidation) {const auto& p=*c.arm_pose_invalidation;
            j.open().key("calls").integer(p.calls).key("refused").integer(p.refused).key("called").boolean(p.called).key("valid").boolean(p.valid).key("status").string(p.status).key("dirty_before");
            if(p.before)j.integer(*p.before);else j.null();j.key("dirty_after");if(p.after)j.integer(*p.after);else j.null();
            j.key("resolution_enabled").boolean(p.resolution_enabled).key("flush_called").boolean(p.flush_called).key("resolved").boolean(p.resolved)
                .key("verified_hands").integer(p.verified_hands).key("world_error");if(p.world_error)j.number(*p.world_error,6);else j.null();
            j.key("recovery_calls").integer(p.recovery_calls).key("recovery_failures").integer(p.recovery_failures).key("recovery_failed").boolean(p.recovery_failed).close();
        }else j.null();
        j.key("part_visibility");
        if(c.part_visibility) {const auto& p=*c.part_visibility;
            j.open().key("requested").string(p.requested).key("applied");if(p.applied)j.string(*p.applied);else j.null();
            j.key("status").string(p.status).key("control_status").string(p.control_status).key("revision").integer(p.revision)
                .key("updates").integer(p.updates).key("writes").integer(p.writes).key("restored").integer(p.restored)
                .key("refused").integer(p.refused).key("lost_lease").integer(p.lost_lease).key("conflicts").integer(p.conflicts)
                .key("failures").integer(p.failures).key("rollback_failures").integer(p.rollback_failures)
                .key("instance").string(Events::hex(p.instance)).key("resource").string(Events::hex(p.resource)).key("last_changes").list();
            for(const auto& row:p.changes)j.element().key("id").integer(row.id).key("before").integer(row.before).key("after").integer(row.after).close();
            j.end_list().close();
        } else j.null();
        for(size_t h=0;h<2;++h) {
            j.key(h?"right_native":"left_native").maybe(c.native_hands[h],3).key(h?"right_solved":"left_solved").maybe(c.solved_hands[h],3).key(h?"right_target":"left_target").maybe(c.targets[h],3);
        }
        j.key("animation_deviation_metres").open().key("head").maybe(c.animation_deviation[0],3).key("left").maybe(c.animation_deviation[1],3)
            .key("right").maybe(c.animation_deviation[2],3).close().key("animation_takeover").boolean(c.animation_takeover);
        j.key("animset").integer(c.animset).key("weapons").list();
        for(const auto& w:c.weapons) {
            const auto pos=[](const std::optional<Matrix>& m){return m?std::optional{position(*m)}:std::nullopt;};
            const auto ax=[](const std::optional<Matrix>& m,bool z){return m?std::optional{z?z_axis(*m):x_axis(*m)}:std::nullopt;};
            j.element().key("name").string(w.name).key("native").maybe(pos(w.native),3).key("native_x_axis").maybe(ax(w.native,false),3).key("native_z_axis").maybe(ax(w.native,true),3)
                .key("final").maybe(pos(w.final),3).key("final_x_axis").maybe(ax(w.final,false),3).key("final_z_axis").maybe(ax(w.final,true),3)
                .key("to_left_hand_metres").maybe(weapon_to_hand(c,w,0),3).key("to_right_hand_metres").maybe(weapon_to_hand(c,w,1),3).close();
        }
        j.end_list();
        j.key("lean_degrees").open().key("native").maybe(c.lean_native,2).key("final").maybe(c.lean_final,2).close()
            .key("twist_degrees").open().key("native").maybe(c.twist_native,2).key("final").maybe(c.twist_final,2).close().key("head_final").maybe(c.head_final,3);
        j.key("body").list();
        for(const auto& b:c.body) {
            j.element().key("name").string(b.name).key("native").maybe(b.native?std::optional{position(*b.native)}:std::nullopt,3)
                .key("final").maybe(b.final?std::optional{position(*b.final)}:std::nullopt,3).key("skin_index");
            if(b.skin_index) j.integer(*b.skin_index);else j.null();j.close();
        }
        j.end_list().close();
    } else j.null();
    j.key("game");
    if(game) j.open().key("available").boolean(game->available).key("cinematic").boolean(game->scripted).key("paused").boolean(game->paused)
        .key("flags").integer(game->published).key("requested").integer(game->requested).close();
    else j.null();
    j.key("chainsword_rev");
    if(rev) j.open().key("age_ms").number(age(rev->time),1).key("installed").boolean(rev->installed).key("active").boolean(rev->active)
        .key("starts").integer(rev->starts).key("stops").integer(rev->stops).key("refused").integer(rev->refused)
        .key("play_rate").number(rev->play_rate,3).key("gain").number(rev->gain,3).close();
    else j.null();
    j.key("weapon_ray");
    if(ray) {
        const auto& r=*ray;
        j.open().key("age_ms").number(age(r.time),1).key("owned").boolean(r.owned).key("overridden").boolean(r.overridden)
            .key("origin").vector(r.origin,3).key("forward").vector(r.forward,4).key("target").vector(r.target,3).key("max_distance").number(r.max_distance,1)
            .key("aim_target").maybe(r.aim_target,3).key("aimed").boolean(r.aimed).close();
    } else j.null();
    j.key("camera");
    if(camera) {
        const auto& c=*camera;
        j.open().key("age_ms").number(age(c.time),1).key("mode").string(Events::camera_mode(c.camera_mode)).key("anchored").boolean(c.anchored)
            .key("game").vector(position(c.game),3).key("anchor").vector(position(c.anchor),3).key("eye_centre").maybe(eye_centre(c,now),3)
            .key("eye_forward").maybe(c.eye_forward,4).key("game_forward").vector({c.game[8],c.game[9],c.game[10]},4);
        // Where the game's interaction/focus camera points relative to the player's gaze.
        const auto aligned=c.eye_forward?vr_gaze::alignment(c.game,*c.eye_forward):std::nullopt;
        j.key("gaze_native");
        if(aligned) j.open().key("yaw_degrees").number(aligned->yaw_degrees,1).key("pitch_degrees").number(aligned->pitch_degrees,1).close();else j.null();
        j.key("body_yaw").number(c.body_yaw,2).key("facing_error_degrees").number(c.facing_error_degrees,2).key("melee_facing").boolean(c.melee_facing).key("room_error_metres").number(c.room_error,3)
            .key("room_steps").integer(c.room_steps).key("room_blocked").boolean(c.room_blocked).key("room_held_metres").number(c.room_held,3)
            .key("room_rebases").integer(c.room_rebases).key("room_rebased_metres").number(c.room_rebased,3).key("room_consumed").vector(c.room_consumed,3)
            .key("ride").boolean(c.riding).key("travel_speed_metres_per_second").number(c.travel_speed,1).key("vignette").number(c.vignette,3).key("released").boolean(c.released).key("combo_camera").boolean(c.combo_camera);
        // World scale and the rendered stereo baseline (world units): their ratio is the IPD.
        j.key("units_per_metre").number(c.units_per_metre,4).key("eye_separation").maybe(eye_separation(c,now),4).close();
    } else j.null();
    const auto m=metrics(character,camera,now);
    j.key("metrics").open().key("head_offset_metres").maybe(m.head_offset,4).key("head_offset_final_metres").maybe(m.head_offset_final,4)
        .key("left_target_offset_metres").maybe(m.hand_target_offset[0],4).key("right_target_offset_metres").maybe(m.hand_target_offset[1],4)
        .key("left_residual_metres").maybe(m.hand_residual[0],4).key("right_residual_metres").maybe(m.hand_residual[1],4).close();
    j.close();return j.text();
}

// Thread-safe recorder. Producers (input poll, animation publication, camera anchor) store
// their latest record; tick() writes one sample at the configured rate plus queued events.
class Recorder {
public:
    static Recorder& get() { static auto* recorder=new Recorder;return *recorder; }
    bool open(const std::filesystem::path& path,double hz,Clock::time_point now=Clock::now()) {
        std::scoped_lock lock{m_mutex,m_file_mutex};
        if(!(hz>=1 && hz<=120)) return false;
        m_file.open(path,std::ios::trunc);if(!m_file) return false;
        m_path=path;m_interval=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>{1/hz});m_start=now;m_next=now;m_next_flush=now;m_open=true;
        // Wall clock at t=0 lines the trace up with voice recordings and sm2vr.log timestamps.
        const auto wall=std::format("{:%Y-%m-%dT%H:%M:%S}",std::chrono::zoned_time{std::chrono::current_zone(),std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())});
        Json header;header.open().key("t").number(0,3).key("type").string("header").key("format").string("sm2vr-trace").key("version").integer(1).key("hz").number(hz,1)
            .key("wall_clock_local").string(wall).close();
        m_file<<header.text()<<'\n';return true;
    }
    bool enabled() const { return m_open; }
    std::filesystem::path path() const { std::lock_guard lock{m_mutex};return m_path; }
    uint64_t samples() const { std::lock_guard lock{m_mutex};return m_samples; }
    void input(const Input& value) { if(!m_open) return;std::lock_guard lock{m_mutex};queue(m_events.input(value,seconds(value.time)));m_input=value; }
    void character(Character value) { if(!m_open) return;std::lock_guard lock{m_mutex};queue(m_events.character(value,seconds(value.time)));m_character=std::move(value); }
    void game(const GameState& value) { if(!m_open) return;std::lock_guard lock{m_mutex};queue(m_events.game(value,seconds(value.time)));m_game=value; }
    void weapon(const WeaponRay& value) { if(!m_open) return;std::lock_guard lock{m_mutex};m_ray=value; }
    void chainsword_rev(const ChainswordRev& value) { if(!m_open) return;std::lock_guard lock{m_mutex};m_rev=value; }
    // Camera records arrive per eye; positions are kept per eye so the centre can be formed.
    void camera(const Camera& value,int eye,const Vector& eye_world) {
        if(!m_open || eye<0 || eye>1) return;std::lock_guard lock{m_mutex};
        auto record=value;
        if(m_camera) { record.eyes=m_camera->eyes;record.eye_times=m_camera->eye_times; }
        record.eyes[eye]=eye_world;record.eye_times[eye]=value.time;
        queue(m_events.camera(record,seconds(value.time)));m_camera=record;
    }
    void note(const std::string& name,const std::string& detail,Clock::time_point now=Clock::now()) {
        if(!m_open) return;std::lock_guard lock{m_mutex};
        Json j;j.open().key("t").number(seconds(now),3).key("type").string("event").key("event").string(name).key("detail").string(detail).close();m_pending.push_back(j.text());
    }
    // The record lock is held only to take the pending lines and copy the latest records: the
    // render thread's camera records, the sound hooks' notes and the animation thread's character
    // records wait on it. Formatting the sample and the file writes and flush happen after, under
    // the file lock only (MOD_REVIEW_2026-10-29).
    void tick(Clock::time_point now=Clock::now()) {
        if(!m_open) return;
        std::vector<std::string> lines;std::optional<Snapshot> due;bool flush{};
        {
            std::lock_guard lock{m_mutex};lines.swap(m_pending);
            if(now>=m_next) {
                due=Snapshot{m_input,m_character,m_camera,m_ray,m_game,m_rev};++m_samples;
                m_next+=m_interval;if(m_next<=now) m_next=now+m_interval; // Never burst after a stall.
            }
            if(now>=m_next_flush) { flush=true;m_next_flush=now+std::chrono::seconds(1); }
        }
        std::lock_guard file{m_file_mutex};
        for(const auto& line:lines) m_file<<line<<'\n';
        if(due) m_file<<sample(seconds(now),due->input,due->character,due->camera,now,due->ray,due->game,due->rev)<<'\n';
        if(flush) m_file.flush();
    }
    void close() { std::scoped_lock lock{m_mutex,m_file_mutex};if(m_open) { for(const auto& line:m_pending) m_file<<line<<'\n';m_file.flush();m_file.close(); }m_open=false; }
private:
    double seconds(Clock::time_point t) const { return std::chrono::duration<double>(t-m_start).count(); }
    void queue(std::vector<std::string>&& lines) { for(auto& l:lines) m_pending.push_back(std::move(l)); if(m_pending.size()>4096) m_pending.erase(m_pending.begin(),m_pending.begin()+2048); }
    struct Snapshot { std::optional<Input> input; std::optional<Character> character; std::optional<Camera> camera; std::optional<WeaponRay> ray; std::optional<GameState> game; std::optional<ChainswordRev> rev; };
    mutable std::mutex m_mutex;
    std::mutex m_file_mutex; // m_file: tick's writes (outside m_mutex), open and close.
    std::atomic<bool> m_open{};
    std::ofstream m_file; std::filesystem::path m_path;
    Clock::time_point m_start{},m_next{},m_next_flush{}; Clock::duration m_interval{};
    uint64_t m_samples{};
    std::optional<Input> m_input; std::optional<Character> m_character; std::optional<Camera> m_camera; std::optional<WeaponRay> m_ray; std::optional<GameState> m_game; std::optional<ChainswordRev> m_rev;
    Events m_events; std::vector<std::string> m_pending;
};
} // namespace vr_trace
