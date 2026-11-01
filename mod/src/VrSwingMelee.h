#pragma once
#include "VrCameraPose.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>

// Physical metres in standing-space axes, before rig snaps, world scale or wrist offsets.
// Produces a native attack request; it never manufactures damage or collision results.
class VrSwingMelee {
public:
    using Clock=std::chrono::steady_clock;
    struct Parameter { const char* name; float initial,minimum,maximum; };
    static constexpr std::array<Parameter,6> PARAMETERS{{
        {"speed_metres_per_second",1.5f,0.5f,5}, {"travel_metres",0.18f,0.05f,0.6f},
        {"reach_metres",0.45f,0.15f,0.9f}, {"rearm_metres_per_second",0.5f,0.05f,1},
        {"pulse_ms",140,80,250}, {"cooldown_ms",450,200,1500}}};
    struct Settings {
        std::array<float,6> values=[] { std::array<float,6> values{};for(size_t i=0;i<values.size();++i) values[i]=PARAMETERS[i].initial;return values; }();
        bool operator==(const Settings&) const = default;
        bool valid() const {
            for(size_t i=0;i<values.size();++i) if(!std::isfinite(values[i]) || values[i]<PARAMETERS[i].minimum || values[i]>PARAMETERS[i].maximum) return false;
            return values[3]<values[0] && values[4]<values[5];
        }
        static std::optional<Settings> read(std::istream& input) {
            Settings result;std::array<bool,6> seen{};std::string line;
            while(std::getline(input,line)) {
                const auto first=line.find_first_not_of(" \t\r");
                if(first==std::string::npos || line[first]=='#') continue;
                const auto split=line.find('=');if(split==std::string::npos) return {};
                size_t index=0;while(index<PARAMETERS.size() && line.substr(0,split)!=PARAMETERS[index].name) ++index;
                if(index==PARAMETERS.size() || seen[index]) return {};
                std::istringstream number{line.substr(split+1)};
                if(!(number>>result.values[index]) || !(number>>std::ws).eof()) return {};
                seen[index]=true;
            }
            return input.eof() && std::ranges::all_of(seen,[](bool v){return v;}) && result.valid()?std::optional{result}:std::nullopt;
        }
        bool write(std::ostream& out) const {
            if(!valid()) return false;
            out<<std::setprecision(9);
            for(size_t i=0;i<values.size();++i) out<<PARAMETERS[i].name<<'='<<values[i]<<'\n';
            return bool(out);
        }
        static std::optional<Settings> load(const std::filesystem::path& path) { std::ifstream in{path};return read(in); }
        bool save(const std::filesystem::path& path) const {
            if(!path.is_absolute() || !valid()) return false;
            std::ofstream out{path};if(!write(out)) return false;out.flush();return bool(out);
        }
    };
    struct Pose { vr_camera::Matrix hand,head; Clock::time_point captured; uint64_t epoch{}; };
    struct Snapshot { uint64_t swings{},discontinuities{},vetoes{},flicks{},windups{}; float speed{},travel{},other_hand_speed{},wrist_speed{}; bool ready{},pressed{},power{}; };
    // Joe, 20 Oct: power is a swing made while the trigger is held. A finger resting on the
    // Index trigger reads 0.05-0.09, so it must not count; same start as the chainsword rev
    // (EngineChainswordRev::START). Callers compare the raw trigger with this.
    static constexpr float POWER_TRIGGER=0.15f;
    // Joe, 20 Oct: no wrist flicks. The wrist itself must have moved this far within the
    // 200 ms history before any endpoint can commit; a twist alone moves only the virtual
    // tip. In his session the eight flick swings spanned at most 0.10 m at the wrist and the
    // arm swings at least 0.13 m.
    static constexpr float MIN_WRIST_TRAVEL=0.11f;
    // Joe's 21 Oct trace: flowing slashes (wrist 4-5 m/s, tip 9-12 m/s) never slowed below
    // the 0.5 m/s rearm between strokes (about 1 m/s at each reversal), so after the first
    // stroke none could start. After a strong stroke (peak at least twice the activation
    // speed), slowing to REARM_FRACTION of its peak also rearms; the next stroke must then
    // reach COMBO_FRACTION of that peak, so bringing the sword back after a hard slash is
    // not a second attack.
    static constexpr float REARM_FRACTION=0.35f, COMBO_FRACTION=0.5f;
    // Joe's 21 Oct evening campaign ("why does the sword just stop working?", 20:41:51): an
    // execution camera dropped the loadout reading, which reset the detector while he kept
    // slashing; his strokes then never slowed below the 0.5 m/s rearm (slowest 0.54-1.1 m/s)
    // and 6 s of hard slashes (1592.8-1598.4 s, tip 10-13.6 m/s) sent nothing. After a
    // reset, the same reversal rule as a combo arms the detector: a strong motion slowing to
    // REARM_FRACTION of its peak, the next stroke at COMBO_FRACTION of that peak.
    // Conditions on starting a new attack that leave one already sent alone.
    struct Commit {
        // False (the hand is in a holster zone): no new swing starts here. Readiness is
        // kept, so a swing that leaves the zone can start (Joe's sword rests at the hip
        // and is raised by the shoulder, both holster zones). A pulse already sent runs its
        // full length and its cooldown still applies. Selections are a harder gate: the
        // caller passes them as `allowed=false` to update, which resets the detector.
        bool allowed=true;
        // Recent endpoint speed of the other hand (VrHandMotion), m/s. A punch with the
        // other hand twists this controller too; its 0.45 m virtual tip turns that twist
        // into blade speed. When the other hand was at least as fast as this one (and at
        // least the activation speed), the motion is the other hand's: the swing is
        // consumed as a veto, with no native press.
        float other_hand_speed=0;
        // A stricter stroke (the bolter out): the committing endpoint at least min_speed, and
        // the wrist itself at least min_wrist_speed at commitment and min_wrist_travel within
        // the history. 0 keeps the sword rules.
        float min_speed=0,min_wrist_speed=0,min_wrist_travel=0;
        // The wrist must have stayed below eye level for the last OVERHEAD_MEMORY (an arm
        // lowered from overhead is not a gun swing).
        bool below_eyes=false;
    };
    bool set_settings(const Settings& value) { if(!value.valid()) return false; m_settings=value;reset();return true; }
    Settings settings() const { return m_settings; }
    // The 21 Oct stroke rules (combo rearm, wind-ups), on by default; off reproduces the
    // 20 Oct rules for trace replays (scripts/melee_replay.cpp --old).
    void set_stroke_rules(bool value) { m_combo_rearm=value; }
    // The 21 Oct evening reversal rearm after a reset, on by default (melee_replay --no-fresh-rearm).
    void set_fresh_rearm(bool value) { m_fresh_rearm=value; }
    Snapshot snapshot() const { return m_state; }
    void reset() { m_points.clear();m_tip_only=m_dipped=m_windup_seen=false;m_fresh=true;m_fresh_peak=0;m_stroke_peak=m_combo_floor=0;m_state.ready=m_state.pressed=m_state.power=false;m_pulse_until={};m_cooldown_until={};m_epoch=0;m_state.speed=m_state.travel=m_state.other_hand_speed=m_state.wrist_speed=0; }
    // Wrist and virtual blade tip relative to the head, in standing-space axes. The
    // controller's -Z aim axis approximates the held blade. HMD translation is subtracted
    // in room axes; head rotation, world scale and snaps cannot create speed.
    struct Endpoints { std::array<float,3> wrist,tip; };
    static std::optional<Endpoints> endpoints(const Pose& pose,float reach) {
        Endpoints e{};
        for(int i=0;i<3;++i) { e.wrist[i]=pose.hand[12+i]-pose.head[12+i];e.tip[i]=e.wrist[i]-pose.hand[8+i]*reach; }
        const auto finite=[](const std::array<float,3>& v){return std::ranges::all_of(v,[](float value){return std::isfinite(value);});};
        if(!finite(e.wrist) || !finite(e.tip)) return {};
        return e;
    }
    static float distance(const std::array<float,3>& a,const std::array<float,3>& b) { float squared{};for(int i=0;i<3;++i)squared+=std::pow(a[i]-b[i],2);return std::sqrt(squared); }
    // Implausible jumps are tracking discontinuities, never a swing. A 45 cm blade tip
    // moves much faster than its wrist during a valid angular swipe. The old common
    // 12 m/s ceiling rejected physical light swings (tip 13.7, wrist 6.3 m/s in the
    // trace). Keep the translation jump guard; bound angular tip travel separately.
    static bool discontinuity(float wrist_speed,float tip_speed) { return wrist_speed>12 || tip_speed>30; }
    bool update(const std::optional<Pose>& pose,bool allowed,Clock::time_point now,bool power=false,Commit commit={}) {
        if(!allowed || !pose || !pose->epoch || !vr_camera::rigid(pose->hand) || !vr_camera::rigid(pose->head) || now<pose->captured || now-pose->captured>std::chrono::milliseconds(100)) { reset();return false; }
        if(m_epoch!=pose->epoch) { reset();m_epoch=pose->epoch; }
        m_state.pressed=now<m_pulse_until;
        if(!m_points.empty() && pose->captured==m_points.back().time) return m_state.pressed;
        const auto ends=endpoints(*pose,m_settings.values[2]);
        if(!ends) { ++m_state.discontinuities;reset();return false; }
        const auto& point=ends->tip;const auto& wrist=ends->wrist;
        float tip_speed{},wrist_speed{};
        if(!m_points.empty()) {
            const auto dt=std::chrono::duration<float>(pose->captured-m_points.back().time).count();
            if(dt<=0 || dt>0.1f) { ++m_state.discontinuities;reset();m_epoch=pose->epoch; }
            else {
                tip_speed=distance(point,m_points.back().position)/dt;
                wrist_speed=distance(wrist,m_points.back().wrist)/dt;
                m_state.speed=(std::max)(tip_speed,wrist_speed);m_state.wrist_speed=wrist_speed;
                if(discontinuity(wrist_speed,tip_speed)) { ++m_state.discontinuities;reset();m_epoch=pose->epoch;tip_speed=wrist_speed=0; }
            }
        }
        m_points.push_back({point,wrist,pose->captured});
        while(m_points.size()>64 || pose->captured-m_points.front().time>std::chrono::milliseconds(200)) m_points.pop_front();
        const float tip_travel=distance(point,m_points.front().position),wrist_travel=distance(wrist,m_points.front().wrist);
        float wrist_reach{};for(const auto& p:m_points) wrist_reach=(std::max)(wrist_reach,distance(wrist,p.wrist));
        m_state.travel=(std::max)(tip_travel,wrist_travel);
        m_state.other_hand_speed=commit.other_hand_speed;
        if(m_state.speed<=m_settings.values[3] && m_tip_only) { ++m_state.flicks;m_tip_only=false; } // The motion ended without the wrist moving.
        // A strong stroke's reversal may come before the cooldown ends (Joe's slashes were
        // about 0.6 s apart); remember the dip and rearm when the cooldown expires.
        if(m_stroke_peak>0) {
            m_stroke_peak=(std::max)(m_stroke_peak,m_state.speed);
            if(m_combo_rearm && m_stroke_peak>=2*m_settings.values[0] && m_state.speed<=REARM_FRACTION*m_stroke_peak) { m_combo_floor=COMBO_FRACTION*m_stroke_peak;m_stroke_peak=0;m_dipped=true; }
        }
        if(m_combo_rearm && m_fresh_rearm && m_fresh && !m_state.ready && m_points.size()>1) {
            m_fresh_peak=(std::max)(m_fresh_peak,m_state.speed);
            if(m_fresh_peak>=2*m_settings.values[0] && m_state.speed<=REARM_FRACTION*m_fresh_peak) { m_combo_floor=COMBO_FRACTION*m_fresh_peak;m_fresh_peak=0;m_dipped=true; }
        }
        if(m_points.size()>1 && now>=m_cooldown_until) {
            if(m_state.speed<=m_settings.values[3]) { m_state.ready=true;m_combo_floor=m_stroke_peak=0;m_dipped=false; }
            else if(m_dipped && !m_state.ready) { m_state.ready=true;m_dipped=false; }
        }
        if(m_state.ready) { m_fresh=false;m_fresh_peak=0; }
        if(m_state.speed<=m_settings.values[3]) m_windup_seen=false;
        const float needed=(std::max)({m_settings.values[0],m_combo_floor,commit.min_speed});
        // Rotation around the estimated tip can leave it stationary during a large
        // hand arc. Either endpoint must independently meet BOTH thresholds;
        // do not combine a fast tip twitch with unrelated slow wrist travel.
        const bool endpoint=(tip_speed>=needed && tip_travel>=m_settings.values[1]) ||
            (wrist_speed>=needed && wrist_travel>=m_settings.values[1]);
        const bool windup=m_combo_rearm && winding_up(*pose,wrist,now);
        if(wrist[1]>0) m_overhead_at=now;
        const bool low=!commit.below_eyes || m_overhead_at==Clock::time_point{} || now-m_overhead_at>=OVERHEAD_MEMORY;
        const bool committed=endpoint && wrist_reach>=(std::max)(MIN_WRIST_TRAVEL,commit.min_wrist_travel) && wrist_speed>=commit.min_wrist_speed && low && commit.allowed && !windup;
        if(m_state.ready && endpoint && commit.allowed && windup && !m_windup_seen) { ++m_state.windups;m_windup_seen=true; }
        if(m_state.ready && endpoint && commit.allowed && !windup && !committed) m_tip_only=true;
        if(m_state.ready && committed) {
            m_tip_only=false;
            m_state.ready=false;m_combo_floor=0;
            m_cooldown_until=now+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float,std::milli>{m_settings.values[5]});
            if(commit.other_hand_speed>=(std::max)(m_settings.values[0],m_state.speed)) { ++m_state.vetoes;m_stroke_peak=0;return m_state.pressed; }
            ++m_state.swings;m_state.pressed=true;m_stroke_peak=m_state.speed;
            // Select once at the committed swing, never turn one held pulse into
            // two different native action edges when the trigger changes.
            m_state.power=power;
            m_pulse_until=now+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float,std::milli>{m_settings.values[4]});
        }
        return m_state.pressed;
    }
    // Joe's 21 Oct trace: lifting the sword from the hip to the shoulder, or back up after a
    // low cut, before the real slash, read as the swing, and the slash that followed fell
    // in the cooldown. A wrist rising and coming back towards the body (head-yaw frame,
    // over the last WINDUP_WINDOW), or rising almost straight up (raising the fist for the
    // emote, or the sword from the hip: 20 Oct trace 220.5-685.3 s), is a wind-up: it neither
    // attacks nor uses up readiness. Strikes move the hand forward, across or down; a rising
    // slash moves across or forward as it rises.
    static constexpr float WINDUP_METRES=0.02f;
    static constexpr auto WINDUP_WINDOW=std::chrono::milliseconds(50);
    static constexpr auto OVERHEAD_MEMORY=std::chrono::milliseconds(500);
private:
    bool winding_up(const Pose& pose,const std::array<float,3>& wrist,Clock::time_point now) const {
        const Point* back=nullptr;
        for(auto it=m_points.rbegin();it!=m_points.rend();++it) { back=&*it;if(now-it->time>=WINDUP_WINDOW) break; }
        if(!back || back==&m_points.back()) return false;
        float fx=-pose.head[8],fz=-pose.head[10];const float n=std::sqrt(fx*fx+fz*fz);
        if(!(n>1e-3f)) return false;
        fx/=n;fz/=n;
        const float dx=wrist[0]-back->wrist[0],dz=wrist[2]-back->wrist[2];
        const float up=wrist[1]-back->wrist[1],forward=dx*fx+dz*fz,horizontal=std::sqrt(dx*dx+dz*dz);
        return up>WINDUP_METRES && (forward<-WINDUP_METRES || up>2*horizontal);
    }
    struct Point { std::array<float,3> position,wrist;Clock::time_point time; };
    Settings m_settings;
    Snapshot m_state;
    std::deque<Point> m_points;
    uint64_t m_epoch{};
    bool m_tip_only{}; // A ready motion met the speed/travel thresholds on the tip alone.
    float m_stroke_peak{}; // Peak speed of the stroke since the last attack (0: none to rearm against).
    float m_combo_floor{}; // Speed the next stroke needs after a mid-motion rearm.
    bool m_combo_rearm{true},m_dipped{},m_windup_seen{};
    bool m_fresh{true}; // Not ready since the last reset (see the reversal rearm above).
    bool m_fresh_rearm{true};
    float m_fresh_peak{}; // Peak speed of the motion in progress while m_fresh.
    Clock::time_point m_pulse_until{},m_cooldown_until{},m_overhead_at{};
};

// Recent endpoint speed of one hand, measured exactly as VrSwingMelee measures its own
// (wrist and virtual blade tip relative to the head). The session feeds it the left hand
// so the right-hand detector can tell a left punch, which twists the right controller
// too, from a right swing. Reports the peak over the last PEAK_WINDOW; stale, invalid,
// re-anchored or jumping tracking reads as no motion.
class VrHandMotion {
public:
    using Clock=VrSwingMelee::Clock;
    static constexpr auto PEAK_WINDOW=std::chrono::milliseconds(150);
    void reset() { m_last.reset();m_speeds.clear(); }
    float update(const std::optional<VrSwingMelee::Pose>& pose,float reach,Clock::time_point now) {
        if(!pose || !pose->epoch || !vr_camera::rigid(pose->hand) || !vr_camera::rigid(pose->head) || now<pose->captured || now-pose->captured>std::chrono::milliseconds(100)) { reset();return 0; }
        const auto ends=VrSwingMelee::endpoints(*pose,reach);
        if(!ends) { reset();return 0; }
        if(m_last && m_last->epoch!=pose->epoch) reset();
        if(!m_last || m_last->time!=pose->captured) {
            if(m_last) {
                const auto dt=std::chrono::duration<float>(pose->captured-m_last->time).count();
                if(dt>0 && dt<=0.1f) {
                    const float wrist=VrSwingMelee::distance(ends->wrist,m_last->ends.wrist)/dt,tip=VrSwingMelee::distance(ends->tip,m_last->ends.tip)/dt;
                    if(VrSwingMelee::discontinuity(wrist,tip)) m_speeds.clear();
                    else m_speeds.push_back({pose->captured,(std::max)(wrist,tip)});
                } else m_speeds.clear();
            }
            m_last=Last{*ends,pose->captured,pose->epoch};
        }
        while(!m_speeds.empty() && now-m_speeds.front().first>PEAK_WINDOW) m_speeds.pop_front();
        return peak();
    }
    float peak() const { float best{};for(const auto& s:m_speeds) best=(std::max)(best,s.second);return best; }
private:
    struct Last { VrSwingMelee::Endpoints ends;Clock::time_point time;uint64_t epoch{}; };
    std::optional<Last> m_last;
    std::deque<std::pair<Clock::time_point,float>> m_speeds;
};

// Joe held the controllers together on the sword (21 Oct campaign trace: 0.16-0.24 m apart,
// moving together). Then the left hand is part of the swing: it must not veto it and its
// pose is not a guard. Left punches and guards in the 20 Oct session kept the hands 0.43 m
// or more apart. Stays true for HOLD after the hands part, since a two-handed swing
// separates them a little on the way through.
class VrTwoHanded {
public:
    using Clock=VrSwingMelee::Clock;
    static constexpr float METRES=0.30f;
    static constexpr auto HOLD=std::chrono::milliseconds(300);
    void reset() { m_until={}; }
    bool update(const std::optional<vr_camera::Matrix>& right,const std::optional<vr_camera::Matrix>& left,Clock::time_point now) {
        if(!right || !left) { reset();return false; }
        float squared{};for(int i=12;i<15;++i) squared+=std::pow((*right)[i]-(*left)[i],2);
        if(std::isfinite(squared) && squared<=METRES*METRES) m_until=now+HOLD;
        return now<m_until;
    }
private:
    Clock::time_point m_until{};
};

// Joe, 21 Oct evening (20:26): "power attack maybe is still too sensitive". In that session
// every power swing had the trigger fully pulled (1.0), so a higher threshold changes
// nothing; but 7 of 41 power swings had it pulled less than 150 ms before the swing
// committed, i.e. clenched during the stroke (his strokes take about 0.19 s from a slow hand
// to commitment, median). Power now needs the trigger held above POWER_TRIGGER for HOLD
// before the swing commits: pulled before the stroke started. Pulled later, the swing is
// light (the chainsword still revs).
class VrPowerTrigger {
public:
    using Clock=VrSwingMelee::Clock;
    static constexpr auto HOLD=std::chrono::milliseconds(250);
    void reset() { m_since={}; }
    // How long the raw right trigger has been above POWER_TRIGGER (zero when it is not).
    Clock::duration update(float trigger,Clock::time_point now) {
        if(!(trigger>VrSwingMelee::POWER_TRIGGER)) { m_since={};return {}; }
        if(m_since==Clock::time_point{} || now<m_since) m_since=now;
        return now-m_since;
    }
private:
    Clock::time_point m_since{};
};

// One poll of right-hand sword arbitration, shared by VrSession and tools/melee_replay.cpp
// so that a trace replay runs exactly the production decisions.
struct VrMeleePoll {
    std::optional<VrSwingMelee::Pose> right,left;
    bool allowed{};    // Sword loadout, focus, actor, armed, no snap, no holster selection.
    bool zone_block{}; // Hand in a holster zone (VrHolster::blocks_swing): no new swing starts.
    bool two_handed{}; // VrTwoHanded: the left hand holds the sword too.
    float trigger{};   // Raw right trigger: power above VrSwingMelee::POWER_TRIGGER...
    // ...held this long (VrPowerTrigger) at commitment, at least VrPowerTrigger::HOLD.
    VrSwingMelee::Clock::duration trigger_held{};
    bool gun{};        // The bolter is out (VrGunMeleeGate): light only, strong strokes only.
};
// Joe, 21 Oct: swings with the bolter out too, "so players can go easily from shooting to
// melee". Aiming and recoil must never attack, so with the gun a stroke must reach
// GUN_SPEED and the wrist itself GUN_WRIST_SPEED and GUN_WRIST_TRAVEL. Replayed on Joe's
// sessions with the bolter out: his three hard bolter swings committed at 6.3-9.6 m/s;
// lowering an arm from overhead reached 2.6-5.2 m/s, so a gun swing must also start below
// eye level; aiming and recoil never got that far.
// The right trigger is fire, so a gun swing is always light.
inline constexpr float GUN_SPEED=5.0f,GUN_WRIST_SPEED=2.5f,GUN_WRIST_TRAVEL=0.25f;
inline bool vr_melee_poll(VrSwingMelee& swing,VrHandMotion& left_motion,const VrMeleePoll& p,VrSwingMelee::Clock::time_point now) {
    const float left_speed=left_motion.update(p.left,swing.settings().values[2],now);
    VrSwingMelee::Commit commit{.allowed=!p.zone_block,.other_hand_speed=p.two_handed?0.f:left_speed};
    if(p.gun) { commit.min_speed=GUN_SPEED;commit.min_wrist_speed=GUN_WRIST_SPEED;commit.min_wrist_travel=GUN_WRIST_TRAVEL;commit.below_eyes=true; }
    return swing.update(p.right,p.allowed,now,!p.gun && p.trigger>VrSwingMelee::POWER_TRIGGER && p.trigger_held>=VrPowerTrigger::HOLD,commit);
}

// When a swing may start with the bolter (primary) out: both triggers released for at least
// QUIET (no aim down sights, no shot and no recoil settling), no two-handed support grip.
// Holster zones stay a hard gate with the gun (reaching for a holster is fast; the caller
// uses VrHolster::suppresses_swing).
class VrGunMeleeGate {
public:
    using Clock=VrSwingMelee::Clock;
    static constexpr auto QUIET=std::chrono::milliseconds(300);
    static constexpr float RELEASED=0.05f;
    void reset() { m_last_trigger={}; m_seen=false; }
    bool update(float right_trigger,float left_trigger,bool supporting,Clock::time_point now) {
        if(!std::isfinite(right_trigger) || !std::isfinite(left_trigger) || right_trigger>RELEASED || left_trigger>RELEASED || supporting) { m_last_trigger=now;m_seen=true; }
        return !m_seen || now-m_last_trigger>=QUIET;
    }
private:
    Clock::time_point m_last_trigger{};
    bool m_seen{};
};
