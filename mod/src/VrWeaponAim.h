#pragma once
#include "VrArmPose.h"
#include "VrHandedness.h"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

// One committed weapon pose feeds both attachment publication and native collision aim.
// Geometry is resolved by rig names. Units/epochs come from the existing wrist pipeline.
class VrWeaponAim {
public:
    using Clock=std::chrono::steady_clock;
    using Matrix=vr_arm::Matrix;
    // speed_scale: grenade launches only (launch speed / native speed, VrGrenadeAim).
    struct Pose { uintptr_t actor{};uint64_t epoch{},sequence{};Clock::time_point captured{};Matrix world{};std::optional<size_t> hand;float speed_scale{1}; };
    struct Hit { Pose pose;std::array<float,3> target{};Clock::time_point captured{}; };
    struct Gate {bool allowed{};uint64_t epoch{};};
    static Gate gate(bool foreground,bool diagnostic,bool runtime,bool right_tracked,uint64_t epoch) {
        return {foreground && (diagnostic || (runtime && right_tracked && epoch)),diagnostic?1:epoch};
    }
    void enable(bool value) { std::scoped_lock lock{m_mutex};m_enabled=value;m_pose.reset();m_throw.reset();m_hit.reset(); }
    bool enabled() const { std::scoped_lock lock{m_mutex};return m_enabled; }
    void allow(bool value,uint64_t epoch) {
        std::scoped_lock lock{m_mutex};if(!value || epoch!=m_epoch) {m_pose.reset();m_throw.reset();m_hit.reset();}
        m_allowed=value;m_epoch=epoch;
    }
    void publish(const Pose& pose) {
        std::scoped_lock lock{m_mutex};
        if(!m_enabled || !m_allowed || !pose.actor || !pose.sequence || pose.epoch!=m_epoch || !vr_camera::rigid(pose.world)) {m_pose.reset();m_hit.reset();return;}
        m_pose=pose;
    }
    std::optional<Pose> pose(uintptr_t actor,Clock::time_point now=Clock::now()) const {
        std::scoped_lock lock{m_mutex};return valid(actor,now)?m_pose:std::nullopt;
    }
    // Short, frozen throw target uses the existing exact-player native ray/alias
    // path. Gun attachment/selected-hand consumers continue reading pose().
    void throw_aim(const std::optional<Pose>& pose) {
        std::scoped_lock lock{m_mutex};
        m_throw=pose && m_enabled && m_allowed && pose->actor && pose->epoch==m_epoch && pose->sequence && vr_camera::rigid(pose->world)?pose:std::nullopt;
    }
    std::optional<Pose> ray_pose(uintptr_t actor,Clock::time_point now=Clock::now()) const {
        std::scoped_lock lock{m_mutex};return ray_locked(actor,now);
    }
    bool hit(const Pose& pose,const std::array<float,3>& target,Clock::time_point now=Clock::now()) {
        std::scoped_lock lock{m_mutex};
        const auto current=ray_locked(pose.actor,now);
        if(!current || pose.epoch!=m_epoch || pose.sequence!=current->sequence ||
            !std::ranges::all_of(target,[](float value){return std::isfinite(value) && std::abs(value)<1000000;})) return false;
        m_hit=Hit{pose,target,now};return true;
    }
    std::optional<Hit> hit(uintptr_t actor,Clock::time_point now=Clock::now()) const {
        std::scoped_lock lock{m_mutex};
        if(!valid(actor,now) || !m_hit || m_hit->pose.actor!=actor || m_hit->pose.epoch!=m_epoch ||
            !fresh(m_hit->captured,now) || !fresh(m_hit->pose.captured,now)) return {};
        if((m_hit->pose.sequence&(uint64_t{1}<<63)) && (!ray_locked(actor,now) || ray_locked(actor,now)->sequence!=m_hit->pose.sequence))return {};
        return m_hit;
    }
    static bool fresh(Clock::time_point captured,Clock::time_point now) { return captured!=Clock::time_point{} && now>=captured && now-captured<=std::chrono::milliseconds(100); }
private:
    std::optional<Pose> ray_locked(uintptr_t actor,Clock::time_point now) const {
        if(!valid(actor,now)) return {};
        if(m_throw && m_throw->actor==actor && m_throw->epoch==m_epoch && now>=m_throw->captured && now-m_throw->captured<=std::chrono::milliseconds(750)) {
            auto result=*m_throw;result.captured=now;result.sequence|=uint64_t{1}<<63;return result;
        }
        return m_pose;
    }
    bool valid(uintptr_t actor,Clock::time_point now) const {return m_enabled && m_allowed && m_pose && actor && m_pose->actor==actor && m_pose->epoch==m_epoch && fresh(m_pose->captured,now);}
    mutable std::mutex m_mutex;
    bool m_enabled{},m_allowed{};uint64_t m_epoch{};
    std::optional<Pose> m_pose;
    std::optional<Pose> m_throw;
    std::optional<Hit> m_hit;
};

namespace vr_weapon {
struct Parameter {const char* name;float minimum,maximum;};
inline constexpr std::array<Parameter,6> PARAMETERS{{{"grip_x_metres",-.25f,.25f},{"grip_y_metres",-.25f,.25f},{"grip_z_metres",-.25f,.25f},
    {"pitch_degrees",-180,180},{"yaw_degrees",-180,180},{"roll_degrees",-180,180}}};
struct Settings {
    std::array<float,6> values{};
    bool operator==(const Settings&) const = default;
    bool valid() const {for(size_t i=0;i<values.size();++i) if(!std::isfinite(values[i]) || values[i]<PARAMETERS[i].minimum || values[i]>PARAMETERS[i].maximum) return false;return true;}
    vr_arm::Matrix matrix() const {
        auto result=vr_camera::IDENTITY;
        for(int i=0;i<3;++i) {
            auto rotation=vr_camera::IDENTITY;const auto angle=values[i+3]*0.01745329252f;const auto a=(i+1)%3,b=(i+2)%3;
            rotation[a*4+a]=rotation[b*4+b]=std::cos(angle);rotation[a*4+b]=std::sin(angle);rotation[b*4+a]=-std::sin(angle);
            result=vr_camera::multiply(result,rotation);
        }
        // Translation is expressed in authored gun-grip axes, not rotated by Euler construction.
        for(int i=0;i<3;++i) result[12+i]=values[i];return result;
    }
    static std::optional<Settings> read(std::istream& in) {
        Settings result;std::array<bool,6> seen{};std::string line;
        while(std::getline(in,line)) {
            const auto start=line.find_first_not_of(" \t\r");if(start==std::string::npos || line[start]=='#') continue;
            const auto split=line.find('=');if(split==std::string::npos) return {};
            size_t i=0;while(i<PARAMETERS.size() && line.substr(0,split)!=PARAMETERS[i].name) ++i;
            if(i==PARAMETERS.size() || seen[i]) return {};
            std::istringstream number{line.substr(split+1)};if(!(number>>result.values[i]) || !(number>>std::ws).eof()) return {};seen[i]=true;
        }
        return in.eof() && std::ranges::all_of(seen,[](bool v){return v;}) && result.valid()?std::optional{result}:std::nullopt;
    }
    static std::optional<Settings> load(const std::filesystem::path& path) {std::ifstream in{path};return read(in);}
    bool save(const std::filesystem::path& path) const {
        if(!path.is_absolute() || !valid()) return false;
        std::ofstream out{path};out<<std::setprecision(9);for(size_t i=0;i<values.size();++i) out<<PARAMETERS[i].name<<'='<<values[i]<<'\n';out.flush();return bool(out);
    }
};
// How the native animation holds the gun, read from which hand socket the game's Gun_pose sits on.
//  left:       on GUN_L with the right hand elsewhere: the pistol (left trigger fires it).
//  right:      on GUN_R, or on both sockets (a bolter): a primary.
//  two_handed: on GUN_L with GUN_R within TWO_HANDED_METRES of it: a primary carried on the left
//              socket with the right hand on it too. The Heavy's heavy weapon in the battle
//              simulator (26 Oct playtest, bone dumps 11:05:34-11:06:34): Gun_pose exactly on
//              GUN_L, GUN_R 31 cm away; the nearest a pistol's GUN_R came that session was 56 cm.
//              It read as the pistol, so the gun jumped to the left hand and the left trigger
//              fired it. A primary: the right hand drives it, the right trigger fires.
// SOCKET_METRES is position only (but see SOCKET_DEGREES). The other heavy weapons sat 2 cm and 9 degrees off GUN_R, which
// the old 2 cm and 0.02-per-axis check never matched: the reading stayed on whatever weapon came
// before (the pistol, for 15 s at 11:03:29). The grip itself is still learned from the socket.
// two_handed also needs GUN_R turned within TWO_HANDED_DEGREES of GUN_L: with both hands on one gun
// both sockets describe that gun. Distance alone let the pistol read two_handed whenever the right
// hand came close with its own weapon (26 Oct classes playtest, 30 Hz trace: the Sniper's knife
// strikes 23-33 cm, the Assault's two-handed hammer 12-28 cm; the pistol jumped to the right hand
// nine times in 90 s). Long guns on GUN_L: 12 degrees (heavy), 21 (sniper rifle, 820 frames);
// every other frame on GUN_L with GUN_R within 40 cm: 50 to 180.
// right also needs Gun_pose turned within SOCKET_DEGREES of GUN_R. Position alone read the pistol
// as a primary in an execution (27 Oct evening playtest, 18:32:10): Gun_pose stayed on GUN_L while
// GUN_R, both hands on the pistol, came 2-9 cm from it turned 100-160 degrees. The misread held,
// GripSwitch then refused the true left reading, and the pistol was drawn in the right hand inside
// the chainsword until the next weapon switch. A bolter sits 0 degrees off GUN_R, a heavy 9.
enum class Grip : uint8_t { left,right,two_handed };
inline constexpr float SOCKET_METRES=.05f,SOCKET_DEGREES=45,TWO_HANDED_METRES=.40f,TWO_HANDED_DEGREES=45;
inline const char* name(Grip g) {return g==Grip::left?"left":g==Grip::right?"right":"two_handed";}
inline size_t hand_of(Grip g) {return g==Grip::left?0:1;}   // The VR hand that drives the gun (0 left).
inline bool on_left_socket(Grip g) {return g!=Grip::right;} // The socket the grip is learned from.
inline bool primary(Grip g) {return g!=Grip::left;}         // Only the pistol (secondary) is held left.
// Angle in degrees between two frames' rotations (rows normalised; positions ignored).
inline float turn_degrees(const vr_arm::Matrix& a,const vr_arm::Matrix& b) {
    float trace=0;
    for(int row=0;row<3;++row) {
        const vr_arm::Vector u{a[row*4],a[row*4+1],a[row*4+2]},v{b[row*4],b[row*4+1],b[row*4+2]};
        const float scale=vr_arm::length(u)*vr_arm::length(v);if(!(scale>0)) return 180;
        trace+=vr_arm::dot(u,v)/scale;
    }
    return std::acos(std::clamp((trace-1)/2,-1.f,1.f))*57.2957795f;
}
inline std::optional<Grip> read_grip(const vr_arm::Matrix& gun,const vr_arm::Matrix& left_socket,const vr_arm::Matrix& right_socket,float units_per_metre) {
    const auto distance=[](const vr_arm::Matrix& a,const vr_arm::Matrix& b){return vr_arm::length(vr_arm::sub(vr_arm::position(a),vr_arm::position(b)));};
    const float socket=SOCKET_METRES*units_per_metre; // ("near" is a Windows macro)
    if(distance(gun,right_socket)<=socket && turn_degrees(gun,right_socket)<=SOCKET_DEGREES) return Grip::right;
    if(distance(gun,left_socket)>socket) return {};
    return distance(left_socket,right_socket)<=TWO_HANDED_METRES*units_per_metre && turn_degrees(left_socket,right_socket)<=TWO_HANDED_DEGREES?
        Grip::two_handed:Grip::left;
}
// One rule for a held-weapon reading that flips: a different reading must persist for HOLD before
// it replaces the current one, as VrCombatRouting's REV_HOLDOVER does for the chainsword rev. A
// poll with no reading (the gun between sockets: recoil, a draw in progress) neither confirms nor
// cancels a pending change. The first reading is taken at once. Every consumer of the held weapon
// (the gun's hand, trigger routing, support, holsters, sword gates) sees only the settled reading.
//
// Joe's rule (26 Oct): a primary never goes to the left hand. With a primary held, a left reading
// counts only within SWITCH_WINDOW of a weapon-switch request (weapon_switch(): the native D-pad
// down the trackpad and both holsters send); otherwise it is no reading, so the primary keeps the
// right hand and its fit. The native animation carries a primary on GUN_L alone in finishers and
// knife strikes: in the classes playtest trace the Vanguard's and the Sniper's primaries read left
// for 1 to 10 s at a time, five times, with no weapon switch (the only two D-pad presses were the
// holster draws). Each one moved the gun to the left hand and the left trigger.
//
// The other way, pistol to primary, has the same blind spot (30 Oct 20:24 playtest, battle simulator):
// after any melee input (RB, LB, D-pad up) the gun sits on GUN_L alone, sword in the right hand, for
// seconds (96 % of readings within 1 s of a melee press, 65 % 6-15 s after), and
// between fights the bolter's carry pose is on neither socket. So after a shoulder draw from the
// pistol the bolter read left or none for minutes, the left reading kept confirming the pistol, and
// the bolter followed the left hand and the left trigger: 20:37:11-20:39:41 (Joe, 20:38:49: "the
// thing with bolter is on my left hand"), and every other pistol-to-bolter draw in the run (20:43:29,
// 20:51:21). What does tell them apart is the draw itself: a primary drawn from the back leaves both
// sockets for 0.9-3.5 s (none readings) starting within 0.1 s of the press; the press that changed
// nothing left GUN_L for 0.2 s (20:37:09) or not at all (20:42:03). So with the pistol held, a weapon
// switch whose gun leaves both sockets within DRAW_START of the press and stays off for DRAW_HOLD
// lands the primary in the right hand at once (draws()). Each press settles at most one change
// (spent), so the drawn primary's own left readings in the same window are refused.
//
// A draw or a new actor lands a primary with no reading, so the gun is not on GUN_R. GUN_R is not a
// fit then: with the gun off it, it sits in the chainsword's grip, 11 cm from HANDR where the seated
// gun is 33 cm away, 68 degrees and 27.5 cm from the gun (31 Oct 16:04 playtest, 16:12:03.957 and
// 16:13:18.8: "on the same angle as my chainsaw"). seated() stays false until the reading has held
// the grip for HOLD (the first frames on GUN_R are the socket blending onto the gun, 18 degrees off
// at 16:12:04.4). The grip itself is settled at once: only the fit waits (retarget).
class GripSwitch {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto HOLD=std::chrono::milliseconds(250);
    static constexpr auto SWITCH_WINDOW=std::chrono::seconds(3); // The live draws settled 0.6 and 1.5 s after the press.
    static constexpr auto DRAW_START=std::chrono::milliseconds(500),DRAW_HOLD=std::chrono::milliseconds(500);
    static constexpr auto PRESS_GAP=std::chrono::milliseconds(100); // weapon_switch() is polled while D-pad down is held.
    std::optional<Grip> update(std::optional<Grip> reading,Clock::time_point now) {
        const auto held=settled(reading,now);
        if(!reading || reading!=held) m_seat_since.reset();
        else if(!m_seat_since) m_seat_since=now;
        if(m_seat_since && now>=*m_seat_since && now-*m_seat_since>=HOLD) m_seated=true;
        return held;
    }
    // A weapon switch was requested (the native D-pad down). Polled while held: one press is one request.
    void weapon_switch(Clock::time_point now) {
        if(!m_switch_at || now<*m_switch_at || now-*m_switch_at>PRESS_GAP) {m_request_at=now;m_request_from=m_current;m_spent=false;}
        m_switch_at=now;
    }
    bool switching(Clock::time_point now) const {return m_switch_at && !m_spent && now>=*m_switch_at && now-*m_switch_at<=SWITCH_WINDOW;}
    void reset() {m_current.reset();m_candidate.reset();m_off_since.reset();m_seat_since.reset();m_seated=true;} // Weapon following restarted: the first reading is taken at once.
    // A new actor (a respawn, a level load) holds the primary: its first reading is not taken at once.
    // 30 Oct playtest (battle simulator): every respawn came back with the bolter, also the two after
    // dying with the pistol out (20:34:10, 20:42:12). With the bolter on GUN_L alone most frames, a
    // first reading taken at once read left: the bolter went to the left hand and the left trigger,
    // 20:46:35-20:47:05 ("I've lost control of my gun again", 20:46:54) and 20:42:12-20:42:17. Cost: a
    // spawn holding the pistol keeps it in the right hand until the next weapon switch.
    void new_actor() {if(m_current && !primary(*m_current)) m_current=Grip::right;m_candidate.reset();m_off_since.reset();m_seat_since.reset();m_seated=!m_current;m_spent=true;}
    std::optional<Grip> current() const {return m_current;}
    bool seated() const {return m_seated;}         // False while a grip that landed off its socket waits for it (above).
    uint64_t switches() const {return m_switches;} // Settled changes, for the log.
    uint64_t refused() const {return m_refused;}   // Left readings refused while a primary was held.
    uint64_t draws() const {return m_draws;}       // Primaries landed by the draw rule (pistol held, gun off both sockets).
private:
    std::optional<Grip> settled(std::optional<Grip> reading,Clock::time_point now) {
        if(reading) m_off_since.reset();else if(!m_off_since) m_off_since=now;
        if(!reading && drawing(now)) {
            const auto from=std::max(*m_off_since,*m_request_at);
            if(from-*m_request_at<=DRAW_START && now-from>=DRAW_HOLD) {settle(Grip::right);m_seated=false;++m_draws;return m_current;}
        }
        if(reading && !primary(*reading) && m_current && primary(*m_current) && !switching(now)) {++m_refused;m_candidate.reset();reading.reset();}
        if(!reading) return m_current;
        if(!m_current || *reading==*m_current) {m_current=reading;m_candidate.reset();return m_current;}
        if(m_candidate!=reading) {m_candidate=reading;m_since=now;}
        if(now>=m_since && now-m_since>=HOLD) settle(*reading);
        return m_current;
    }
    bool drawing(Clock::time_point now) const {
        return switching(now) && m_off_since && m_current && !primary(*m_current) && m_request_from && !primary(*m_request_from);
    }
    // A change between the pistol and a primary spends the press; one primary for another (right and
    // two_handed flapping) does not. A grip settled from its reading is on its socket.
    void settle(Grip grip) {if(m_current && primary(*m_current)!=primary(grip)) m_spent=true;m_current=grip;m_candidate.reset();m_seated=true;++m_switches;}
    std::optional<Grip> m_current,m_candidate;Clock::time_point m_since{};uint64_t m_switches{},m_refused{},m_draws{};
    std::optional<Clock::time_point> m_switch_at,m_request_at,m_off_since,m_seat_since;std::optional<Grip> m_request_from;bool m_spent{},m_seated{true};
};
struct Attachment {
    size_t index{};vr_arm::Matrix local{},world{};
    size_t hand{1};
    vr_arm::Matrix relative{}; // Stable authored socket-to-wrist fit, excluding live calibration.
    std::optional<Grip> grip;  // Unset on a rig without both hand sockets.
    bool seated{true};         // False: relative is provisional, the grip has not been seen on its socket yet.
};
// A primary carried off both sockets sits in the hand: Gun_pose is rigid to HANDR, the same fit GUN_R
// gives once the gun is on it (median 0.05 degrees between them over 1325 frames, 16:12-16:14). The
// bolter is 33 cm from HANDR, the other classes' primaries 30-45 cm (26 Oct classes trace). A gun
// further away with no reading is mid-draw or in the other hand's melee carry: not a fit.
inline constexpr float CARRY_METRES=.50f;
// Preserve the authored gun-to-hand grip while moving its independent spine branch.
// Caller commits this with the arm batch and publishes the ray pose only after success.
// previous_grip: the committed grip (default: from previous_hand). The fit is kept only while the
// grip is unchanged. switcher: debounces the reading (GripSwitch); without it a reading applies at once.
// previous_seated: the committed fit's Attachment::seated.
// swap: left-handed play (VrHandedness.h). The reading, the fit and Attachment::hand keep the game's
// own hand, which is the logical one (0 the pistol's hand); only the wrist that carries the gun is
// the other one.
inline std::optional<Attachment> retarget(std::span<const vr_arm::Matrix> original,
    std::span<const vr_arm::Matrix> changed,std::span<const int32_t> parents,std::span<const std::string> names,
    const vr_arm::Matrix& root,const vr_arm::Matrix& grip,float units_per_metre,std::optional<size_t> previous_hand=std::nullopt,
    const std::optional<vr_arm::Matrix>& previous_relative=std::nullopt,std::optional<Grip> previous_grip=std::nullopt,
    GripSwitch* switcher=nullptr,GripSwitch::Clock::time_point now={},bool previous_seated=true,const vr_hand::Swap* swap=nullptr) {
    if(names.size()!=parents.size() || !vr_camera::rigid(grip) || !std::isfinite(units_per_metre) || units_per_metre<0.01f || units_per_metre>100) return {};
    const auto index=[&](const char* name)->std::optional<size_t> {
        const auto it=std::find(names.begin(),names.end(),name);
        if(it==names.end() || std::count(names.begin(),names.end(),name)!=1) return {};
        return static_cast<size_t>(it-names.begin());
    };
    const auto hand=index("HANDR"),gun=index("Gun_pose"),animation=index("GUN_ANIM");
    if(!hand || !gun || !animation || parents[*gun]!=static_cast<int32_t>(*animation) || parents[*animation]<0 || parents[*animation]>=static_cast<int32_t>(*animation) || names[parents[*animation]]!="BACKup") return {};
    std::vector<vr_arm::Matrix> before,after;
    if(!vr_arm::hierarchy(original,parents,root,before) || !vr_arm::hierarchy(changed,parents,root,after)) return {};
    auto source_hand=*hand;
    size_t driven_hand=previous_hand.value_or(1);
    if(driven_hand>1) return {};
    auto authored=before[*gun];
    std::optional<Grip> reading,held;
    const auto left=index("HANDL"),left_socket=index("GUN_L"),right_socket=index("GUN_R");
    if(left && left_socket && right_socket && parents[*left_socket]==static_cast<int32_t>(*left) &&
       parents[*right_socket]==static_cast<int32_t>(*hand)) {
        // Both sockets coincide for a bolter. The pistol follows GUN_L while GUN_R carries the
        // chainsword; a heavy weapon rides GUN_L with the right hand on it (read_grip). Resolve the
        // authored grip from that actual socket, not the distance to HANDR.
        reading=read_grip(before[*gun],before[*left_socket],before[*right_socket],units_per_metre);
        if(!previous_grip && previous_hand) previous_grip=*previous_hand==0?Grip::left:Grip::right;
        // Recoil/aim animation can move Gun_pose off BOTH sockets for several frames: no reading.
        // Retain the last observed loadout and use the hand socket's authored grip, rather than
        // importing that animation displacement.
        held=switcher?switcher->update(reading,now):reading?reading:previous_grip;
        if(!held) return {}; // Never learn a grip from an unowned transition.
        driven_hand=hand_of(*held);
        source_hand=driven_hand==0?*left:*hand;
        authored=before[on_left_socket(*held)?*left_socket:*right_socket];
    }
    auto offset=grip;for(int axis=0;axis<3;++axis) offset[12+axis]*=units_per_metre;
    // Native firing can animate the hand socket itself, not only Gun_pose.
    // Retain the full wrist-relative fit across those frames; still follow the
    // current tracked wrist and apply live calibration on every publication.
    // A grip change relearns it: a heavy weapon on GUN_L and a bolter on GUN_R are both driven by
    // the right hand, but sit differently in it.
    // A grip landed off its socket (GripSwitch::seated) has no socket to learn from: the carried gun is
    // the fit while it waits, and the socket replaces it once, on the frame the grip seats.
    const bool waiting=switcher && !switcher->seated();
    const bool carried=waiting && !reading &&
        vr_arm::length(vr_arm::sub(vr_arm::position(before[*gun]),vr_arm::position(before[source_hand])))<=CARRY_METRES*units_per_metre;
    if(carried) authored=before[*gun];
    const bool same=previous_hand==std::optional{driven_hand} && (!held || !previous_grip || *previous_grip==*held);
    const auto relative=!carried && same && previous_relative && (waiting || previous_seated)?
        *previous_relative:vr_camera::multiply(authored,vr_camera::inverse(before[source_hand]));
    if(!vr_camera::rigid(relative)) return {};
    if(swap && !left) return {};
    const auto fitted=vr_camera::multiply(offset,relative);
    const auto world=swap?vr_camera::multiply(swap->moved(fitted,driven_hand),after[driven_hand==0?*hand:*left]):vr_camera::multiply(fitted,after[source_hand]);
    const auto local=vr_camera::multiply(world,vr_camera::inverse(after[*animation]));
    if(!vr_camera::rigid(world) || !vr_camera::rigid(local)) return {};
    return Attachment{*gun,local,world,driven_hand,relative,held,!waiting};
}
}
