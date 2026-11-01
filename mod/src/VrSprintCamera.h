#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

// Sprint-only forward eye offset (docs/SPRINT_CAMERA.md). The native sprint animation
// leans the marine's upper body forward: in Joe's 20 Oct OpenXR trace BACKup moves
// +0.22 m, NECK +0.44 m and HEAD +0.51 m further ahead of the body root than walking,
// while the tracked eye stays on its root-relative anchor. The eye then sits level with
// the upper back (1.7 cm ahead of BACKup, 22 cm walking) and the backpack fills the view. While the player is actually sprinting
// this adds a forward offset to the accepted head_forward_offset_metres placement (same
// level-heading axis, both eyes and controller origins), ramped in and out so it never
// snaps. Pure bookkeeping; VrSession owns the clock and applies the offset.
class VrSprintCamera {
public:
    using Clock=std::chrono::steady_clock;
    struct Settings {
        float offset_metres{0.20f};          // Calibration sprint_forward_offset_metres; 0 disables. Joe chose 0.20 m (21 Oct).
        float blend_seconds{0.25f};          // Calibration sprint_blend_seconds; full ramp in or out.
        // Native ground speed (physical m/s at the active units_per_metre). Driver runs
        // measured native sprint 8.36-8.74 m/s and full-stick running 5.78-5.81 m/s.
        float confirm_speed{7.0f};           // At or above: native sprint confirmed.
        float release_speed{6.2f};           // Below, after confirming: no longer sprinting.
        // Native lean develops 0.1-0.2 s after the sprint press while speed is still
        // rising; start the ramp at the press but drop it if the game never confirms.
        float onset_grace_seconds{0.4f};
        // After a confirmed sprint ends the native stop animation leans further forward
        // (BACKup up to +0.42 m, peak ~0.55 s, back to standing by ~1.1 s): hold the
        // offset through it before ramping out.
        float release_hold_seconds{0.7f};
        bool valid() const {
            return std::isfinite(offset_metres) && offset_metres>=0 && offset_metres<=0.4f &&
                std::isfinite(blend_seconds) && blend_seconds>=0.05f && blend_seconds<=1.5f &&
                std::isfinite(confirm_speed) && std::isfinite(release_speed) && release_speed>0 && release_speed<=confirm_speed &&
                std::isfinite(onset_grace_seconds) && onset_grace_seconds>=0 && onset_grace_seconds<=2 &&
                std::isfinite(release_hold_seconds) && release_hold_seconds>=0 && release_hold_seconds<=3;
        }
        bool operator==(const Settings&) const = default;
    };
    struct Snapshot {
        bool requested{},native_known{},confirmed{},active{};
        float weight{},offset_metres{};
        std::optional<float> speed_metres_per_second;
        uint64_t engagements{},refusals{};
    };
    // Advance the ramp under the old blend first so a new blend time cannot jump it.
    bool set_settings(const Settings& value,Clock::time_point now) { if(!value.valid()) return false; advance(now); m_settings=value; return true; }
    Settings settings() const { return m_settings; }
    // requested: the mod is holding native sprint (LS) for the game this poll.
    // speed: native horizontal body speed in physical metres/second, nullopt when the
    // body is not observed (then the mod's own sprint state decides alone).
    void update(bool requested,std::optional<float> speed,Clock::time_point now) {
        advance(now);
        if(speed && !(std::isfinite(*speed) && *speed>=0)) speed.reset();
        if(requested && !m_requested) { m_request_start=now; m_confirmed=false; m_hold_until={}; }
        if(requested) {
            if(speed && *speed>=m_settings.confirm_speed) m_confirmed=true;
            else if(speed && *speed<m_settings.release_speed) m_confirmed=false;
        }
        const bool grace=requested && now-m_request_start<seconds(m_settings.onset_grace_seconds);
        bool target=requested && (!speed || m_confirmed || grace);
        if(requested && speed && !target && m_target) ++m_refusals;
        // A sprint that really ran (native confirmed, or the mod alone when unobserved)
        // keeps the offset through the native stop lean.
        // A short unconfirmed press (still inside the onset grace) ramps straight out.
        if(!requested && m_requested && m_target && (m_confirmed || !m_native_known)) m_hold_until=now+seconds(m_settings.release_hold_seconds);
        if(!requested && now<m_hold_until) target=true;
        if(target && !m_target) ++m_engagements;
        m_requested=requested;m_target=target;m_speed=speed;m_native_known=speed.has_value();
        // A hold ends at its own time, not at the next poll, so the ramp-out is poll-rate independent.
        m_target_until=target && !requested?std::optional{m_hold_until}:std::nullopt;
        if(!requested) m_confirmed=false;
    }
    // Continuous in time between polls: eyes are composed at render rate.
    float weight(Clock::time_point now) const { return ramp(now); }
    float offset_metres(Clock::time_point now) const {
        const float w=ramp(now);
        return m_settings.offset_metres*w*w*(3-2*w); // Smoothstep: no velocity step at either end.
    }
    Snapshot snapshot(Clock::time_point now) const {
        return {m_requested,m_native_known,m_confirmed,m_target,ramp(now),offset_metres(now),m_speed,m_engagements,m_refusals};
    }
    // Drop to no offset immediately (recenter or mode change already discontinuous).
    void reset(Clock::time_point now) { m_weight=0;m_time=now;m_target=m_requested=m_confirmed=false;m_hold_until={};m_target_until.reset();m_speed.reset();m_native_known=false; }
private:
    static Clock::duration seconds(float value) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(value)); }
    float ramp(Clock::time_point now) const {
        if(m_time==Clock::time_point{} || now<=m_time) return m_weight;
        const auto rate=[&](Clock::time_point from,Clock::time_point to){return std::chrono::duration<float>(to-from).count()/m_settings.blend_seconds;};
        if(!m_target) return std::clamp(m_weight-rate(m_time,now),0.0f,1.0f);
        const auto on_until=m_target_until?std::clamp(*m_target_until,m_time,now):now;
        const float up=std::clamp(m_weight+rate(m_time,on_until),0.0f,1.0f);
        return std::clamp(up-rate(on_until,now),0.0f,1.0f);
    }
    void advance(Clock::time_point now) { m_weight=ramp(now); if(now>m_time) m_time=now; }
    Settings m_settings;
    bool m_requested{},m_confirmed{},m_target{},m_native_known{};
    float m_weight{};
    Clock::time_point m_time{},m_request_start{},m_hold_until{};
    std::optional<Clock::time_point> m_target_until;
    std::optional<float> m_speed;
    uint64_t m_engagements{},m_refusals{};
};
