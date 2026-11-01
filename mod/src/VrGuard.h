#pragma once
#include "VrBodyFrame.h"
#include <chrono>

// Left-hand guard: raising the left controller in front of the face/chest with its
// pointing axis turned across or up (a forearm guard, not a pistol-support grip) holds
// the native parry/block button for as long as the pose is held. A quick raise is a
// tap (parry); holding it is a hold (block). The game still decides what happens.
// Entering the pose is a zone test on the current sample, so it adds no detection
// latency to a timing-critical parry.
//
// Defaults from Joe's 20 October OpenXR trace (docs/validation/melee-trace-2026-10-20.md):
// his real guards started with the hand at most 0.17 m in front of the eyes and the
// controller 63-89 degrees across; pistol holds and punches entered 0.22-0.68 m out at
// 50-62 degrees. A guard must now start close to the face (max_entry_forward_metres) and
// at least 62 degrees across (58 since 21 Oct evening, below); once raised it may drift out to max_forward_metres, because
// a held block drifts outwards and a tight outer limit split one block into several new
// parries. Replayed on that trace: 40 guard starts became 14.
//
// Joe's 21 Oct evening campaign ("I can't figure out a parry", "still cannot figure out guard"):
// 27 guards started, but 8 more raises held close to the face peaked at 58-61.6 degrees, some
// for 2-4 s (375.5, 971.1 s), so min_angle is now 58. Replayed: evening guard starts 37 -> 45;
// his 20 Oct punch/pistol session 19 -> 21.
//
// Joe, 23 Oct: the zone grew to reach further from the face and higher: max_entry_forward
// 0.30 -> 0.38 m (still short of VrLeftPush's 0.40 m end point) and max_height 0.20 -> 0.30 m.
class VrGuard {
public:
    using Clock=std::chrono::steady_clock;
    using Values=vr_body::Parameters<10>;
    enum Index { MIN_HEIGHT,MAX_HEIGHT,MIN_FORWARD,MAX_FORWARD,MIN_LATERAL,MAX_LATERAL,MIN_ANGLE,EXIT_MARGIN,MIN_HOLD,MAX_ENTRY_FORWARD };
    static constexpr std::array<Values::Parameter,10> PARAMETERS{{
        {"min_height_metres",-0.55f,-0.9f,0}, {"max_height_metres",0.3f,-0.3f,0.5f},
        {"min_forward_metres",0.15f,0,0.5f}, {"max_forward_metres",0.75f,0.3f,1},
        {"min_lateral_metres",-0.5f,-0.8f,0}, {"max_lateral_metres",0.3f,-0.1f,0.6f},
        {"min_angle_degrees",58,0,90}, {"exit_margin_metres",0.05f,0,0.15f}, {"min_hold_ms",80,30,250},
        {"max_entry_forward_metres",0.38f,0.1f,1}}};
    static constexpr float AIM_RELEASE=0.05f, EXIT_ANGLE_SLACK=10;
    struct Settings {
        Values values=Values::defaults(PARAMETERS);
        bool operator==(const Settings&) const = default;
        float operator[](Index i) const { return values.values[i]; }
        bool valid() const {
            const auto& v=values.values;
            return values.in_range(PARAMETERS) && v[MIN_HEIGHT]+0.1f<=v[MAX_HEIGHT] && v[MIN_FORWARD]+0.1f<=v[MAX_FORWARD] && v[MIN_LATERAL]+0.1f<=v[MAX_LATERAL] &&
                v[MIN_FORWARD]+0.05f<=v[MAX_ENTRY_FORWARD] && v[MAX_ENTRY_FORWARD]<=v[MAX_FORWARD];
        }
        static std::optional<Settings> read(std::istream& in) { const auto v=Values::read(in,PARAMETERS);if(!v) return {};Settings s{*v};return s.valid()?std::optional{s}:std::nullopt; }
        bool write(std::ostream& out) const { return valid() && values.write(out,PARAMETERS); }
        static std::optional<Settings> load(const std::filesystem::path& path) { std::ifstream in{path};return read(in); }
        bool save(const std::filesystem::path& path) const { if(!path.is_absolute() || !valid()) return false;std::ofstream out{path};if(!write(out)) return false;out.flush();return bool(out); }
    };
    struct Snapshot {
        vr_body::Offset hand{}; float angle_degrees{};
        bool in_zone{},pose{},held{},aim_latched{};
        uint64_t guards{},angle_rejections{}; // Zone entries refused because the controller pointed forward.
        uint64_t yields{}; // Held guards released for a sword attack (see yield).
        float last_duration_ms{};
    };
    bool set_settings(const Settings& value) { if(!value.valid()) return false;m_settings=value;reset();return true; }
    Settings settings() const { return m_settings; }
    Snapshot snapshot() const { return m_state; }
    // Start latched: a guard already raised at enable/recovery must be lowered first.
    void reset() { m_latched=true;m_state.in_zone=m_state.pose=m_state.held=m_state.aim_latched=false;m_hold_until={};m_zone_counted=false; }
    // A sword attack was just sent. The attack pulse already masks LB; re-pressing it when
    // the pulse ends would be a fresh native parry, which also aborts the attack
    // (Game.AbortMelee listens to MeleeParry). Playtest 20 Oct: 4 of 37 swings did that.
    // The raised guard therefore lets go and must be lowered before it can block again.
    void yield(Clock::time_point now) {
        if(!m_state.pose && !m_state.held) return;
        ++m_state.yields;
        const bool was=m_state.pose;reset();if(was) finish(now);
    }

    // The relaxed left hand hangs in the grenade belt zone (VrGrenadeThrow: 0.4-1.1 m below the
    // eyes, within 0.35 m of the body), where the grip belongs to the grenade. The guard waits
    // there but does not latch: a lowered hand needs no lowering first. Until 26 Oct the belt
    // reset (latched) the guard on every poll, so a fast raise that was already turned across
    // when it left the belt never pressed LB until it was lowered and raised again. Playtest
    // 26 Oct (docs/PARRY_TIMING_2026-10-26.md): replayed, 27 raises into the guard pose (16 Vanguard,
    // 11 Bulwark) never pressed LB, and 19 of the 36 Bulwark guards only started after a
    // forward-pointing sample had cleared the latch.
    // A grenade being held or thrown (VrGrenadeThrow::busy) still resets and latches.
    bool rest(Clock::time_point now) {
        const bool was=m_state.pose;
        m_latched=false;m_zone_counted=false;m_state.in_zone=m_state.pose=m_state.aim_latched=false;
        if(was) finish(now);
        m_state.held=now<m_hold_until;
        return m_state.held;
    }

    // One poll with the grenade belt state (VrSession and scripts/melee_replay.cpp call this).
    // at_belt: the hand is in the belt zone with no grenade transaction (that one is allowed=false).
    // A guard can only start outside the belt; a raised guard keeps the hand when it sinks into
    // the belt's top (-0.40 m) until it leaves its own zone, so a hand hovering at the boundary
    // cannot flicker LB. belt_latch restores the old rule (SM2VR_GUARD_BELT_LATCH=1).
    bool poll(const std::optional<vr_camera::Matrix>& head,const std::optional<vr_camera::Matrix>& hand,float aim,bool allowed,bool at_belt,bool belt_latch,Clock::time_point now) {
        if(allowed && at_belt) {
            if(belt_latch) return update(head,hand,aim,false,now);
            if(!m_state.pose) return rest(now);
        }
        return update(head,hand,aim,allowed,now);
    }

    // head/hand are raw standing-space matrices; aim is the left trigger (aim down sights).
    bool update(const std::optional<vr_camera::Matrix>& head,const std::optional<vr_camera::Matrix>& hand,float aim,bool allowed,Clock::time_point now) {
        if(!allowed || !head || !hand || !vr_camera::rigid(*head) || !vr_camera::rigid(*hand) || !std::isfinite(aim)) { const bool was=m_state.pose;reset();if(was) finish(now);return false; }
        const auto offset=vr_body::locate(*head,*hand);const auto axis=vr_body::pointing(*head,*hand);
        if(!offset || !axis) { const bool was=m_state.pose;reset();if(was) finish(now);return false; }
        m_state.hand=*offset;
        m_state.angle_degrees=std::acos(std::clamp(axis->forward,-1.0f,1.0f))*57.2957795f;
        const float margin=m_state.pose?m_settings[EXIT_MARGIN]:0;
        const auto& h=*offset;
        m_state.in_zone=h.height>=m_settings[MIN_HEIGHT]-margin && h.height<=m_settings[MAX_HEIGHT]+margin &&
            h.forward>=m_settings[MIN_FORWARD]-margin && h.forward<=m_settings[MAX_FORWARD]+margin &&
            h.lateral>=m_settings[MIN_LATERAL]-margin && h.lateral<=m_settings[MAX_LATERAL]+margin;
        const float angle_needed=m_settings[MIN_ANGLE]-(m_state.pose?EXIT_ANGLE_SLACK:0);
        // The left trigger aims or fires the pistol, held out in this zone between shots.
        // Letting go of the trigger must not parry: once it is pulled, the guard waits
        // until the hand leaves the zone. Playtest 20 Oct: 13 of 38 guard starts came
        // within 25 ms of a pistol trigger release, a parry between every shot.
        if(aim>AIM_RELEASE) m_state.aim_latched=true;
        else if(!m_state.in_zone) m_state.aim_latched=false;
        const bool close=m_state.pose || h.forward<=m_settings[MAX_ENTRY_FORWARD];
        const bool pose=m_state.in_zone && close && m_state.angle_degrees>=angle_needed && !m_state.aim_latched;
        if(m_state.in_zone && close && m_state.angle_degrees<angle_needed && !m_zone_counted && !m_state.aim_latched && !m_latched) { ++m_state.angle_rejections;m_zone_counted=true; }
        if(!m_state.in_zone) m_zone_counted=false;
        if(m_latched) { if(!pose) m_latched=false;m_state.held=now<m_hold_until;return m_state.held; }
        if(pose && !m_state.pose) { ++m_state.guards;m_started=now;m_hold_until=now+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float,std::milli>{m_settings[MIN_HOLD]}); }
        if(!pose && m_state.pose) finish(now);
        m_state.pose=pose;
        m_state.held=pose || now<m_hold_until;
        return m_state.held;
    }
    // While raised, the left hand belongs to the guard (the grenade gesture must not start).
    bool owns_hand() const { return m_state.pose; }
private:
    void finish(Clock::time_point now) { m_state.last_duration_ms=std::chrono::duration<float,std::milli>(now-m_started).count(); }
    Settings m_settings;
    Snapshot m_state;
    bool m_latched{true},m_zone_counted{};
    Clock::time_point m_hold_until{},m_started{};
};
