#pragma once
#include "VrCameraPose.h"
#include "VrViewer.h"
#include <algorithm>
#include <chrono>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

// Simulation-independent bookkeeping. Physical movement is submitted as velocity; only
// observed accepted body displacement is removed from the camera anchor (Snapshot::consumed).
// Stick locomotion mixes with it; animations pause it. Every return to first person from a view
// that isn't the Marine's places the body under the head at once (docs/BODY_FOLLOWING.md).
class VrBodyFollowing {
public:
    using Clock = std::chrono::steady_clock;
    using Vector = std::array<float,3>;
    // takeover: a native animation pulled the camera out of the first-person envelope
    // (execution, gun strike, rock kick, get-up); the view holds its place instead.
    // ride (6; 5 is the viewer in the trace): a takeover whose eye rides on the Marine (the jump pack).
    enum class CameraMode {unavailable,body,external,scripted,takeover,ride=6};
    // Where the gaze camera may steer the native camera (vr_gaze::Steering): anchored first person,
    // and (Joe, 26 Oct) a jump pack ride, so the ground pound's landing marker follows the gaze in
    // the air. Never a takeover hold, a cutscene or an external camera.
    static bool gaze_steers(CameraMode mode,bool anchored){return (mode==CameraMode::body && anchored) || mode==CameraMode::ride;}
    struct Settings {
        // Walking in the play space (docs/BODY_FOLLOWING.md, Joe 2 Nov). Lengths are the Marine's
        // metres, 1.6 of the player's at the first-person scale, as everywhere in this class.
        float gain{12}; // [1,30] per second, room-position catch-up response.
        // [0.1,3] metres/second. 2.5 is 1.55 physical m/s: Joe's head passed 1.0 for 0.1% of the
        // 2 Nov run and peaked at 1.49.
        float max_speed{2.5f};
        // [0,0.4] metres the head may stray from the body before it follows, and it then follows
        // until within ROOM_SETTLED. 0.2 is 12.5 cm of the player's: 19 of 20 two-second stretches
        // of standing play on 2 Nov stayed inside that, head bob and looking around included.
        float deadzone{.2f};
        float room_enabled{1}; // 0 off; on from 0.5, so the overlay's slider reads as a switch.
        // [0.2,1.5] metres. Against a wall the head may lean this far past the body, then the view
        // stops there, so the eye doesn't go through the wall.
        float room_wall_metres{.5f};
        bool room_on() const {return room_enabled>=.5f;}
        float camera_radius_metres{1}; // [0.4,2] first-person horizontal envelope.
        float camera_height_metres{4}; // [2,6] absolute camera-to-body height envelope.
        // Animation takeovers (docs/ANIMATION_CAMERA.md). Instead of riding the game's
        // animation camera, the view stays where first person was when the animation started,
        // at the standing eye height, fixed in the world while the character animates.
        bool takeover_enabled{true};
        // Story cutscenes (the game's scripted camera; docs/ANIMATION_CAMERA.md): the view holds
        // the last first-person spot (eye height and heading) fixed in the world for the whole
        // cutscene, instead of riding the native cinematic camera (Joe, 24 Oct). Off: native.
        bool cutscene_hold{true};
        // The character's head is drawn during a takeover once the eye is this far from its
        // HEAD bone (physical metres), and hidden again within HEAD_CLEAR_RETURN of it, so the
        // view never starts inside the helmet and the character is never seen headless.
        float head_clear_metres{.45f}; // [0.2,1.5]
        // Native cameras beyond these (from the root) stay native: remote shots, respawns.
        // Tonight's executions/kicks/get-ups peaked at 5.9 m and 4.4 m up; a death at 6.7 m.
        float takeover_radius_metres{8}; // [4,20]
        float takeover_height_limit_metres{6}; // [4,10]
        // Eye height above the root until one is learned from the first-person camera.
        float standing_height_metres{2.55f}; // [1.5,3.5]
        // Slopes and stairs (docs/BODY_CLIPPING.md): the game's camera follows the root's
        // height with a first-order lag (21 Oct campaign fit: about 0.12-0.2 s), so climbing
        // leaves the eye 0.1-0.3 m low against the body and the armour rises into view. The
        // anchor leads the native height by height_lead_seconds times the root's vertical
        // speed, at most HEIGHT_LEAD_LIMIT physical metres. 0 disables.
        float height_lead_seconds{.18f}; // [0,0.5]
        // Standing still, the body keeps its facing until the head has turned more than this
        // from it, then catches up (TURN_CATCH_UP) until within TURN_SETTLED; walking, it
        // follows the head as before. Joe, 24 Oct: the armour moved with every look around.
        // 0: the body follows the head every frame (the rule before 24 Oct).
        float turn_threshold_degrees{30}; // [0,90]
        // Riding along (docs/ANIMATION_CAMERA.md, 26 Oct): in a takeover (the game's camera out of
        // the first-person envelope), once the followed body travels at least this fast,
        // horizontally or upwards, the eye rides on the Marine instead of holding the ground spot
        // (the jump pack's dash ran at 15-19 m/s while its camera fell 2.3 m behind; its leap
        // camera pulls back 4.5 m). A fall never rides (the ledge drop stays held), nor does an
        // execution: its lunge peaks at 8-11 m/s before its camera pulls back, then creeps under 7.
        // In first person (dodges, melee lunges, sprint) nothing changes: the eye follows anyway.
        bool ride_enabled{true};
        float ride_speed_metres_per_second{13}; // [8,30]
        // Comfort while riding: a vignette narrows the clear view (vr_vignette) as the speed goes
        // from RIDE_VIGNETTE_FROM of ride_speed up to it. 0: none; 1: the narrowest. Off by default
        // (Joe, 26 Oct: "we don't need that on the flight either"); tune it in the F11 overlay.
        float ride_vignette{0.f}; // [0,1]
        // Early return (docs/TAKEOVER_RETURN_2026-10-26.md): a held takeover that came from first
        // person hands the view back as soon as the game gives the player control back, seen as
        // the Marine moving at least release_speed_metres_per_second along the stick the game is
        // sent (within RELEASE_ALONG) for release_seconds. Before 26 Oct the hold waited for the
        // game's camera to glide back and settle: 0.85-1.1 s after an execution and 1.4-3.3 s
        // after the Vanguard's grapnel, while the Marine walked up to 14 m away from the view.
        // 0: off (the 23 Oct rule). Then, until that camera settles, first person keeps the
        // standing eye height (the camera is still out) and a new animation holds again.
        float release_speed_metres_per_second{1.5f}; // 0 or [0.5,6]
        float release_seconds{.1f}; // [0.03,0.5]
        // The same early return once an animation has run and settled (docs/ANIMATION_CAMERA.md, Joe
        // 31 Oct): the Marine's root, HEAD and both HAND bones moved faster than STILL_SPEED during the
        // takeover and then stayed under it for this long. Executions in the battle simulator carry the
        // Marine 0.3-3.6 m and the hold waited 1.1-1.2 s after he stopped when the stick wasn't pushed.
        // The root alone stops 1.2-1.5 s into an execution while the arms keep stabbing for 3-4 s more,
        // so the bones are needed. 0: off (only the stick or the camera's return ends the hold).
        float execution_still_seconds{.3f}; // 0 or [0.1,2]
        // Melee combos stay in first person (docs/ANIMATION_CAMERA.md, Joe 31 Oct: "you sort of get taken
        // out of the combat"). The big strike that ends a combo lunges the Marine 2-6 m while the game's
        // camera trails 1.3-1.6 m behind him, and a hold there left the view up to 5.4 m behind. After
        // combo_strikes strikes (native RB or D-pad up), each within combo_gap_seconds of the one
        // before, and with the last one that recent, the envelope widens to combo_camera_radius_metres
        // until the camera is back inside it, for at most combo_camera_seconds. A parry (LB) or a
        // finisher press (right stick) starts the count again, so gun strikes and executions hold as
        // before. Radius 0: off.
        float combo_camera_radius_metres{2.5f}; // 0 or [1.2,4]
        float combo_strikes{3}; // [2,8]
        float combo_gap_seconds{1.6f}; // [0.5,3]
        float combo_camera_seconds{5}; // [1,10]
        struct Parameter {const char* name;float Settings::* field;float minimum,maximum;};
        // The camera guard file and overlay. Only the two limits are required in the file; the
        // rest keep their defaults when absent, so older files still load.
        inline static constexpr std::array<Parameter,17> CAMERA_PARAMETERS{{
            {"camera_radius_metres",&Settings::camera_radius_metres,.4f,2},
            {"camera_height_metres",&Settings::camera_height_metres,2,6},
            {"turn_threshold_degrees",&Settings::turn_threshold_degrees,0,90},
            {"ride_speed_metres_per_second",&Settings::ride_speed_metres_per_second,8,30},
            {"ride_vignette",&Settings::ride_vignette,0,1},
            {"release_speed_metres_per_second",&Settings::release_speed_metres_per_second,0,6},
            {"release_seconds",&Settings::release_seconds,.03f,.5f},
            {"execution_still_seconds",&Settings::execution_still_seconds,0,2},
            {"combo_camera_radius_metres",&Settings::combo_camera_radius_metres,0,4},
            {"combo_strikes",&Settings::combo_strikes,2,8},
            {"combo_gap_seconds",&Settings::combo_gap_seconds,.5f,3},
            {"combo_camera_seconds",&Settings::combo_camera_seconds,1,10},
            {"room_enabled",&Settings::room_enabled,0,1},
            {"room_gain",&Settings::gain,1,30},
            {"room_max_speed",&Settings::max_speed,.1f,3},
            {"room_deadzone",&Settings::deadzone,0,.4f},
            {"room_wall_metres",&Settings::room_wall_metres,.2f,1.5f}}};
        bool valid() const { return std::isfinite(gain) && gain >= 1 && gain <= 30 &&
            std::isfinite(max_speed) && max_speed >= 0.1f && max_speed <= 3 &&
            std::isfinite(deadzone) && deadzone >= 0 && deadzone <= 0.4f &&
            std::isfinite(room_enabled) && room_enabled>=0 && room_enabled<=1 &&
            std::isfinite(room_wall_metres) && room_wall_metres>=.2f && room_wall_metres<=1.5f && room_wall_metres>=deadzone &&
            std::isfinite(camera_radius_metres) && camera_radius_metres>=0.4f && camera_radius_metres<=2 &&
            std::isfinite(camera_height_metres) && camera_height_metres>=2 && camera_height_metres<=6 &&
            std::isfinite(head_clear_metres) && head_clear_metres>=.2f && head_clear_metres<=1.5f &&
            std::isfinite(takeover_radius_metres) && takeover_radius_metres>=4 && takeover_radius_metres<=20 &&
            std::isfinite(takeover_height_limit_metres) && takeover_height_limit_metres>=4 && takeover_height_limit_metres<=10 &&
            std::isfinite(standing_height_metres) && standing_height_metres>=1.5f && standing_height_metres<=3.5f &&
            std::isfinite(height_lead_seconds) && height_lead_seconds>=0 && height_lead_seconds<=.5f &&
            std::isfinite(turn_threshold_degrees) && turn_threshold_degrees>=0 && turn_threshold_degrees<=90 &&
            std::isfinite(ride_speed_metres_per_second) && ride_speed_metres_per_second>=8 && ride_speed_metres_per_second<=30 &&
            std::isfinite(ride_vignette) && ride_vignette>=0 && ride_vignette<=1 &&
            std::isfinite(release_speed_metres_per_second) && (release_speed_metres_per_second==0 || (release_speed_metres_per_second>=.5f && release_speed_metres_per_second<=6)) &&
            std::isfinite(release_seconds) && release_seconds>=.03f && release_seconds<=.5f &&
            std::isfinite(execution_still_seconds) && (execution_still_seconds==0 || (execution_still_seconds>=.1f && execution_still_seconds<=2)) &&
            std::isfinite(combo_camera_radius_metres) && (combo_camera_radius_metres==0 || (combo_camera_radius_metres>=1.2f && combo_camera_radius_metres<=4)) &&
            std::isfinite(combo_strikes) && combo_strikes>=2 && combo_strikes<=8 &&
            std::isfinite(combo_gap_seconds) && combo_gap_seconds>=.5f && combo_gap_seconds<=3 &&
            std::isfinite(combo_camera_seconds) && combo_camera_seconds>=1 && combo_camera_seconds<=10; }
        static std::optional<Settings> read_camera_guard(std::istream& input) {
            Settings result;std::array<bool,CAMERA_PARAMETERS.size()> seen{};std::string line;
            while(std::getline(input,line)) {
                const auto start=line.find_first_not_of(" \t\r");if(start==std::string::npos || line[start]=='#') continue;
                const auto split=line.find('=');if(split==std::string::npos) return {};
                size_t i=0;while(i<CAMERA_PARAMETERS.size() && line.substr(0,split)!=CAMERA_PARAMETERS[i].name) ++i;
                if(i==CAMERA_PARAMETERS.size() || seen[i]) return {};
                std::istringstream value{line.substr(split+1)};
                if(!(value>>result.*(CAMERA_PARAMETERS[i].field)) || !(value>>std::ws).eof()) return {};
                seen[i]=true;
            }
            return input.eof() && seen[0] && seen[1] && result.valid()?std::optional{result}:std::nullopt;
        }
        static std::optional<Settings> load_camera_guard(const std::filesystem::path& path) {std::ifstream input{path};return read_camera_guard(input);}
        bool save_camera_guard(const std::filesystem::path& path) const {
            if(!path.is_absolute() || !valid()) return false;
            std::ofstream output{path};output<<std::setprecision(9);
            for(const auto& p:CAMERA_PARAMETERS) output<<p.name<<'='<<this->*(p.field)<<'\n';
            output.flush();return bool(output);
        }
    };
    struct Snapshot { uintptr_t actor{},body{},context{}; uint64_t turns{},room_steps{}; float yaw{},room_error{}; Vector consumed{},requested{}; bool anchored{}; CameraMode camera_mode{CameraMode::unavailable};uint64_t camera_transitions{};float camera_radius{},camera_height{};
        // Takeovers so far and the standing eye height above the root they use (world units).
        uint64_t takeovers{}; float standing_height{};
        // The height lead applied to the anchor this frame (world units).
        float height_lead{};
        // Whether the character's head should be drawn: the view is a native camera, or a
        // takeover whose eye is clear of the head. False in first person.
        bool show_head{};
        // A scripted cinematic is held at its trigger spot (camera_mode stays scripted).
        bool cutscene_hold{};
        // Melee presses that handed the facing to the game (melee()), and whether it has it now.
        uint64_t melee_holds{}; bool melee_facing{};
        // Rides so far and whether a takeover rides on the Marine now (camera_mode ride); the followed body's
        // speed in physical metres/second (3D, over TRAVEL_WINDOW); the comfort vignette's
        // strength [0,1] for the eye images (vr_vignette), 0 outside a ride.
        uint64_t rides{}; bool riding{}; float travel_speed{}; float vignette{};
        // Early returns so far, and whether first person is on now while the game's camera is still
        // out after one (Settings::release_speed_metres_per_second).
        uint64_t releases{}; bool released{};
        // Of those, the ones ended by the animation settling (Settings::execution_still_seconds).
        uint64_t still_releases{};
        // Combos that kept first person past the envelope so far, and whether one does now
        // (Settings::combo_camera_radius_metres).
        uint64_t combos{}; bool combo_camera{};
        // Walking in the play space. consumed: the head's movement the body has taken over (world
        // units). room_blocked: something stops the body taking it (a wall). room_held: metres of
        // head movement the view has not followed against walls so far. room_rebases: returns to
        // first person that put the body under the head; room_rebased: what the last one dropped (metres).
        bool room_blocked{}; float room_held{}; uint64_t room_rebases{}; float room_rebased{}; };
    bool set_settings(const Settings& settings) { if (!settings.valid()) { return false; } m_settings = settings; return true; }
    Settings settings() const { return m_settings; }
    void camera_authority(std::optional<bool> scripted) {
        if(!scripted) {suspend(CameraMode::unavailable);return;}
        if(*scripted) suspend(CameraMode::scripted);
    }
    void allow(bool value) { m_allowed = value; if (!value) { m_pending.reset(); m_dodge_pressed=false;m_dodge_until={};m_melee_pressed=false;m_melee_until={};clear_combo(); } }
    // Native dodges turn the actor before applying forward root motion. Reapplying
    // HMD yaw during that animation bends lateral rolls back toward the view.
    // Yield only facing; the stable camera anchor and native velocity stay intact.
    // Observed roll takeover is ~1.3s; bound the handover to 1.8s per press so a
    // held button or a rejected dodge cannot disable facing indefinitely.
    static constexpr auto DODGE_FACING_HOLD=std::chrono::milliseconds(1800);
    void dodge(bool pressed,Clock::time_point now) {
        if(m_allowed && pressed && !m_dodge_pressed) m_dodge_until=now+DODGE_FACING_HOLD;
        m_dodge_pressed=m_allowed && pressed;
    }
    // Melee attacks (native RB, D-pad up; docs/COMBAT_CONTROLS.md, 26 Oct): the game's melee
    // constraint turns the attacker onto the target it picked (from its camera, which the gaze
    // camera steers, and the stick) while the strike plays. Writing the head's yaw after every
    // native update undid that turn, so strikes went where the body faced while the lunge slid
    // towards the enemy. From each press the facing is the game's, while the button is held
    // and MELEE_FACING_HOLD after (Joe's lunges moved 0.2-1.2 s after the press), bounded by
    // MELEE_FACING_LIMIT per press. Meanwhile the body's facing (and the eye's placement over
    // it) follows the native one, and afterwards turns back to the head at the usual rate
    // instead of snapping.
    static constexpr auto MELEE_FACING_HOLD=std::chrono::milliseconds(1000),MELEE_FACING_LIMIT=std::chrono::milliseconds(3000);
    // The native pad buttons that count as a melee press (XInput bits): RB (light), D-pad up (power,
    // held strike) and, Joe 26 Oct, LB (parry/block), so a parry also turns onto its attacker.
    static constexpr uint16_t MELEE_FACING_BUTTONS=0x0200|0x0001|0x0100;
    static bool melee_press(uint16_t buttons){return (buttons&MELEE_FACING_BUTTONS)!=0;}
    void melee(bool pressed,Clock::time_point now) {
        if(m_allowed && pressed && !m_melee_pressed) {m_melee_from=now;if(m_state.melee_holds!=UINT64_MAX)++m_state.melee_holds;}
        if(m_allowed && pressed && now>=m_melee_from && now-m_melee_from<=MELEE_FACING_LIMIT) m_melee_until=std::max(m_melee_until,now+MELEE_FACING_HOLD);
        m_melee_pressed=m_allowed && pressed;
    }
    // The native pad buttons a combo counts (XInput bits): RB (light) and D-pad up (power), and the
    // ones that start the count again: LB (a parry, perhaps a gun strike next) and the right stick
    // click (an execution).
    static constexpr uint16_t COMBO_STRIKE_BUTTONS=0x0200|0x0001,COMBO_BREAK_BUTTONS=0x0100|0x0080;
    // The buttons the game is sent, every frame, 0 while unarmed (Settings::combo_strikes).
    void combo(uint16_t buttons,Clock::time_point now) {
        const bool strike=m_allowed && (buttons&COMBO_STRIKE_BUTTONS)!=0;
        if(strike && !m_strike_pressed) {
            m_strikes=m_strikes && combo_recent(now)?m_strikes+1:1;
            m_strike_at=now;
        }
        m_strike_pressed=strike;
        if(buttons&COMBO_BREAK_BUTTONS) m_strikes=0;
    }
    // The native left stick the game is sent (pad axes over 32767: x right, y forward of the
    // game's camera; 0,0 when unarmed), for the early return of a takeover.
    void stick(float x,float y,Clock::time_point now) {if(std::isfinite(x) && std::isfinite(y)) {m_stick={x,y};m_stick_at=now;}}
    // The native body's facing after the game's own update (degrees, the yaw() convention),
    // read before the mod writes its own; it is the body's facing while melee has it.
    void native_facing(uintptr_t actor,float yaw_degrees,Clock::time_point now) {
        if(actor && actor==m_state.actor && std::isfinite(yaw_degrees)) {m_native_yaw=yaw_degrees;m_native_yaw_received=now;}
    }
    // The character's animated HEAD bone in world units (EngineArmDriver), for the takeover's
    // head visibility. Without a fresh one the head is taken to be the standing eye height
    // above the root.
    // The character's native pose publications (root, HEAD, HANDL, HANDR): how a takeover's animation
    // settles (Settings::execution_still_seconds).
    void character_motion(const vr_viewer::Motion::Sample& sample) {
        if(m_takeover && sample.actor && sample.actor==m_state.actor && m_scale>0) m_motion.add(sample,m_scale,STILL_SPEED);
    }
    void head_bone(uintptr_t actor,const Vector& position,Clock::time_point now) {
        if(actor && actor==m_state.actor && finite(position)) {m_head_bone=position;m_head_bone_received=now;}
    }
    // The game camera's yaw from the view's stable basis (radians), so the stick moves the Marine
    // where the player looks. A takeover's view keeps the first-person basis, so it keeps this
    // too: with 0 there (before 26 Oct) the stick sent the Marine a median 124 degrees off the view
    // once the game gave control back (docs/TAKEOVER_RETURN_2026-10-26.md).
    float movement_heading_offset(Clock::time_point now) const {
        if(m_state.anchored) return fresh(m_head_received,now) && fresh(m_body_received,now)?m_movement_heading_offset:0;
        return m_takeover && fresh(m_takeover_heading_at,now) && fresh(m_body_received,now)?m_movement_heading_offset:0;
    }
    // Called with the camera root's current exact actor identity; a changed owner clears all
    // prior motion/pose state. The adapter never keeps dereferenceable actor pointers here.
    void identify(uintptr_t actor) { if (m_state.actor != actor) { m_dodge_pressed=false;m_dodge_until={};m_melee_pressed=false;m_melee_until={};m_native_yaw_received={};clear_combo();m_keep_placement=false;m_resume=m_resume || m_state.anchored || m_state.camera_mode==CameraMode::external || m_state.camera_mode==CameraMode::takeover || m_state.camera_mode==CameraMode::ride;const auto turns=m_state.turns,steps=m_state.room_steps,transitions=m_state.camera_transitions,takeovers=m_state.takeovers,melee_holds=m_state.melee_holds,rides=m_state.rides,releases=m_state.releases,still_releases=m_state.still_releases,combos=m_state.combos,room_rebases=m_state.room_rebases;const auto consumed=m_state.consumed;const float room_held=m_state.room_held; m_state={}; m_state.actor=actor; m_state.consumed=consumed;m_state.room_held=room_held;m_state.room_rebases=room_rebases; m_state.turns=turns;m_state.combos=combos;m_state.melee_holds=melee_holds;m_state.rides=rides;m_state.releases=releases;m_state.still_releases=still_releases;m_release.reset();m_blend_pending=false;m_blend_at={};m_last_eye_at={}; m_state.room_steps=steps;m_state.camera_transitions=transitions;m_state.takeovers=takeovers; m_pending.reset();m_takeover.reset(); m_reference=0; m_body_received={};m_head_received={};m_head_bone_received={};m_eye_received={}; } }
    void body(uintptr_t actor,uintptr_t object,uintptr_t context,const Vector& position,Clock::time_point now) {
        if (!actor || actor != m_state.actor || !finite(position)) { return; }
        if (m_state.body && object != m_state.body) { suspend(CameraMode::unavailable); }
        const bool restart=object != m_state.body || !fresh(m_body_received,now);
        if(restart) m_track_count=0;
        m_track[m_track_next]={now,position};m_track_next=(m_track_next+1)%m_track.size();m_track_count=std::min(m_track_count+1,m_track.size());
        if (restart) { m_speed_from=now;m_speed_position=position;m_ground_speed.reset();m_vertical_speed.reset(); }
        else if (now-m_speed_from>=SPEED_WINDOW) {
            // Native body positions over >=100 ms: per-update deltas jitter with hook timing.
            const float elapsed=std::chrono::duration<float>(now-m_speed_from).count();
            const float distance=std::hypot(position[0]-m_speed_position[0],position[2]-m_speed_position[2]);
            m_ground_speed=elapsed<=0.5f?std::optional{distance/elapsed}:std::nullopt;
            m_vertical_speed=elapsed<=0.5f?std::optional{(position[1]-m_speed_position[1])/elapsed}:std::nullopt;
            m_speed_from=now;m_speed_position=position;
        }
        m_state.body=object; m_state.context=context; m_position=position; m_body_received=now;++m_body_updates;
        settle_room(); // Here, or the eye drawn before the next game step would be a step ahead.
    }
    // Horizontal speed of the followed native body in world units/second, measured
    // from its own positions; nullopt until measured or when the body is not fresh.
    std::optional<float> ground_speed(Clock::time_point now) const {
        return m_state.body && fresh(m_body_received,now)?m_ground_speed:std::nullopt;
    }
    // Vertical speed of the followed native body in world units/second (same window).
    std::optional<float> vertical_speed(Clock::time_point now) const {
        return m_state.body && fresh(m_body_received,now)?m_vertical_speed:std::nullopt;
    }
    // The followed body's velocity (world units/second) over the latest TRAVEL_WINDOW of its own
    // positions, a sliding window, so a take-off is seen within TRAVEL_WINDOW (riding along); the
    // 100 ms SPEED_WINDOW steps behind ground_speed() would leave a dash held for up to 0.2 s.
    std::optional<Vector> travel_velocity(Clock::time_point now) const {
        if(!m_state.body || !fresh(m_body_received,now) || m_track_count<2) return std::nullopt;
        const auto& newest=m_track[(m_track_next+m_track.size()-1)%m_track.size()];
        for(size_t k=1;k<m_track_count;++k) {
            const auto& older=m_track[(m_track_next+m_track.size()-1-k)%m_track.size()];
            const auto span=newest.first-older.first;
            if(span<TRAVEL_WINDOW) continue;
            if(span>TRAVEL_LIMIT) return std::nullopt;
            const float seconds=std::chrono::duration<float>(span).count();
            return Vector{(newest.second[0]-older.second[0])/seconds,(newest.second[1]-older.second[1])/seconds,(newest.second[2]-older.second[2])/seconds};
        }
        return std::nullopt;
    }
    static constexpr auto TRAVEL_WINDOW=std::chrono::milliseconds(60),TRAVEL_LIMIT=std::chrono::milliseconds(300);
    static constexpr auto SPEED_WINDOW=std::chrono::milliseconds(100);
    static constexpr float HEIGHT_LEAD_LIMIT=.3f,HEIGHT_LAG_MARGIN=.05f; // physical metres
    // scale is the Marine's: every limit here was fitted in its metres (docs/BODY_SCALE.md). The
    // tracked head arrives in those metres too (marine_head), so scale times head is still the eyes'
    // offset in world units.
    static vr_camera::Matrix marine_head(vr_camera::Matrix head,float fit_factor) {
        for(int j=12;j<15;++j) head[j]*=fit_factor;
        return head;
    }
    // The anchor for this eye (game camera in, view origin out), then the comfort vignette.
    vr_camera::Matrix camera(const vr_camera::Matrix& game,const vr_camera::Matrix& head,
        uint64_t reference,float scale,Clock::time_point now,std::optional<bool> scripted=false) {
        const auto anchor=view(game,head,reference,scale,now,scripted);
        comfort(now);
        return anchor;
    }
    // Riding along (Settings::ride_speed_metres_per_second): the ride ends RIDE_SETTLE after the
    // body's speed falls under RIDE_RELEASE of the ride speed (the leap's apex stays inside it).
    // The held view joins the riding eye, and a takeover that rode joins first person, through an
    // offset decaying with RIDE_BLEND_SECONDS (no cut), unless it is over RIDE_BLEND_LIMIT
    // physical metres. Faster than RIDE_TELEPORT (physical m/s) is a teleport, not travel (the
    // Vanguard's grapnel pull, in first person, peaked at 31-49 in the 26 Oct trace).
    static constexpr float RIDE_RELEASE=.4f,RIDE_BLEND_SECONDS=.12f,RIDE_BLEND_LIMIT=2,RIDE_TELEPORT=50;
    static constexpr auto RIDE_SETTLE=std::chrono::milliseconds(400);
    // The vignette's target rises from 0 at RIDE_VIGNETTE_FROM of the ride speed to ride_vignette
    // at the ride speed; it follows the target with VIGNETTE_ATTACK going in and VIGNETTE_RELEASE
    // going out (seconds), so a brief dip does not flicker it.
    static constexpr float RIDE_VIGNETTE_FROM=.5f,VIGNETTE_ATTACK=.05f,VIGNETTE_RELEASE=.25f;
private:
    vr_camera::Matrix view(const vr_camera::Matrix& game,const vr_camera::Matrix& head,
        uint64_t reference,float scale,Clock::time_point now,std::optional<bool> scripted) {
        // A new reference starts the room again where the head is.
        if(reference!=m_room_reference) {m_room_reference=reference;m_state.consumed={};}
        if(!scripted || *scripted) {
            if(scripted && m_settings.cutscene_hold && std::isfinite(scale) && scale>=.01f && scale<=100 && vr_camera::rigid(head)) {
                if(!m_cutscene)begin_cutscene(now);
                if(m_cutscene) {camera_authority(scripted);m_state.cutscene_hold=true;m_state.show_head=true;
                    auto anchor=m_cutscene->basis;for(int j=0;j<3;++j)anchor[12+j]=m_cutscene->eye[j];return anchor;}
            }
            m_cutscene.reset();m_state.cutscene_hold=false;camera_authority(scripted);return native(game);
        }
        // Gameplay again: first person re-anchors on the character as after any native camera.
        m_cutscene.reset();m_state.cutscene_hold=false;
        if (!m_state.actor || !m_state.body || !fresh(m_body_received,now) || !vr_camera::rigid(game,false) ||
            !vr_camera::rigid(head) || !std::isfinite(scale) || scale < 0.01f || scale > 100) { suspend(CameraMode::unavailable);return native(game); }
        m_state.camera_radius=std::hypot(game[12]-m_position[0],game[14]-m_position[2])/scale;
        m_state.camera_height=(game[13]-m_position[1])/scale;
        // A known local dodge briefly leaves the normal 1m envelope (observed
        // up to 1.96m). Keep its established camera reference, never a new one.
        // Scripted/unknown authority above and larger separations still hand over.
        const bool dodge_camera=m_state.anchored && m_reference==reference && m_scale==scale && now<m_dodge_until;
        float camera_radius=dodge_camera?std::max(m_settings.camera_radius_metres,2.5f):m_settings.camera_radius_metres;
        if(combo_camera(reference,scale,now)) camera_radius=std::max(camera_radius,m_settings.combo_camera_radius_metres);
        const bool outside=m_state.camera_radius>camera_radius || std::abs(m_state.camera_height)>m_settings.camera_height_metres;
        const bool remote=m_state.camera_radius>m_settings.takeover_radius_metres || std::abs(m_state.camera_height)>m_settings.takeover_height_limit_metres;
        travel(scale,now);
        if(m_takeover) {
            // A recenter or scale change calibrates anew through the ordinary path below.
            if(m_reference!=reference || m_scale!=scale || !m_settings.takeover_enabled) m_takeover.reset();
            // Riding, the eye is on the Marine whatever the game's camera does (it trails a fast flight).
            else if(remote && !m_takeover->riding) {suspend(CameraMode::external);return native(game);}
            // A takeover that rode (the jump pack) hands back to first person without a cut.
            else if(!takeover_continues(outside,now)) {m_blend_pending=m_takeover->rode;m_takeover.reset();}
            // The game gave control back (the Marine walks where the stick says): first person now,
            // while its camera is still out (docs/TAKEOVER_RETURN_2026-10-26.md).
            else if(const bool still=settled(now);still || released(game,now)) {m_blend_pending=m_takeover->rode;m_takeover.reset();m_release=Release{now};if(m_state.releases!=UINT64_MAX)++m_state.releases;
                if(still && m_state.still_releases!=UINT64_MAX)++m_state.still_releases;}
            else return takeover_anchor(game,head,now);
        }
        m_state.height_lead=0;m_state.riding=false;
        if(m_release) {
            // Until the game's camera settles, as a takeover would end. Something other than the stick
            // moving the Marine (a new animation), or RELEASE_LIMIT, hands back to the rules: with the
            // camera still out, a new hold from where first person is now.
            if(m_reference!=reference || m_scale!=scale || remote || m_settings.release_speed_metres_per_second<=0) m_release.reset();
            else if(settle_done(m_release->settle,outside,now)) {m_release.reset();m_blend_pending=true;} // the native height joins through the blend
            else if(animated(game,now) || now-m_release->since>=RELEASE_LIMIT) m_release.reset();
        }
        m_state.released=m_release.has_value();
        if(outside && !m_release) {
            if(m_settings.takeover_enabled && !remote && begin_takeover(game,head,reference,scale,now)) return takeover_anchor(game,head,now);
            suspend(CameraMode::external);return native(game);
        }
        // Geometric camera separation also happens during ordinary dodges.
        // Keep the established placement when that same actor/reference returns;
        // sampling the still-trailing native camera here permanently moves the
        // eyes behind the body. Scripted/unknown ownership and recenter still
        // take the full native handover path below.
        if(!m_state.anchored && m_keep_placement && m_reference==reference && m_scale==scale){
            m_state.anchored=true;m_resume=false;m_keep_placement=false;m_room_rebase=true;
        }
        if (!m_state.anchored || m_reference != reference) {
            if (!vr_camera::level_anchor(game,m_basis,false)) { return game; }
            const auto placement_basis=m_basis;
            const bool resuming=m_resume;
            // Returning from an external camera must not apply the current accumulated
            // head yaw twice. Rebase the game heading, without changing the XR reference/IPD.
            if(m_resume) {
                auto heading=vr_camera::IDENTITY;
                if(!vr_camera::level_anchor(head,heading)) return game;
                heading[12]=heading[13]=heading[14]=0;
                vr_camera::Matrix rebased{},view{};
                if(!vr_camera::compose(m_basis,vr_camera::inverse(heading),scale,rebased,view)) return game;
                m_basis=rebased;
            }
            m_resume=false;m_keep_placement=false;
            m_reference=reference; m_scale=scale; m_room_rebase=true;
            Vector world_offset{};
            for (int j=0;j<3;++j) { world_offset[j]=game[12+j]-m_position[j]; }
            // Store horizontal head placement in the initial heading basis, not world axes.
            const auto& offset_basis=resuming?placement_basis:m_basis;
            m_camera_offset={world_offset[0]*offset_basis[0]+world_offset[2]*offset_basis[2],0,
                world_offset[0]*offset_basis[8]+world_offset[2]*offset_basis[10]};
            m_state.anchored=true;m_body_yaw.reset();
        }
        if(m_room_rebase) room_rebase(head,scale);
        mode(CameraMode::body);m_state.show_head=false;
        auto anchor=m_basis;
        vr_camera::Matrix target{},view{};
        if (!vr_camera::compose(anchor,head,scale,target,view)) { return game; }
        vr_camera::Matrix heading{};
        if (!vr_camera::level_anchor(target,heading,false)) { return game; }
        const float head_yaw=std::atan2(heading[8],heading[10])*57.295779513f;
        m_state.yaw=body_yaw(head_yaw,now);
        // The head's root-relative placement turns with the body, not with every look around.
        // The head's level axes turned by the body's lag behind it (the same map that takes the
        // forward axis's yaw atan2(x,z) from the head's to the body's), so a mirrored native
        // frame stays mirrored; on the head's heading exactly the head's axes, as before.
        std::array<float,4> body_axes{heading[0],heading[2],heading[8],heading[10]}; // right x,z; forward x,z
        if(m_state.yaw!=head_yaw) {const float d=(m_state.yaw-head_yaw)/57.295779513f,c=std::cos(d),n=std::sin(d);
            for(int k:{0,2}) {const float x=body_axes[k],z=body_axes[k+1];body_axes[k]=x*c+z*n;body_axes[k+1]=z*c-x*n;}}
        // Native stick directions follow the game's camera, which can rotate
        // independently during a roll. VrSession adds current tracked/snap yaw.
        m_movement_heading_offset=std::remainder(std::atan2(game[8],game[10])-std::atan2(m_basis[8],m_basis[10]),6.28318530718f);
        // The body turns under the HMD: its head's root-relative placement must turn too.
        // Keep orientation on the stable reference to avoid native camera yaw feedback.
        for (int j:{0,2}) {
            anchor[12+j]=m_position[j]+m_camera_offset[0]*body_axes[j?1:0]+m_camera_offset[2]*body_axes[j?3:2]-m_state.consumed[j];
        }
        // Native height and animations remain until calibrated bones exist, led by the root's
        // vertical speed so slopes and stairs do not leave the eye low against the body.
        // After an early return the game's camera is still out (and gliding back): the standing eye height.
        anchor[13]=m_release?m_position[1]+standing_units(scale):game[13];
        const auto vy=vertical_speed(now);
        if(vy && m_settings.height_lead_seconds>0 && !m_release) {
            const float lead=std::clamp(m_settings.height_lead_seconds**vy,-HEIGHT_LEAD_LIMIT*scale,HEIGHT_LEAD_LIMIT*scale);
            // Never more than the native camera actually trails the root (against the learned
            // standing height, plus HEIGHT_LAG_MARGIN for its bob and the estimate): on a platform
            // that carries both (an elevator, 24 Oct) it trails nothing, and the full lead put
            // the eye 0.3 m into the armour.
            const float lag=m_standing?*m_standing-(game[13]-m_position[1]):lead,margin=HEIGHT_LAG_MARGIN*scale;
            m_state.height_lead=lead>0?std::clamp(lead,0.f,std::max(lag+margin,0.f)):std::clamp(lead,std::min(lag-margin,0.f),0.f);
            anchor[13]+=m_state.height_lead;
        }
        // Learned on level ground only, so a slope's lag is measured against it.
        if(!m_release && (!vy || std::abs(*vy)<=STANDING_STILL*scale)) learn_standing(game[13]-m_position[1],scale,now);
        blend(anchor,scale,now);
        // Where first person stands: a takeover starting next frame holds this spot.
        m_eye={anchor[12],anchor[13],anchor[14]};m_eye_root=m_position;m_eye_received=now;m_spot=Spot{m_eye,m_basis,now};
        m_last_eye=m_eye;m_last_eye_at=now;
        m_physical=room_head(head,scale);m_head_received=now;
        m_state.room_error=std::hypot(m_physical[0]-m_state.consumed[0],m_physical[2]-m_state.consumed[2])/scale;
        return anchor;
    }
public:
    std::optional<float> yaw(uintptr_t actor,Clock::time_point now) {
        m_state.melee_facing=m_allowed && now<m_melee_until;
        if (!m_allowed || now<m_dodge_until || m_state.melee_facing || !m_state.anchored || actor != m_state.actor || !fresh(m_head_received,now) || !fresh(m_body_received,now)) { return std::nullopt; }
        ++m_state.turns; return m_state.yaw;
    }
    // What last step's room velocity left in the physics body's velocity, which the game's
    // character update is about to read (its input +0xa0). That update accelerates from there, so
    // left in, a room step carries on as momentum and reads as the game moving the Marine (it stops
    // at about 9 world units/s^2 in the 2 Nov trace). The adapter takes this off the input, so the
    // update sees only its own movement. A wall's share has gone from the body already, so only
    // what survived the collision comes off.
    Vector room_feedback(uintptr_t context,const Vector& input) const {
        const float asked=std::hypot(m_room_last[0],m_room_last[2]);
        if(!context || context!=m_state.context || !(asked>0) || !finite(input)) return {};
        const Vector extra{input[0]-m_native_last[0],0,input[2]-m_native_last[2]};
        const float size=std::hypot(extra[0],extra[2]);
        if(!(size>0) || extra[0]*m_room_last[0]+extra[2]*m_room_last[2]<=0) return {};
        const float part=std::min(1.f,asked/size);
        return {extra[0]*part,0,extra[2]*part};
    }
    // One game step. native: the velocity the game's own update chose. Returns the velocity to add.
    Vector velocity(uintptr_t context,const Vector& native,float dt,Clock::time_point now) {
        if (!m_allowed || !m_state.anchored || m_room_rebase || !context || context != m_state.context || !fresh(m_head_received,now) ||
            !fresh(m_body_received,now) || !finite(native) || !std::isfinite(dt) || dt <= 0 || dt > 0.1f) { m_pending.reset();m_room_last={}; return {}; }
        room_window(now);
        if(m_pending) for(int j:{0,2}) m_pending->native[j]+=native[j]*dt;
        m_room_last={};m_state.requested={};
        if (!m_settings.room_on() || room_paused(native,now)) { return {}; }
        Vector desired{m_physical[0]-m_state.consumed[0],0,m_physical[2]-m_state.consumed[2]};
        auto distance=std::hypot(desired[0],desired[2]);
        m_state.room_error=distance/m_scale;
        // Blocked with the head past room_wall_metres: the view stops there and the room slides
        // along, so a step back moves the body at once instead of walking back through the wall.
        const float wall=m_settings.room_wall_metres*m_scale;
        if(m_state.room_blocked && distance>wall) {
            // Only the way the wall refuses (what the body left open), so along it the view stays the head's.
            const auto open=m_pending?m_pending->step:desired;
            const float size=std::hypot(open[0],open[2]);
            if(size>0) {
                const float slide=std::min((distance-wall)*m_settings.gain,m_settings.max_speed*m_scale)*dt;
                for(int j:{0,2}) {m_state.consumed[j]+=open[j]/size*slide;desired[j]-=open[j]/size*slide;}
                distance=std::hypot(desired[0],desired[2]);m_state.room_held+=slide/m_scale;
            }
        }
        if(!m_room_walking && distance<=m_settings.deadzone*m_scale) return {};
        m_room_walking=true;
        if(distance<=std::min(ROOM_SETTLED,m_settings.deadzone)*m_scale) {m_room_walking=false;return {};}
        const auto speed=std::min({distance*m_settings.gain,m_settings.max_speed*m_scale,distance/dt});
        for (int j:{0,2}) { desired[j]*=speed/distance; }
        m_state.requested=desired; ++m_state.room_steps;
        if(!m_pending) {m_pending=Pending{.start=m_position,.body_updates=m_body_updates};for(int j:{0,2}) m_pending->native[j]=native[j]*dt;}
        for(int j:{0,2}) m_pending->step[j]+=desired[j]*dt;
        m_pending->last=speed*dt;m_pending->idle=0;m_room_asked+=speed*dt;
        m_room_last=desired;m_native_last=native;
        return desired;
    }
    // Camera mode (the viewer) drew this frame: nothing in here saw the head. The player may have
    // walked round the Marine, so the next first-person frame puts the body under the head.
    void viewer_frame() {m_room_rebase=true;m_pending.reset();}
    Snapshot snapshot() const { return m_state; }
    uint64_t cutscene_holds() const { return m_cutscene_holds; }
    // The followed body's world position while fresh (viewer mode stands on its ground).
    std::optional<Vector> body_position(Clock::time_point now) const {
        return m_state.body && fresh(m_body_received,now)?std::optional{m_position}:std::nullopt;
    }
    // Takeover exit: the native camera must be back near the head (inside SETTLE_RADIUS and
    // within SETTLE_HEIGHT of the standing height) for SETTLE_TIME, or inside the envelope for
    // INSIDE_LIMIT, so the return glide of the game's camera never bobs the view.
    static constexpr float SETTLE_RADIUS=.6f,SETTLE_HEIGHT=.2f;
    static constexpr auto SETTLE_TIME=std::chrono::milliseconds(150),INSIDE_LIMIT=std::chrono::milliseconds(1000);
    // Early return (Settings::release_speed_metres_per_second): the stick must be at least
    // RELEASE_STICK out and the Marine within 30 degrees of it (RELEASE_ALONG, a cosine). Afterwards
    // ANIMATED_SPEED (physical m/s) more than 45 degrees off the stick (ANIMATED_ALONG) for
    // ANIMATED_TIME is an animation again (execution lunges ran 8-11 m/s, melee 13-18), and the rules
    // apply again RELEASE_LIMIT after the return if the game's camera has not settled by then.
    static constexpr float RELEASE_STICK=.5f,RELEASE_ALONG=.866f,ANIMATED_SPEED=3,ANIMATED_ALONG=.707f;
    static constexpr auto ANIMATED_TIME=std::chrono::milliseconds(250),RELEASE_LIMIT=std::chrono::milliseconds(4000);
    // Physical m/s: the fastest bone under this counts as still (as the viewer's play, vr_viewer::Play).
    static constexpr float STILL_SPEED=.35f;
    // Head visibility hysteresis: once drawn, the head is hidden again only when the eye comes
    // this much closer than head_clear_metres to it, so it never flickers at the threshold.
    static constexpr float HEAD_CLEAR_RETURN=.1f;
    static constexpr float STANDING_SECONDS=2; // standing-height learning time constant
    static constexpr float STANDING_STILL=.05f; // physical metres/second of vertical motion
    // An animation camera shaped like tonight's takeovers (21 Oct: pulled 3-5 m back, up to
    // 1.8 m up, bobbing): 3 m behind and 0.5 m right of the root, 0.8 m up plus a 0.25 m,
    // 1.5 Hz bob. Tests and the live -TakeoverControl diagnostic feed it in place of the
    // native camera; the game's own camera is never changed.
    static vr_camera::Matrix pulled_back(const vr_camera::Matrix& game,const Vector& root,float scale,float seconds) {
        auto out=game;
        const float right_x=game[0],right_z=game[2],forward_x=game[8],forward_z=game[10];
        const float r=std::hypot(right_x,right_z),f=std::hypot(forward_x,forward_z);
        if(!(r>1e-4f) || !(f>1e-4f) || !std::isfinite(seconds)) return game;
        out[12]=root[0]+scale*(.5f*right_x/r-3*forward_x/f);
        out[14]=root[2]+scale*(.5f*right_z/r-3*forward_z/f);
        out[13]=game[13]+scale*(.8f+.25f*std::sin(seconds*9.42477796f));
        return out;
    }
private:
    // Walking in the play space. ROOM_SETTLED: the catch-up ends this close (metres). ROOM_STICK: the
    // stick counts as pushed; with it neutral a body moving faster than ROOM_NATIVE_QUIET (world
    // units/s) is being moved by an animation, which keeps it. ROOM_BLOCKED: over a ROOM_WINDOW, a
    // body taking less than this share of what it was asked is against something. ROOM_CARRY: game
    // steps a request stays open for the position that shows it. ROOM_TELEPORT: metres no step moves.
    // ROOM_CONTACT: metres between positions that, while blocked, are the wall's contact and not a move.
    static constexpr float ROOM_SETTLED=.05f,ROOM_STICK=.1f,ROOM_NATIVE_QUIET=.05f,ROOM_BLOCKED=.5f,ROOM_TELEPORT=.5f,ROOM_CONTACT=.02f;
    static constexpr int ROOM_CARRY=3;
    static constexpr auto ROOM_WINDOW=std::chrono::milliseconds(150);
    // The head's displacement from the reference in the world (units, level).
    Vector room_head(const vr_camera::Matrix& head,float scale) const {
        Vector out{};for(int j:{0,2}) out[j]=scale*(head[12]*m_basis[j]+head[13]*m_basis[4+j]-head[14]*m_basis[8+j]);
        return out;
    }
    // First person again after a view that wasn't the Marine's: whatever the head did meanwhile
    // is dropped, so the eye is on the body now and the body has nothing to catch up with.
    void room_rebase(const vr_camera::Matrix& head,float scale) {
        const auto walked=m_settings.room_on()?room_head(head,scale):Vector{};
        const float dropped=std::hypot(walked[0]-m_state.consumed[0],walked[2]-m_state.consumed[2])/scale;
        m_state.consumed=walked;m_room_rebase=m_room_walking=m_state.room_blocked=false;m_pending.reset();
        m_room_asked=m_room_moved=0;m_room_window_from={};
        if(dropped>0) {++m_state.room_rebases;m_state.room_rebased=dropped;}
    }
    // A native camera view keeps the consumed walk off the eye too, or the eye would sit that far
    // from the game's camera. The head maps through the game camera's heading here: same components.
    vr_camera::Matrix native(const vr_camera::Matrix& game) const {
        const auto& c=m_state.consumed;
        vr_camera::Matrix level{};
        if((c[0]==0 && c[2]==0) || !vr_camera::level_anchor(game,level,false)) return game;
        const float right=c[0]*m_basis[0]+c[2]*m_basis[2],forward=c[0]*m_basis[8]+c[2]*m_basis[10];
        auto out=game;
        for(int j:{0,2}) out[12+j]-=right*level[j]+forward*level[8+j];
        return out;
    }
    // Something other than the head has the Marine: dodges and melee (the game's root motion and
    // facing), a return still settling, or an animation moving it with the stick neutral. The
    // head's movement waits and the body catches up after. The stick mixes with it.
    bool room_paused(const Vector& native,Clock::time_point now) const {
        if(m_state.camera_mode!=CameraMode::body || m_release || m_state.combo_camera || now<m_dodge_until || now<m_melee_until) return true;
        const bool stick=fresh(m_stick_at,now) && std::hypot(m_stick[0],m_stick[1])>=ROOM_STICK;
        return !stick && std::hypot(native[0],native[2])>ROOM_NATIVE_QUIET;
    }
    // Takes the requests since the body last reported a position against that position. What moved
    // the body beyond the game's own velocity is the room's, up to what was requested and still
    // open, so a wall or a late position consumes only what the body really did. Along a wall the
    // body's own direction is the one consumed, so the eye doesn't move.
    void settle_room() {
        if(!m_pending || m_pending->body_updates==m_body_updates) return;
        auto& p=*m_pending;
        const Vector moved{m_position[0]-p.start[0]-p.native[0],0,m_position[2]-p.start[2]-p.native[2]};
        const float size=std::hypot(moved[0],moved[2]),open=std::hypot(p.step[0],p.step[2]);
        // A teleport/correction/load must not masquerade as accepted playspace movement.
        if(size>ROOM_TELEPORT*m_scale) {m_pending.reset();return;}
        const bool forward=moved[0]*p.step[0]+moved[2]*p.step[2]>0;
        // Pressed against a wall the contact rocks the body 2-3 mm each way (2 Nov, in the hangar).
        // Crediting only the forward half walked the view off the wall at 1 cm a second, so both
        // count: the eye holds still and nothing builds up.
        if(m_state.room_blocked && size<=ROOM_CONTACT*m_scale) {
            for(int j:{0,2}) m_state.consumed[j]+=moved[j];
            if(forward) m_room_moved+=size;
        }
        else if(size>0 && forward) {
            const float taken=std::min(1.f,open/size);
            for(int j:{0,2}) {m_state.consumed[j]+=moved[j]*taken;p.step[j]-=moved[j]*taken;}
            m_room_moved+=size*taken;
        }
        p.start=m_position;p.native={};p.body_updates=m_body_updates;
        if(++p.idle>ROOM_CARRY) {m_pending.reset();return;}
        const float limit=ROOM_CARRY*p.last,left=std::hypot(p.step[0],p.step[2]);
        if(left>limit) for(int j:{0,2}) p.step[j]*=limit/left;
    }
    void room_window(Clock::time_point now) {
        if(m_room_window_from==Clock::time_point{} || now<m_room_window_from) {m_room_window_from=now;return;}
        if(now-m_room_window_from<ROOM_WINDOW) return;
        m_state.room_blocked=m_room_asked>0 && m_room_moved<ROOM_BLOCKED*m_room_asked;
        m_room_asked=m_room_moved=0;m_room_window_from=now;
    }
    static constexpr float TURN_CATCH_UP=240,TURN_SETTLED=2; // degrees/second, degrees
    static constexpr float TURN_WALKING=.3f; // physical metres/second of ground speed
    static constexpr float TURN_SNAP=12; // degrees in one frame: a snap turn, which the body takes at once
    // The body's facing (degrees) for this frame's head heading. Re-anchoring starts it on the head.
    float body_yaw(float head,Clock::time_point now) {
        // Melee has the facing (melee()): the body is where the game turned it.
        if(m_allowed && now<m_melee_until && fresh(m_native_yaw_received,now)) {
            m_body_yaw=m_native_yaw;m_head_yaw=head;m_turning=false;m_returning=true;m_body_yaw_at=now;return m_native_yaw;
        }
        const auto speed=ground_speed(now);
        const bool walking=speed && *speed>TURN_WALKING*m_scale;
        // Following the head every frame (walking, threshold 0) snaps to it, except straight
        // after melee, when the body turns back at TURN_CATCH_UP first.
        const bool follow=m_settings.turn_threshold_degrees<=0 || walking;
        if(!m_body_yaw || (follow && !m_returning) || now<m_body_yaw_at) {m_body_yaw=m_head_yaw=head;m_turning=m_returning=false;m_body_yaw_at=now;return head;}
        const float dt=std::min(std::chrono::duration<float>(now-m_body_yaw_at).count(),.1f);m_body_yaw_at=now;
        const float jump=std::remainder(head-m_head_yaw,360.f);m_head_yaw=head;
        if(std::abs(jump)>TURN_SNAP) *m_body_yaw=std::remainder(*m_body_yaw+jump,360.f);
        const float error=std::remainder(head-*m_body_yaw,360.f);
        if(follow || std::abs(error)>m_settings.turn_threshold_degrees) m_turning=true;
        if(m_turning) {
            const float step=TURN_CATCH_UP*dt;
            if(std::abs(error)<=std::max(step,TURN_SETTLED)) {m_body_yaw=head;m_turning=m_returning=false;}
            else *m_body_yaw=std::remainder(*m_body_yaw+std::copysign(step,error),360.f);
        }
        else m_returning=false; // Standing within the threshold: the native facing is kept.
        return *m_body_yaw;
    }
    // riding: the jump pack (Settings::ride_speed_metres_per_second) took the Marine off from this
    // takeover; the eye rides on it at offset (the first-person eye's place over the root at entry,
    // world units, horizontal), at the standing eye height, joining from where the view was through
    // lag (decaying with RIDE_BLEND_SECONDS from lag_at). rode: it did, so the return to first
    // person blends too. can_ride: the takeover came from first person (not after a cutscene).
    // The settle rule (SETTLE_RADIUS, SETTLE_HEIGHT, SETTLE_TIME, INSIDE_LIMIT) for a takeover's end
    // and an early return's.
    struct Settle {Clock::time_point inside_since{},settled_since{};};
    // steered_since: the Marine has moved along the stick since then (Settings::release_speed_metres_per_second).
    struct Takeover {Vector eye{},offset{},lag{};bool head_near{true},can_ride{},riding{},rode{};Settle settle;Clock::time_point slow_since{},lag_at{},steered_since{};};
    // First person after an early return, while the game's camera is still out. animated_since: something
    // other than the stick has moved the Marine since then.
    struct Release {Clock::time_point since{};Settle settle;Clock::time_point animated_since{};};
    // The story cinematic moves the player's body out of the scene (24 Oct: 530 units below it),
    // so the spot comes from the last first-person frame, which may predate a short flat-panel
    // hold, never from the body. Without one in CUTSCENE_SPOT_AGE (a cutscene right after a load)
    // the native camera stays.
    struct Cutscene {Vector eye{};vr_camera::Matrix basis{};};
    static constexpr auto CUTSCENE_SPOT_AGE=std::chrono::seconds(30);
    // The last first-person spot; unlike m_eye it survives identify(), since ownership can change
    // as a sequence starts.
    struct Spot {Vector eye{};vr_camera::Matrix basis{};Clock::time_point at{};};
    void begin_cutscene(Clock::time_point now) {
        if(!m_spot || now<m_spot->at || now-m_spot->at>CUTSCENE_SPOT_AGE) return;
        m_cutscene=Cutscene{m_spot->eye,m_spot->basis};if(m_cutscene_holds!=UINT64_MAX)++m_cutscene_holds;
    }
    bool begin_takeover(const vr_camera::Matrix& game,const vr_camera::Matrix& head,uint64_t reference,float scale,Clock::time_point now) {
        // From first person the stable basis is the player's heading. Without one (an animation
        // straight after a cutscene or load, or after a recenter) start from the native camera's
        // level heading, which is what the view already shows; the return then rebases as usual.
        if(!(m_state.anchored || m_keep_placement) || m_reference!=reference || m_scale!=scale) {
            vr_camera::Matrix basis{};
            if(!reference || !vr_camera::level_anchor(game,basis,false)) return false;
            m_basis=basis;m_reference=reference;m_scale=scale;m_keep_placement=false;m_resume=true;
        }
        // The spot first person stood on last frame, or (no first person) the character's own.
        // Its height is the standing eye height over the root at entry, so the game camera's
        // rise in the moment before it leaves the envelope is not kept either.
        Takeover t;
        const bool from_body=m_state.anchored && fresh(m_eye_received,now) && !m_room_rebase;
        // On the character's own spot the eye is on the root, however far the head has walked.
        const auto walked=from_body || !m_settings.room_on()?Vector{}:room_head(head,scale);
        t.eye={from_body?m_eye[0]:m_position[0]-walked[0],m_position[1]+standing_units(scale),from_body?m_eye[2]:m_position[2]-walked[2]};
        // Where the eye sat over the root in that frame (the root has moved on since).
        if(from_body) t.offset={m_eye[0]-m_eye_root[0],0,m_eye[2]-m_eye_root[2]};t.can_ride=from_body;
        suspend(CameraMode::takeover);
        m_takeover=t;++m_state.takeovers;m_motion.reset(now);
        return true;
    }
    bool combo_recent(Clock::time_point now) const {return now-m_strike_at<=std::chrono::duration<float>(m_settings.combo_gap_seconds);}
    void clear_combo() {m_strikes=0;m_strike_pressed=false;m_combo_camera=false;m_state.combo_camera=false;}
    // This frame's camera is past the envelope because of a combo (Settings::combo_camera_radius_metres):
    // from when it leaves the envelope within combo_gap_seconds of the combo's latest strike until it is
    // back, past the wider limit or combo_camera_seconds on. All that time the facing stays the game's,
    // as in the hold it replaces: writing the head's would bend the lunge, which runs past
    // MELEE_FACING_HOLD (a finisher's last strike moved the Marine for 2.8 s).
    bool combo_camera(uint64_t reference,float scale,Clock::time_point now) {
        const float limit=m_settings.combo_camera_radius_metres;
        const bool out=m_state.anchored && m_reference==reference && m_scale==scale && limit>0 &&
            m_state.camera_radius>m_settings.camera_radius_metres && m_state.camera_radius<=limit;
        if(!out || (m_combo_camera && now-m_combo_from>=std::chrono::duration<float>(m_settings.combo_camera_seconds))) m_combo_camera=false;
        else if(!m_combo_camera && m_strikes>=m_settings.combo_strikes && combo_recent(now)) {
            m_combo_camera=true;m_combo_from=now;if(m_state.combos!=UINT64_MAX)++m_state.combos;
        }
        if(m_combo_camera) m_melee_until=std::max(m_melee_until,now+MELEE_FACING_HOLD);
        return m_state.combo_camera=m_combo_camera;
    }
    bool takeover_continues(bool outside,Clock::time_point now) {return !settle_done(m_takeover->settle,outside,now);}
    bool settle_done(Settle& s,bool outside,Clock::time_point now) {
        if(outside) {s.inside_since=s.settled_since={};return false;}
        if(s.inside_since==Clock::time_point{}) s.inside_since=now;
        const bool settled=m_state.camera_radius<=SETTLE_RADIUS && std::abs(m_state.camera_height-standing_units(m_scale)/m_scale)<=SETTLE_HEIGHT;
        if(!settled) s.settled_since={};
        else if(s.settled_since==Clock::time_point{}) s.settled_since=now;
        return (s.settled_since!=Clock::time_point{} && now-s.settled_since>=SETTLE_TIME) || now-s.inside_since>=INSIDE_LIMIT;
    }
    // How the followed body moves against the stick the game is sent: its horizontal speed
    // (physical m/s, over TRAVEL_WINDOW) and the cosine between its direction and the stick's,
    // which the game reads against its own camera (x along the camera's right (-fz,fx), y along its
    // level forward (fx,fz)); along is -2 with the stick neutral (under RELEASE_STICK) or stale.
    struct Drive {float speed{},along{-2};};
    Drive drive(const vr_camera::Matrix& game,Clock::time_point now) const {
        Drive d;const auto v=travel_velocity(now);
        if(!v) return d;
        d.speed=std::hypot((*v)[0],(*v)[2])/m_scale;
        if(!(d.speed<=RIDE_TELEPORT)) {d.speed=0;return d;}
        const float f=std::hypot(game[8],game[10]),x=m_stick[0],y=m_stick[1];
        if(!fresh(m_stick_at,now) || std::hypot(x,y)<RELEASE_STICK || !(f>1e-4f) || !(d.speed>0)) return d;
        const float fx=game[8]/f,fz=game[10]/f,dx=-fz*x+fx*y,dz=fx*x+fz*y;
        d.along=((*v)[0]*dx+(*v)[2]*dz)/(std::hypot(dx,dz)*d.speed*m_scale);
        return d;
    }
    // The early return: a held takeover from first person whose Marine has moved at release speed
    // along the stick for release_seconds. Never while riding, or with no first-person placement to
    // return to (after a cutscene or a load the full handover would sample the far camera).
    bool released(const vr_camera::Matrix& game,Clock::time_point now) {
        auto& t=*m_takeover;const float speed=m_settings.release_speed_metres_per_second;
        const auto d=drive(game,now);
        if(speed<=0 || t.riding || !t.can_ride || !m_keep_placement || d.speed<speed || d.along<RELEASE_ALONG) {t.steered_since={};return false;}
        if(t.steered_since==Clock::time_point{}) t.steered_since=now;
        return now-t.steered_since>=std::chrono::duration<float>(m_settings.release_seconds);
    }
    // The animation has run and settled: the bones moved, then stayed under STILL_SPEED for
    // execution_still_seconds. Same conditions as the stick's return (not riding, a first-person spot).
    bool settled(Clock::time_point now) const {
        const auto& t=*m_takeover;const float seconds=m_settings.execution_still_seconds;
        if(seconds<=0 || t.riding || !t.can_ride || !m_keep_placement || !m_motion.moved()) return false;
        const auto still=m_motion.still_for(now);
        return still && *still>=std::chrono::duration<float>(seconds);
    }
    // After an early return: the Marine moved at ANIMATED_SPEED not along the stick (within
    // ANIMATED_ALONG, or with it neutral) for ANIMATED_TIME, so an animation drives it again.
    // Locomotion's own lag (a turn, stopping) stays under that.
    bool animated(const vr_camera::Matrix& game,Clock::time_point now) {
        auto& r=*m_release;const auto d=drive(game,now);
        if(d.speed<ANIMATED_SPEED || d.along>=ANIMATED_ALONG) {r.animated_since={};return false;}
        if(r.animated_since==Clock::time_point{}) r.animated_since=now;
        return now-r.animated_since>=ANIMATED_TIME;
    }
    // Held: stable heading (no forced yaw, no roll) at the entry spot and height, fixed in the world
    // whatever the character's root or the game's camera do; only the real head moves the view.
    // Riding: the same heading, the eye carried by the Marine.
    vr_camera::Matrix takeover_anchor(const vr_camera::Matrix& game,const vr_camera::Matrix& head,Clock::time_point now) {
        auto& t=*m_takeover;
        // The view turns with m_basis as in first person: the stick maps against the game's camera the same way.
        m_movement_heading_offset=std::remainder(std::atan2(game[8],game[10])-std::atan2(m_basis[8],m_basis[10]),6.28318530718f);m_takeover_heading_at=now;
        ride(t,now);
        auto anchor=m_basis;
        if(t.riding) {
            const float weight=std::exp(-std::chrono::duration<float>(now-t.lag_at).count()/RIDE_BLEND_SECONDS);
            const Vector at{m_position[0]+t.offset[0],m_position[1]+standing_units(m_scale),m_position[2]+t.offset[2]};
            for(int j=0;j<3;++j) anchor[12+j]=at[j]+t.lag[j]*weight;
            // The eye is the Marine's: its head stays hidden, however far the flight pose moves the bone.
            t.head_near=true;m_state.show_head=false;
            mode(CameraMode::ride);
            m_last_eye={anchor[12],anchor[13],anchor[14]};m_last_eye_at=now;
            return anchor;
        }
        for(int j=0;j<3;++j) anchor[12+j]=t.eye[j];
        // The head is drawn once the eye is clear of it, hidden again only when it comes back.
        vr_camera::Matrix eye{},view{};
        if(vr_camera::compose(anchor,head,m_scale,eye,view)) {
            const Vector bone=fresh(m_head_bone_received,now)?m_head_bone:Vector{m_position[0],m_position[1]+standing_units(m_scale),m_position[2]};
            const float distance=std::hypot(eye[12]-bone[0],eye[13]-bone[1],eye[14]-bone[2])/m_scale;
            t.head_near=distance<m_settings.head_clear_metres-(t.head_near?0:HEAD_CLEAR_RETURN);
        }
        m_state.show_head=!t.head_near;
        mode(CameraMode::takeover);
        m_last_eye=t.eye;m_last_eye_at=now;
        return anchor;
    }
    // The followed body's speed this frame (Snapshot::travel_speed, physical m/s) and its rising
    // speed (horizontal, plus vertical only upwards), from travel_velocity().
    void travel(float scale,Clock::time_point now) {
        const auto velocity=travel_velocity(now);
        const float across=velocity?std::hypot((*velocity)[0],(*velocity)[2]):0,up=velocity?(*velocity)[1]:0;
        m_travel=std::hypot(across,up)/scale;m_rising=std::hypot(across,std::max(up,0.f))/scale;
        // A teleport (a respawn, a class change: 200-450 m/s in the 26 Oct trace) is not travel.
        if(!(m_travel<=RIDE_TELEPORT)) m_travel=m_rising=0;
        m_state.travel_speed=m_travel;
    }
    // Starts and ends the ride inside a takeover. It starts once the Marine rises or crosses at
    // ride_speed; the held view catches up (no cut up to RIDE_BLEND_LIMIT). It ends RIDE_SETTLE after
    // the speed drops below RIDE_RELEASE of it (the leap's apex stays inside), and the view holds
    // where the eye was (a landing under the still pulled-back camera) until the takeover ends.
    void ride(Takeover& t,Clock::time_point now) {
        const float ride=m_settings.ride_speed_metres_per_second;
        if(!t.riding) {
            if(!m_settings.ride_enabled || !t.can_ride || m_rising<ride) return;
            const Vector at{m_position[0]+t.offset[0],m_position[1]+standing_units(m_scale),m_position[2]+t.offset[2]};
            const Vector lag{t.eye[0]-at[0],t.eye[1]-at[1],t.eye[2]-at[2]};
            t.lag=std::hypot(lag[0],lag[1],lag[2])<=RIDE_BLEND_LIMIT*m_scale?lag:Vector{};
            t.riding=t.rode=true;t.lag_at=now;t.slow_since={};
            if(m_state.rides!=UINT64_MAX) ++m_state.rides;
        }
        else if(!m_settings.ride_enabled) end_ride(t);
        else if(m_travel>=ride*RIDE_RELEASE) t.slow_since={};
        else if(t.slow_since==Clock::time_point{}) t.slow_since=now;
        else if(now-t.slow_since>=RIDE_SETTLE) end_ride(t);
        m_state.riding=t.riding;
    }
    // The hold takes over from where the riding eye was (no cut).
    void end_ride(Takeover& t) {if(m_last_eye_at!=Clock::time_point{}) t.eye=m_last_eye;t.riding=false;t.slow_since={};m_state.riding=false;}
    // Joins the previous eye (a takeover that rode) to this first-person anchor without a cut: the
    // offset between them decays with RIDE_BLEND_SECONDS. Over RIDE_BLEND_LIMIT it cuts.
    void blend(vr_camera::Matrix& anchor,float scale,Clock::time_point now) {
        if(m_blend_pending) {
            m_blend_pending=false;
            if(fresh(m_last_eye_at,now)) {
                const Vector offset{m_last_eye[0]-anchor[12],m_last_eye[1]-anchor[13],m_last_eye[2]-anchor[14]};
                const float length=std::hypot(offset[0],offset[1],offset[2]);
                if(length>1e-4f*scale && length<=RIDE_BLEND_LIMIT*scale) {m_blend=offset;m_blend_at=now;}
            }
        }
        if(m_blend_at==Clock::time_point{} || now<m_blend_at) return;
        const float weight=std::exp(-std::chrono::duration<float>(now-m_blend_at).count()/RIDE_BLEND_SECONDS);
        if(weight<.01f) {m_blend_at={};return;}
        for(int j=0;j<3;++j) anchor[12+j]+=m_blend[j]*weight;
    }
    // The comfort vignette's strength (Snapshot::vignette): only while riding.
    void comfort(Clock::time_point now) {
        const float ride=m_settings.ride_speed_metres_per_second;
        const float target=m_state.camera_mode==CameraMode::ride?
            m_settings.ride_vignette*std::clamp((m_travel-RIDE_VIGNETTE_FROM*ride)/((1-RIDE_VIGNETTE_FROM)*ride),0.f,1.f):0.f;
        const float dt=m_vignette_at==Clock::time_point{} || now<m_vignette_at?0.f:std::min(std::chrono::duration<float>(now-m_vignette_at).count(),.1f);
        m_vignette_at=now;
        auto& v=m_state.vignette;
        v+=(target-v)*(1-std::exp(-dt/(target>v?VIGNETTE_ATTACK:VIGNETTE_RELEASE)));
        if(target==0 && v<1e-3f) v=0;
    }
    float standing_units(float scale) const {return m_standing?*m_standing:m_settings.standing_height_metres*scale;}
    // The first-person native camera's height above the root, averaged over a few seconds of
    // settled gameplay (its own bob averages out), so a takeover keeps the familiar eye height.
    void learn_standing(float height,float scale,Clock::time_point now) {
        if(!std::isfinite(height) || m_state.camera_radius>SETTLE_RADIUS || std::abs(height/scale-m_settings.standing_height_metres)>1) return;
        if(!m_standing || m_standing_scale!=scale) {m_standing=height;m_standing_scale=scale;}
        else {
            const float dt=std::clamp(std::chrono::duration<float>(now-m_standing_at).count(),0.f,.5f);
            *m_standing+=(height-*m_standing)*(1-std::exp(-dt/STANDING_SECONDS));
        }
        m_standing_at=now;m_state.standing_height=*m_standing;
    }
    void mode(CameraMode value) {if(m_state.camera_mode!=value) {m_state.camera_mode=value;++m_state.camera_transitions;}}
    void suspend(CameraMode value) {
        if(value!=CameraMode::takeover) m_takeover.reset();
        m_release.reset();m_state.released=false;m_combo_camera=false;m_state.combo_camera=false;
        // Any other view ends a ride and a handback blend.
        m_state.riding=false;m_blend_pending=false;m_blend_at={};
        m_state.height_lead=0;
        // A native camera shows the whole character; unknown authority keeps what was there.
        if(value==CameraMode::external || value==CameraMode::scripted) m_state.show_head=true;
        const bool held=value==CameraMode::external || value==CameraMode::takeover;
        m_keep_placement=held && (m_state.anchored || m_keep_placement);
        m_resume=m_resume || m_state.anchored || held || value==CameraMode::scripted;
        // consumed stays: the head is still that far from where it started (native(), room_rebase()).
        m_state.anchored=false;m_state.requested={};m_state.room_error=0;m_state.room_blocked=false;m_room_walking=false;
        m_pending.reset();m_head_received={};m_body_yaw.reset();mode(value);
    }
    static bool finite(const Vector& p) { return std::all_of(p.begin(),p.end(),[](float v){return std::isfinite(v)&&std::abs(v)<1000000;}); }
    static bool fresh(Clock::time_point received,Clock::time_point now) { return received != Clock::time_point{} && now>=received && now-received<=std::chrono::milliseconds(100); }
    // Room steps not yet seen in the body's position: since start, the game's own displacement
    // (native) and the room's still open (step); last: the latest step's length; idle: positions
    // since a request.
    struct Pending { Vector start{},step{},native{};uint64_t body_updates{};float last{};int idle{}; };
    Settings m_settings;
    Snapshot m_state;
    bool m_allowed{},m_resume{},m_keep_placement{};
    uint64_t m_reference{};
    float m_scale{1},m_movement_heading_offset{};Clock::time_point m_takeover_heading_at{};
    Vector m_position{},m_camera_offset{},m_physical{};
    Vector m_speed_position{}; Clock::time_point m_speed_from{}; std::optional<float> m_ground_speed,m_vertical_speed;
    std::array<std::pair<Clock::time_point,Vector>,16> m_track{};size_t m_track_count{},m_track_next{}; // travel_velocity()
    vr_camera::Matrix m_basis{vr_camera::IDENTITY};
    Clock::time_point m_head_received{},m_body_received{};
    Clock::time_point m_dodge_until{};bool m_dodge_pressed{};
    Clock::time_point m_melee_until{},m_melee_from{};bool m_melee_pressed{};
    // The combo so far (Settings::combo_strikes) and its view past the envelope.
    float m_strikes{};Clock::time_point m_strike_at{},m_combo_from{};bool m_strike_pressed{},m_combo_camera{};
    float m_native_yaw{};Clock::time_point m_native_yaw_received{};
    Vector m_eye{},m_eye_root{},m_head_bone{};Clock::time_point m_eye_received{},m_head_bone_received{};
    std::optional<Pending> m_pending;
    // Walking in the play space: the last step's velocities (room_feedback), the reference the
    // room started on, the blocked window's distances, and whether the catch-up is running. m_room_last
    // outlives everything but the next game step: that step still has to take it off its input.
    Vector m_room_last{},m_native_last{};uint64_t m_room_reference{},m_body_updates{};bool m_room_walking{},m_room_rebase{};
    float m_room_asked{},m_room_moved{};Clock::time_point m_room_window_from{};
    std::optional<Takeover> m_takeover;
    std::optional<Release> m_release;vr_viewer::Motion m_motion;std::array<float,2> m_stick{};Clock::time_point m_stick_at{};
    std::optional<float> m_body_yaw;float m_head_yaw{};bool m_turning{},m_returning{};Clock::time_point m_body_yaw_at{};
    std::optional<Cutscene> m_cutscene;std::optional<Spot> m_spot;uint64_t m_cutscene_holds{};
    std::optional<float> m_standing;float m_standing_scale{};Clock::time_point m_standing_at{};
    // Riding along: the handback blend, this frame's travel and rising speeds (physical m/s), the last eye shown
    // (anchor origin, world units) and the vignette's clock.
    bool m_blend_pending{};float m_travel{},m_rising{};Clock::time_point m_blend_at{},m_last_eye_at{},m_vignette_at{};
    Vector m_blend{},m_last_eye{};
};
