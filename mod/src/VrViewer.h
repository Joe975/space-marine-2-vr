#pragma once
#include "VrBodyFrame.h"
#include "VrCameraPose.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

// Viewer mode: the game sits frozen in its native photo mode while the headset flies
// freely, detached from the character. See docs/VIEWER_MODE.md.
namespace vr_viewer {
using Matrix=vr_camera::Matrix;
using Vec=std::array<float,3>;

// Room points are XR metres after recenter and rig (x right, y up, z back). An anchor and a
// scale place the room in the game world exactly as vr_camera::compose does:
// world(p) = anchor.pos + units_per_metre*(p.x*right + p.y*up - p.z*forward).
inline Vec direction(const Matrix& anchor,const Vec& p) {
    Vec out{};
    for(int j=0;j<3;++j) out[j]=p[0]*anchor[j]+p[1]*anchor[4+j]-p[2]*anchor[8+j];
    return out;
}
inline Vec world(const Matrix& anchor,float units_per_metre,const Vec& p) {
    const auto d=direction(anchor,p);
    return {anchor[12]+units_per_metre*d[0],anchor[13]+units_per_metre*d[1],anchor[14]+units_per_metre*d[2]};
}
inline void place(Matrix& anchor,float units_per_metre,const Vec& room,const Vec& target) {
    const auto d=direction(anchor,room);
    for(int j=0;j<3;++j) anchor[12+j]=target[j]-units_per_metre*d[j];
}
// Room yaw about +y; a positive angle turns +x toward -z (a right turn seen from above).
inline Vec yaw(const Vec& v,float angle) {
    const float c=std::cos(angle),s=std::sin(angle);
    return {v[0]*c+v[2]*s,v[1],-v[0]*s+v[2]*c};
}
inline float bearing(const Vec& v) { return std::atan2(-v[2],v[0]); }
// The anchor whose mapping of u equals the old mapping of yaw(u,-angle): the world appears
// turned by +angle in the room. Rows rotate together, so the native handedness is kept.
inline Matrix turned(const Matrix& anchor,float angle) {
    const float c=std::cos(angle),s=std::sin(angle);
    auto out=anchor;
    for(int j=0;j<3;++j) {out[j]=c*anchor[j]-s*anchor[8+j];out[8+j]=s*anchor[j]+c*anchor[8+j];}
    return out;
}
inline Vec sub(const Vec& a,const Vec& b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline Vec mid(const Vec& a,const Vec& b) {return {(a[0]+b[0])*.5f,(a[1]+b[1])*.5f,(a[2]+b[2])*.5f};}
inline float length(const Vec& v) {return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);}
inline Vec translation(const Matrix& m) {return {m[12],m[13],m[14]};}

// First person draws each eye as compose(first_person, relative_eye + offset), where the
// offset is the calibration's height and head-forward displacement. Fold that offset into a
// level anchor so the first viewer frame matches the last first-person frame exactly.
inline std::optional<Matrix> entry_anchor(const Matrix& first_person,const Matrix& relative_head,
    float height_offset,float forward_offset,float units_per_metre) {
    Matrix anchor{},heading{};
    if(!vr_camera::level_anchor(first_person,anchor,false) || !vr_camera::level_anchor(relative_head,heading)) return {};
    if(!std::isfinite(units_per_metre) || units_per_metre<0.01f || units_per_metre>100) return {};
    const Vec offset{-forward_offset*heading[8],height_offset,-forward_offset*heading[10]};
    const auto origin=world(anchor,units_per_metre,offset);
    for(int j=0;j<3;++j) anchor[12+j]=origin[j];
    return anchor;
}
} // namespace vr_viewer

// The viewer's room-to-world placement: a level anchor plus a scale. Sticks slide and snap
// turn; one grip drags the world with that hand; both grips scale and yaw the world about
// the hands' midpoint. Pure math on room-space inputs; deterministic.
class VrViewerRig {
public:
    using Matrix=vr_viewer::Matrix;using Vec=vr_viewer::Vec;
    static constexpr float PRESS=.65f,RELEASE=.35f;
    struct Settings {
        float slide_metres_per_second{2.5f},vertical_metres_per_second{1.5f};
        // World scale limits in game units per physical metre (life size is about 0.9). The eyes
        // keep the native 0.1-unit near plane, so a magnified world clips closer than
        // 0.1/minimum metres: 0.2 keeps that at half a metre.
        float minimum_units_per_metre{.2f},maximum_units_per_metre{50};
        // Scale presets as multiples of life size (the first-person scale at entry): life, a
        // 1:12 action-figure scene, and a world four times larger. A preset stands the player on
        // the world's floor at their real eye height (grounded), so the stereo baseline, head
        // motion and eye height above the ground all agree on one scale (docs/VIEWER_MODE.md).
        std::array<float,3> presets{1,12,.25f};
        float preset_pivot_forward_metres{.8f}; // presets scale about the floor this far ahead of the head
        // Entry: step out of the character's head to stand beside it, life size, grounded.
        float entry_side_metres{1},entry_back_metres{.5f};
    };
    // The floor to stand on: the character's ground at entry (world units) and the physical
    // floor in room space (metres; minus the reference head height above the floor).
    struct Ground {float world_y{},room_y{};};
    struct Input {
        Matrix head{vr_camera::IDENTITY};   // room space
        std::array<std::optional<Vec>,2> hands; // left, right controller origins, room space
        float left_grip{},right_grip{};
        float move_x{},move_y{},turn_x{},turn_y{};
        float snap_degrees{30};
        float preset{}; // right trigger: a press cycles the scale presets
    };
    enum class Grab {none,left,right,both};
    // preset: index into Settings::presets, -1 after a free two-hand scale. grounded: the world
    // floor is held at the physical floor (entry and presets set it; flying up/down clears it).
    struct Snapshot {bool engaged{};Matrix anchor{vr_camera::IDENTITY};float units_per_metre{1};Grab grab{Grab::none};uint64_t grabs{},snaps{};
        int preset{};bool grounded{};float life_units_per_metre{1};uint64_t presets{};};
    explicit VrViewerRig(Settings settings={}):m_settings(settings) {}
    // With a ground, the first update steps beside the character onto its floor at life size;
    // without one the view starts exactly where first person left off.
    bool begin(const Matrix& anchor,float units_per_metre,std::optional<Ground> ground={}) {
        Matrix level{};
        if(!vr_camera::level_anchor(anchor,level,false) || !std::isfinite(units_per_metre)) return false;
        if(ground && !(std::isfinite(ground->world_y) && std::isfinite(ground->room_y) && ground->room_y<=-.3f && ground->room_y>=-2.5f)) ground.reset();
        const float life=std::clamp(units_per_metre,m_settings.minimum_units_per_metre,m_settings.maximum_units_per_metre);
        m_state=Snapshot{true,level,life,Grab::none,m_state.grabs,m_state.snaps,0,false,life,m_state.presets};
        m_ground=ground;m_entry_pending=ground.has_value();
        m_held={};m_snap_ready=false;m_preset_ready=false;return true;
    }
    void end() {m_state.engaged=false;m_state.grab=Grab::none;m_held={};}
    bool engaged() const {return m_state.engaged;}
    Snapshot snapshot() const {return m_state;}
    void update(const Input& in,float seconds) {
        if(!m_state.engaged || !std::isfinite(seconds)) return;
        seconds=std::clamp(seconds,0.f,.1f);
        if(m_entry_pending) {m_entry_pending=false;step_beside(in.head);}
        for(int i=0;i<2;++i) {
            const float grip=i?in.right_grip:in.left_grip;
            if(!in.hands[i] || !std::isfinite(grip) || grip<=RELEASE) m_held[i]=false;
            else if(grip>=PRESS) m_held[i]=true;
        }
        const auto grab=m_held[0] && m_held[1]?Grab::both:m_held[0]?Grab::left:m_held[1]?Grab::right:Grab::none;
        if(grab!=m_state.grab) capture(grab,in);
        switch(grab) {
        case Grab::both: two_hands(*in.hands[0],*in.hands[1]);m_state.preset=-1;break;
        case Grab::left: case Grab::right: {
            const auto& hand=*in.hands[grab==Grab::left?0:1];
            vr_viewer::place(m_state.anchor,m_state.units_per_metre,hand,m_world);break;}
        case Grab::none: sticks(in,seconds);break;
        }
        // Grounded, the floor stays under the player's feet: grabs move and scale the world
        // horizontally about the hands, never lift it.
        if(m_state.grounded) ground();
        if(!std::isfinite(in.preset) || in.preset<=RELEASE) m_preset_ready=true;
        else if(m_preset_ready && in.preset>=PRESS && grab==Grab::none) {
            m_preset_ready=false;
            apply_preset(m_state.preset<0?0:(m_state.preset+1)%int(m_settings.presets.size()),in.head);
        }
    }
    // Index into Settings::presets (the right trigger cycles them).
    bool apply_preset(int index,const Matrix& head) {
        if(!m_state.engaged || index<0 || index>=int(m_settings.presets.size())) return false;
        Matrix heading{};
        if(!vr_camera::level_anchor(head,heading)) return false;
        const float scale=std::clamp(m_state.life_units_per_metre*m_settings.presets[index],m_settings.minimum_units_per_metre,m_settings.maximum_units_per_metre);
        // Pivot on the floor just ahead of the player, where they are looking: what is there
        // stays there while everything else grows or shrinks around it.
        const auto p=vr_viewer::translation(head);
        const float ahead=m_settings.preset_pivot_forward_metres;
        const Vec pivot{p[0]-ahead*heading[8],m_ground?m_ground->room_y:p[1],p[2]-ahead*heading[10]};
        const auto target=vr_viewer::world(m_state.anchor,m_state.units_per_metre,pivot);
        vr_viewer::place(m_state.anchor,scale,pivot,target);
        m_state.units_per_metre=scale;m_state.preset=index;++m_state.presets;
        if(m_ground) {m_state.grounded=true;ground();}
        return true;
    }
private:
    void capture(Grab grab,const Input& in) {
        m_state.grab=grab;m_grab_anchor=m_state.anchor;m_grab_scale=m_state.units_per_metre;
        if(grab!=Grab::none) ++m_state.grabs;
        if(grab==Grab::both) {
            const auto span=vr_viewer::sub(*in.hands[1],*in.hands[0]);
            m_grab_distance=vr_viewer::length(span);m_grab_bearing=vr_viewer::bearing(span);m_turn=0;
            m_world=vr_viewer::world(m_state.anchor,m_state.units_per_metre,vr_viewer::mid(*in.hands[0],*in.hands[1]));
        } else if(grab!=Grab::none) {
            m_world=vr_viewer::world(m_state.anchor,m_state.units_per_metre,*in.hands[grab==Grab::left?0:1]);
        }
    }
    // The world point under the hands' midpoint at the grab stays under the midpoint. Hands
    // apart make the world bigger (fewer game units per metre); turning the hand-to-hand line
    // turns the world with it about the vertical axis.
    void two_hands(const Vec& left,const Vec& right) {
        const auto span=vr_viewer::sub(right,left);const float distance=vr_viewer::length(span);
        float scale=m_grab_scale;
        if(m_grab_distance>=.05f && distance>=.01f) scale=m_grab_scale*m_grab_distance/distance;
        scale=std::clamp(scale,m_settings.minimum_units_per_metre,m_settings.maximum_units_per_metre);
        // A near-vertical hand line has no reliable bearing; keep the last turn.
        if(std::hypot(span[0],span[2])>=.05f && m_grab_distance>=.05f) m_turn=vr_viewer::bearing(span)-m_grab_bearing;
        m_state.anchor=vr_viewer::turned(m_grab_anchor,m_turn);m_state.units_per_metre=scale;
        vr_viewer::place(m_state.anchor,scale,vr_viewer::mid(left,right),m_world);
    }
    void sticks(const Input& in,float seconds) {
        Matrix heading{};
        if(!vr_camera::level_anchor(in.head,heading)) return;
        const Vec forward{-heading[8],0,-heading[10]},right{heading[0],0,heading[2]};
        Vec step{};
        const float magnitude=std::hypot(in.move_x,in.move_y);
        if(std::isfinite(magnitude) && magnitude>.2f) {
            const float amount=(std::min(magnitude,1.f)-.2f)/.8f*m_settings.slide_metres_per_second*seconds/magnitude;
            for(int j=0;j<3;++j) step[j]=(right[j]*in.move_x+forward[j]*in.move_y)*amount;
        }
        if(std::isfinite(in.turn_y) && std::abs(in.turn_y)>.3f)
            {step[1]+=std::copysign((std::min(std::abs(in.turn_y),1.f)-.3f)/.7f,in.turn_y)*m_settings.vertical_metres_per_second*seconds;m_state.grounded=false;}
        const auto moved=vr_viewer::direction(m_state.anchor,step);
        for(int j=0;j<3;++j) m_state.anchor[12+j]+=m_state.units_per_metre*moved[j];
        if(!std::isfinite(in.turn_x) || std::abs(in.turn_x)<=.3f) m_snap_ready=true;
        else if(m_snap_ready && std::abs(in.turn_x)>=.7f && std::isfinite(in.snap_degrees)) {
            // Turn about the head, so the head keeps its place in the world.
            const auto pivot=vr_viewer::translation(in.head);const auto target=vr_viewer::world(m_state.anchor,m_state.units_per_metre,pivot);
            auto anchor=vr_viewer::turned(m_state.anchor,std::copysign(in.snap_degrees*.01745329252f,in.turn_x));
            Matrix level{};if(vr_camera::level_anchor(anchor,level,false)) anchor=level;
            vr_viewer::place(anchor,m_state.units_per_metre,pivot,target);
            m_state.anchor=anchor;m_snap_ready=false;++m_state.snaps;
        }
    }
    // Hold the world floor at the physical floor: anchor.y + scale*room_y == world_y.
    void ground() {
        if(!m_ground) return;
        m_state.anchor[13]=m_ground->world_y-m_state.units_per_metre*vr_viewer::direction(m_state.anchor,{0,m_ground->room_y,0})[1];
    }
    // Entry: the character stands where the head was. Stand entry_side_metres to its right and
    // entry_back_metres behind (room axes of the level head), life size, on its floor.
    void step_beside(const Matrix& head) {
        Matrix heading{};
        if(!m_ground || !vr_camera::level_anchor(head,heading)) return;
        const auto p=vr_viewer::translation(head);
        const float side=m_settings.entry_side_metres,back=m_settings.entry_back_metres;
        const Vec beside{p[0]+side*heading[0]+back*heading[8],p[1],p[2]+side*heading[2]+back*heading[10]};
        vr_viewer::place(m_state.anchor,m_state.units_per_metre,p,vr_viewer::world(m_state.anchor,m_state.units_per_metre,beside));
        m_state.grounded=true;m_state.preset=0;ground();
    }
    Settings m_settings;
    Snapshot m_state;
    std::optional<Ground> m_ground;
    bool m_entry_pending{},m_preset_ready{};
    std::array<bool,2> m_held{};
    bool m_snap_ready{};
    Matrix m_grab_anchor{vr_camera::IDENTITY};
    float m_grab_scale{1},m_grab_distance{},m_grab_bearing{},m_turn{};
    Vec m_world{};
};

// Drives the native photo mode with ordinary gamepad presses, watching the published
// native flags: pause menu 0x908, photo mode 0x108 (live probe, docs/VIEWER_MODE.md).
// Enter: release the head first (head hiding off until the game has published the pose twice
// more, so the freeze keeps a full-size head; the arms keep the player's pose unless the
// native pose was chosen, see vr_viewer::pose_edits), then Start, then hold X on the pause
// menu until photo mode shows. From an open pause menu the character is already frozen with
// the hidden head, so Start resumes it before the release.
// Leave: B back to the pause menu, then Start. Each press waits for the previous release.
// Play (Right A, docs/VIEWER_MODE.md "Watch it play out"): B leaves photo mode straight to
// gameplay while the free camera stays; the game runs with a neutral pad. A play that runs its
// course leaves through request() with nothing left to press, as Left B does. Right A again calls
// refreeze(), which pauses with Start and holds X on the pause menu, as on entry.
class VrViewerSequencer {
public:
    using Clock=std::chrono::steady_clock;
    // XInput button bits, kept here so the sequencer has no Windows dependency.
    static constexpr uint16_t START=0x0010,X=0x4000,B=0x2000;
    static constexpr uint32_t MENU=0x800,PHOTO=0x100,PAUSE_MENU=0x908,CINEMATIC=0x10;
    static constexpr auto PULSE=std::chrono::milliseconds(150),GAP=std::chrono::milliseconds(150),
        WAIT=std::chrono::milliseconds(3000),HOLD_LIMIT=std::chrono::milliseconds(3000),
        RELEASE_LIMIT=std::chrono::milliseconds(400);
    // play_*: the game runs (or is being let go or caught again) under the free camera.
    enum class State {off,resuming,releasing,opening_pause,opening_photo,active,closing_photo,closing_pause,
        play_closing_photo,play_closing_pause,playing,play_opening_pause,play_opening_photo};
    // playing: the game runs again under the free camera; refrozen: back in photo mode after a play.
    enum class Outcome {none,entered,exited,failed,playing,refrozen};
    struct Step {uint16_t buttons{};Clock::time_point until{};Outcome outcome{Outcome::none};};
    static bool photo(uint32_t flags) {return (flags&PHOTO) && !(flags&MENU);}
    static bool pause(uint32_t flags) {return (flags&PAUSE_MENU)==PAUSE_MENU;}
    static bool gameplay(uint32_t flags) {return !(flags&(MENU|PHOTO));}
    static const char* name(State state) {
        switch(state) {case State::off:return "off";case State::resuming:return "resuming";case State::releasing:return "releasing";
            case State::opening_pause:return "opening_pause";case State::opening_photo:return "opening_photo";
            case State::active:return "active";case State::closing_photo:return "closing_photo";case State::closing_pause:return "closing_pause";
            case State::play_closing_photo:return "play_closing_photo";case State::play_closing_pause:return "play_closing_pause";case State::playing:return "playing";
            case State::play_opening_pause:return "play_opening_pause";case State::play_opening_photo:return "play_opening_photo";}
        return "unknown";
    }
    // The free camera draws the eyes: photo mode, and every phase of a play.
    static bool rig_engaged(State state) {
        return state==State::active || state==State::play_closing_photo || state==State::play_closing_pause || state==State::playing ||
            state==State::play_opening_pause || state==State::play_opening_photo;
    }
    // The game runs (or is about to run, or is being caught again) under the free camera.
    static bool play_phase(State state) {return rig_engaged(state) && state!=State::active;}
    State state() const {return m_state;}
    uint64_t failures() const {return m_failures;}
    uint64_t plays() const {return m_plays;}
    uint64_t refreezes() const {return m_refreezes;}
    // When the game started running again (the playing outcome).
    Clock::time_point play_started() const {return m_play_started;}
    // Pauses that went ahead without seeing the character released (no native publication
    // within RELEASE_LIMIT); the freeze may then keep first-person edits.
    uint64_t unreleased() const {return m_unreleased;}
    // Toggle from gameplay or the pause menu (enter), or from active or playing (leave). Anything
    // in between is ignored so a second long press cannot interleave presses.
    bool request(std::optional<uint32_t> flags,Clock::time_point now) {
        if(!flags) return false;
        if(m_state==State::off) {
            // Not from the player's own photo mode, and not during a scripted shot.
            if(photo(*flags) || (*flags&CINEMATIC)) return false;
            if(pause(*flags)) start(State::resuming,START,now,PULSE);
            else if(gameplay(*flags)) release(now);
            else return false;
            return true;
        }
        if(m_state==State::active) {start(State::closing_photo,B,now,PULSE);return true;}
        // Playing, the game already runs: leaving needs no press.
        if(m_state==State::playing) {m_state=State::off;m_until=now;return true;}
        return false;
    }
    // From photo mode: let the game run under the free camera. B goes straight to gameplay
    // (live 20 Oct: 0x108 to 0x0 in 312 ms); a pause menu on the way is closed with Start.
    bool play(Clock::time_point now) {
        if(m_state!=State::active) return false;
        start(State::play_closing_photo,B,now,PULSE);if(m_plays!=UINT64_MAX)++m_plays;return true;
    }
    // While playing, on the player's Right A: freeze (Start, then X held on the pause menu, as on entry).
    bool refreeze(Clock::time_point now) {
        if(m_state!=State::playing) return false;
        start(State::play_opening_pause,START,now,PULSE);return true;
    }
    void cancel() {m_state=State::off;m_until={};}
    // character_native: the game has published the player pose twice since the viewer
    // switched head hiding off, so the head is full size again.
    Step update(std::optional<uint32_t> flags,bool foreground,Clock::time_point now,bool character_native=true) {
        Step step;
        if(m_state==State::off) return step;
        if(!flags || (!foreground && m_state!=State::active && m_state!=State::playing)) return fail(now);
        const auto value=*flags;const bool settled=now>=m_until+GAP;
        switch(m_state) {
        case State::off: break;
        case State::resuming:
            if(settled && gameplay(value)) release(now);
            else if(now>=m_deadline) return fail(now);
            break;
        case State::releasing:
            if(!gameplay(value)) return fail(now);
            if(character_native || now>=m_deadline) {
                if(!character_native) ++m_unreleased;
                start(State::opening_pause,START,now,PULSE);
            }
            break;
        case State::opening_pause:
            if(settled && pause(value)) start(State::opening_photo,X,now,HOLD_LIMIT);
            else if(now>=m_deadline) return fail(now);
            break;
        case State::opening_photo:
            if(photo(value)) {m_state=State::active;m_until=now;step.outcome=Outcome::entered;}
            else if(now>=m_deadline || (settled && gameplay(value))) return fail(now);
            break;
        case State::active:
            if(!photo(value)) {m_state=State::off;step.outcome=Outcome::exited;}
            break;
        case State::closing_photo:
            if(settled && gameplay(value)) {m_state=State::off;step.outcome=Outcome::exited;}
            else if(settled && pause(value)) start(State::closing_pause,START,now,PULSE);
            else if(now>=m_deadline) return fail(now);
            break;
        case State::closing_pause:
            if(settled && gameplay(value)) {m_state=State::off;step.outcome=Outcome::exited;}
            else if(now>=m_deadline) return fail(now);
            break;
        case State::play_closing_photo:
            if(settled && gameplay(value)) {start_playing(now);step.outcome=Outcome::playing;}
            else if(settled && pause(value)) start(State::play_closing_pause,START,now,PULSE);
            else if(now>=m_deadline) return fail(now);
            break;
        case State::play_closing_pause:
            if(settled && gameplay(value)) {start_playing(now);step.outcome=Outcome::playing;}
            else if(now>=m_deadline) return fail(now);
            break;
        case State::playing:
            // A cutscene, a menu or photo mode the game opened by itself owns the view now.
            if(!gameplay(value) || (value&CINEMATIC)) {m_state=State::off;step.outcome=Outcome::exited;}
            break;
        case State::play_opening_pause:
            if(settled && pause(value)) start(State::play_opening_photo,X,now,HOLD_LIMIT);
            else if(now>=m_deadline) return fail(now);
            break;
        case State::play_opening_photo:
            if(photo(value)) {m_state=State::active;m_until=now;if(m_refreezes!=UINT64_MAX)++m_refreezes;step.outcome=Outcome::refrozen;}
            else if(now>=m_deadline || (settled && gameplay(value))) return fail(now);
            break;
        }
        if(now<m_until && m_state!=State::off) {step.buttons=m_buttons;step.until=m_until;}
        return step;
    }
private:
    void start(State state,uint16_t buttons,Clock::time_point now,Clock::duration press) {
        m_state=state;m_buttons=buttons;m_until=now+press;m_deadline=now+press+WAIT;
    }
    void release(Clock::time_point now) {m_state=State::releasing;m_buttons=0;m_until=now;m_deadline=now+RELEASE_LIMIT;}
    void start_playing(Clock::time_point now) {m_state=State::playing;m_buttons=0;m_until=now;m_play_started=now;}
    Step fail(Clock::time_point now) {m_state=State::off;m_until=now;++m_failures;return {0,now,Outcome::failed};}
    State m_state{State::off};
    uint16_t m_buttons{};
    Clock::time_point m_until{},m_deadline{},m_play_started{};
    uint64_t m_failures{},m_unreleased{},m_plays{},m_refreezes{};
};

namespace vr_viewer {
// Which first-person character edits run while the viewer sequence is engaged.
// The freeze keeps the last pose the game published: the arm driver counts no publication in
// the pause menu or photo mode (26 Oct log: the count stays put from the pause until the exit),
// so whatever was written into that pose stays: the arm driver's arms and fingers, and the
// head shrink. Up to the freeze the arms and fingers keep following the controllers
// (keep_pose) and only the head is restored, so the frozen Marine holds the player's pose
// with a full-size head. With keep_pose off it falls back to the game's own pose (the
// behaviour before 26 Oct). Once photo mode shows nothing is driven: there are no
// publications, and viewer frames carry no controller targets.
// Up to the freeze the Marine also keeps where the player was looking (Joe, 2 Nov). aim_head turns
// the head to the gaze (VrHeadAim.h). hold_facing keeps body following on: in the 31 Oct 16:04
// run the game turned the Marine 32 degrees off the player's facing in the 100 ms between the
// release and the pause (root yaw -101 to -134, still turning when it froze).
struct PoseEdits {
    bool arms{},hide_head{},aim_head{},hold_facing{};
    bool operator==(const PoseEdits&) const=default;
};
inline bool before_freeze(VrViewerSequencer::State state) {
    using S=VrViewerSequencer::State;
    return state==S::resuming || state==S::releasing || state==S::opening_pause || state==S::opening_photo;
}
inline PoseEdits pose_edits(bool first_person,VrViewerSequencer::State state,bool keep_pose) {
    if(!first_person) return {};
    if(state==VrViewerSequencer::State::off) return {true,true};
    const bool kept=keep_pose && before_freeze(state);
    return {kept,false,kept,kept};
}
// A right stick click saves a picture in camera mode (Joe, 1 Nov): frozen or while the game runs
// under the free camera. Not in the menu presses around them: those frames show the pause menu.
inline bool takes_photos(VrViewerSequencer::State state) {
    return state==VrViewerSequencer::State::active || state==VrViewerSequencer::State::playing;
}

// Watch it play out (Joe, 30 Oct: "a button that the player can let the game play while they're
// in diorama mode to watch the execution... just the length of the execution animation... with
// sound"). Right A in the viewer lets the game run under the free camera; when the play ends the
// viewer closes and the player is back in the game (Joe, 31 Oct), unless Right A froze it first.
// config/index_viewer_play.calibration, read live; docs/VIEWER_MODE.md.
struct Play {
    using Values=vr_body::Parameters<5>;
    enum Index : size_t {MODE,STILL_SPEED,STILL_SECONDS,CLIP_SECONDS,LIMIT_SECONDS};
    // off: Right A does nothing. animation: until the Marine has moved and then stayed still (an
    // execution plays out), or clip_seconds if it never moves. fixed: clip_seconds. hold: while
    // Right A is held; letting go freezes. limit_seconds caps every mode.
    enum class Mode {off,animation,fixed,hold};
    inline static constexpr std::array<Values::Parameter,5> PARAMETERS{{
        {"mode",1,0,3},
        // 30 Oct playtest trace, 15 finisher presses: the root, HEAD and HAND bones all stay under
        // 0.35 m/s for 0.5 s only once an execution has ended (0.5-0.65 s after the Marine stops);
        // mid-execution the hands keep moving faster than that.
        {"still_speed_metres_per_second",.35f,.05f,3},
        {"still_seconds",.5f,.1f,5},
        {"clip_seconds",3,.5f,30},
        // Executions take 3-5.5 s from the press (TAKEOVER_RETURN_2026-10-26).
        {"limit_seconds",10,1,60}}};
    Values values=Values::defaults(PARAMETERS);
    bool valid() const {
        const auto& v=values.values;
        return values.in_range(PARAMETERS) && v[MODE]==std::floor(v[MODE]) && v[CLIP_SECONDS]<=v[LIMIT_SECONDS];
    }
    Mode mode() const {return valid()?static_cast<Mode>(static_cast<int>(values.values[MODE])):Mode::off;}
    float still_speed() const {return values.values[STILL_SPEED];}
    std::chrono::duration<float> still() const {return std::chrono::duration<float>(values.values[STILL_SECONDS]);}
    std::chrono::duration<float> clip() const {return std::chrono::duration<float>(values.values[CLIP_SECONDS]);}
    std::chrono::duration<float> limit() const {return std::chrono::duration<float>(values.values[LIMIT_SECONDS]);}
    static const char* name(Mode mode) {
        switch(mode) {case Mode::off:return "off";case Mode::animation:return "animation";case Mode::fixed:return "fixed";case Mode::hold:return "hold";}
        return "unknown";
    }
    static std::optional<Play> load(const std::filesystem::path& path) {
        std::ifstream in{path};const auto v=Values::read(in,PARAMETERS);if(!v) return {};
        Play p;p.values=*v;return p.valid()?std::optional{p}:std::nullopt;
    }
    bool operator==(const Play&) const=default;
};

// The player character's motion from its native pose publications: the fastest of the root,
// HEAD and both HAND bones, in metres per second. Each speed spans at least INTERVAL, so the
// jitter of publication times does not read as motion. A gap in the publications (or another
// actor) restarts the still time.
class Motion {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto INTERVAL=std::chrono::milliseconds(50),GAP=std::chrono::milliseconds(300);
    // points: root, HEAD, HANDL, HANDR in world units (any may be missing).
    struct Sample {Clock::time_point time{};uintptr_t actor{};std::array<std::optional<Vec>,4> points{};};
    // Publications from before now (the last one before the freeze) are ignored.
    void reset(Clock::time_point now) {*this=Motion{};m_since=m_quiet_since=now;}
    void add(const Sample& s,float units_per_metre,float still_speed) {
        if(s.time==Clock::time_point{} || s.time<m_since || !std::isfinite(units_per_metre) || units_per_metre<=0) return;
        if(m_reference && s.time<=m_latest) return; // already seen
        if(!m_reference || s.actor!=m_reference->actor || s.time-m_latest>GAP) {
            if(m_reference) m_quiet_since=s.time;
            m_reference=s;m_latest=s.time;return;
        }
        m_latest=s.time;
        if(s.time-m_reference->time<INTERVAL) return;
        const float seconds=std::chrono::duration<float>(s.time-m_reference->time).count();
        float fastest=-1;
        for(size_t i=0;i<s.points.size();++i) if(s.points[i] && m_reference->points[i])
            fastest=std::max(fastest,length(sub(*s.points[i],*m_reference->points[i]))/units_per_metre/seconds);
        m_reference=s;
        if(!(fastest>=0)) {m_quiet_since=s.time;return;} // nothing to compare: unknown, not still
        m_speed=fastest;if(m_measured!=UINT64_MAX)++m_measured;
        if(fastest>still_speed) {m_moved=true;m_quiet_since=s.time;}
    }
    // Moved faster than the still speed since the reset.
    bool moved() const {return m_moved;}
    // How long it has stayed under the still speed, up to its latest publication; nullopt
    // when the publications are not fresh (none within GAP of now).
    std::optional<Clock::duration> still_for(Clock::time_point now) const {
        if(!m_reference || now<m_latest || now-m_latest>GAP || m_latest<m_quiet_since) return {};
        return m_latest-m_quiet_since;
    }
    float speed() const {return m_speed;}
    uint64_t measured() const {return m_measured;}
private:
    std::optional<Sample> m_reference;
    Clock::time_point m_since{},m_latest{},m_quiet_since{};
    bool m_moved{};
    float m_speed{};
    uint64_t m_measured{};
};

// Why a play ends. cancelled (Right A again) and left (Left B, out of the viewer) are VrSession's;
// the rest come from play_end.
enum class PlayEnd {none,settled,clip,limit,released,cancelled,left};
inline const char* name(PlayEnd end) {
    switch(end) {case PlayEnd::none:return "none";case PlayEnd::settled:return "settled";case PlayEnd::clip:return "clip";
        case PlayEnd::limit:return "limit";case PlayEnd::released:return "released";case PlayEnd::cancelled:return "cancelled";case PlayEnd::left:return "left";}
    return "unknown";
}
// The player's own Right A freezes the game: a press, or letting go in hold mode. A play that runs
// its course ends in the game (Joe, 31 Oct: the frozen pause menu after a play was "quite strange").
inline bool freezes(PlayEnd end) {return end==PlayEnd::cancelled || end==PlayEnd::released;}
// Whether a play is over, elapsed into it. held: Right A is down now.
inline PlayEnd play_end(const Play& play,std::chrono::steady_clock::duration elapsed,bool held,bool moved,
    std::optional<std::chrono::steady_clock::duration> still_for) {
    if(elapsed>=play.limit()) return PlayEnd::limit;
    switch(play.mode()) {
    case Play::Mode::off: return PlayEnd::limit;
    case Play::Mode::fixed: return elapsed>=play.clip()?PlayEnd::clip:PlayEnd::none;
    case Play::Mode::hold: return held?PlayEnd::none:PlayEnd::released;
    case Play::Mode::animation:
        if(moved) return still_for && *still_for>=play.still()?PlayEnd::settled:PlayEnd::none;
        return elapsed>=play.clip()?PlayEnd::clip:PlayEnd::none;
    }
    return PlayEnd::limit;
}
} // namespace vr_viewer
