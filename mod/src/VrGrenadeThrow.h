#pragma once
#include "VrBodyFrame.h"
#include "VrGrenadeAim.h"
#include <chrono>
#include <deque>

// Left-hand grenade: squeeze the left grip at waist height near the body to take a
// grenade, then let go of the grip during a forward throw. A fast forward release sends
// the native equipment button; a slow release puts the grenade back (nothing is sent).
// In hold mode the native button is held from the grab until the release instead, for
// games/presets that aim on hold and throw on release (that mode cannot cancel).
// Motion is measured relative to the head so walking or leaning cannot fake a throw.
class VrGrenadeThrow {
public:
    using Clock=std::chrono::steady_clock;
    using Values=vr_body::Parameters<8>;
    enum Index { MAX_HEIGHT,MIN_HEIGHT,BELT_RADIUS,THROW_SPEED,FORWARD_FRACTION,PULSE,HOLD_BUTTON,MAX_HOLD };
    static constexpr std::array<Values::Parameter,8> PARAMETERS{{
        {"max_height_metres",-0.4f,-0.9f,0}, {"min_height_metres",-1.1f,-1.6f,-0.4f},
        {"belt_radius_metres",0.35f,0.1f,0.6f},
        {"throw_speed_metres_per_second",1.5f,0.5f,5}, {"forward_fraction",0.3f,0,1},
        {"pulse_ms",120,60,300}, {"hold_button",0,0,1}, {"max_hold_seconds",8,1,30}}};
    static constexpr float PRESS=0.65f, RELEASE=0.35f, MAX_SPEED=12, GUARD_CLEAR_HEIGHT=-0.55f;
    static constexpr auto WINDOW=std::chrono::milliseconds(100);
    struct Settings {
        Values values=Values::defaults(PARAMETERS);
        bool operator==(const Settings&) const = default;
        float operator[](Index i) const { return values.values[i]; }
        bool hold_mode() const { return values.values[HOLD_BUTTON]>=0.5f; }
        bool valid() const { return values.in_range(PARAMETERS) && values.values[MIN_HEIGHT]+0.1f<=values.values[MAX_HEIGHT]; }
        static std::optional<Settings> read(std::istream& in) {
            // Complete old configs migrate deliberately: the old forward reach becomes
            // the horizontal belt radius. The old gaze-relative lateral sign is removed.
            // The two strict schemas reject mixed, duplicate, unknown and partial files.
            const std::string text{std::istreambuf_iterator<char>{in},std::istreambuf_iterator<char>{}};
            if(in.bad()) return {};
            std::istringstream current{text};
            if(const auto v=Values::read(current,PARAMETERS)) {Settings s{*v};return s.valid()?std::optional{s}:std::nullopt;}
            using Legacy=vr_body::Parameters<9>;
            static constexpr std::array<Legacy::Parameter,9> legacy{{
                {"max_height_metres",-0.4f,-0.9f,0},{"min_height_metres",-1.1f,-1.6f,-0.4f},
                {"max_lateral_metres",-0.05f,-0.5f,0.2f},{"max_abs_forward_metres",0.35f,0.1f,0.6f},
                {"throw_speed_metres_per_second",1.5f,0.5f,5},{"forward_fraction",0.3f,0,1},
                {"pulse_ms",120,60,300},{"hold_button",0,0,1},{"max_hold_seconds",8,1,30}}};
            std::istringstream previous{text};const auto old=Legacy::read(previous,legacy);if(!old) return {};
            Settings s;const auto& v=old->values;
            s.values.values={v[0],v[1],v[3],v[4],v[5],v[6],v[7],v[8]};
            return s.valid()?std::optional{s}:std::nullopt;
        }
        bool write(std::ostream& out) const { return valid() && values.write(out,PARAMETERS); }
        static std::optional<Settings> load(const std::filesystem::path& path) { std::ifstream in{path};return read(in); }
        bool save(const std::filesystem::path& path) const { if(!path.is_absolute() || !valid()) return false;std::ofstream out{path};if(!write(out)) return false;out.flush();return bool(out); }
    };
    enum class Outcome { none,grab,thrown,cancelled,timeout,lost };
    struct Snapshot {
        vr_body::Offset hand{}; bool in_zone{},holding{},pressed{},recovering{};
        uint64_t grabs{},throws{},cancels{};
        float speed{}; vr_body::Offset velocity{}; // Head-yaw frame m/s over the last 100 ms.
        float last_release_speed{},last_forward_fraction{};
        VrGrenadeAim::Aim last_aim{}; // Launch direction chosen at the last throw.
        Outcome last{Outcome::none};
    };
    bool set_settings(const Settings& value) { if(!value.valid()) return false;m_settings=value;reset();return true; }
    Settings settings() const { return m_settings; }
    bool set_aim_settings(const VrGrenadeAim::Settings& value) { if(!value.valid()) return false;m_aim=value;return true; }
    VrGrenadeAim::Settings aim_settings() const { return m_aim; }
    Snapshot snapshot() const { return m_state; }
    void reset() {
        if(m_state.holding) { ++m_state.cancels;m_state.last=Outcome::lost; }
        m_grip=true;m_state.holding=m_state.pressed=m_state.in_zone=m_state.recovering=false;m_points.clear();m_pulse_until={};
    }
    // grip is the left grip force. Returns whether the native equipment button is down.
    bool update(const std::optional<vr_camera::Matrix>& head,const std::optional<vr_camera::Matrix>& hand,float grip,bool allowed,Clock::time_point now,std::optional<Clock::time_point> captured=std::nullopt) {
        if(!allowed || !head || !hand || !vr_camera::rigid(*head) || !vr_camera::rigid(*hand) || !std::isfinite(grip)) { reset();return false; }
        const auto offset=vr_body::locate(*head,*hand);const auto forward=vr_body::yaw_forward(*head);
        if(!offset || !forward) { reset();return false; }
        m_state.hand=*offset;
        const auto& h=*offset;
        // There is no torso tracker. The left controller identifies the hand, and
        // a bounded waist cylinder stays put through head-only looks and body turns.
        // In particular, looking down/sideways cannot turn a near-centre belt squeeze
        // into parry just because its gaze-relative lateral component changed sign.
        m_state.in_zone=h.height<=m_settings[MAX_HEIGHT] && h.height>=m_settings[MIN_HEIGHT] &&
            std::hypot(h.lateral,h.forward)<=m_settings[BELT_RADIUS];
        track(*head,*hand,*forward,captured.value_or(now)); // Motion is timed by tracking capture, not by polls.
        if(grip>=PRESS && !m_grip) {
            m_grip=true;
            if(m_state.in_zone) { m_state.holding=m_state.recovering=true;++m_state.grabs;m_state.last=Outcome::grab;m_grabbed=now; }
        } else if(grip<=RELEASE && m_grip) {
            m_grip=false;
            if(m_state.holding) {
                m_state.holding=false;
                m_state.last_release_speed=m_state.speed;
                m_state.last_forward_fraction=m_state.speed>0?m_state.velocity.forward/m_state.speed:0;
                if(m_state.speed>=m_settings[THROW_SPEED] && m_state.last_forward_fraction>=m_settings[FORWARD_FRACTION]) {
                    ++m_state.throws;m_state.last=Outcome::thrown;m_state.last_aim=VrGrenadeAim::aim(m_state.velocity,m_aim);
                    if(!m_settings.hold_mode()) m_pulse_until=now+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float,std::milli>{m_settings[PULSE]});
                } else { ++m_state.cancels;m_state.last=Outcome::cancelled; }
            }
        }
        if(m_state.holding && now-m_grabbed>std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>{m_settings[MAX_HOLD]})) {
            m_state.holding=false;++m_state.cancels;m_state.last=Outcome::timeout;
        }
        // A throw/cancel/timeout owns the whole grip transaction, including release.
        // Keep its raised follow-through out of guard/support until released and
        // lowered below the default guard entry (or the lower tuned belt ceiling).
        if(m_state.recovering && !m_state.holding && !m_grip && h.height<=std::min(GUARD_CLEAR_HEIGHT,m_settings[MAX_HEIGHT])) m_state.recovering=false;
        m_state.pressed=now<m_pulse_until || (m_settings.hold_mode() && m_state.holding);
        return m_state.pressed;
    }
    // The left grip belongs to the grenade (never parry) in the belt zone, while a grenade
    // is held, through release/follow-through, and while its button pulse is out.
    bool owns_grip() const { return m_state.in_zone || busy(); }
    // A grenade transaction (held, its pulse out, or the follow-through until lowered), as
    // opposed to a hand merely resting in the belt zone (VrGuard::rest).
    bool busy() const { return m_state.holding || m_state.pressed || m_state.recovering; }
private:
    void track(const vr_camera::Matrix& head,const vr_camera::Matrix& hand,const std::array<float,2>& forward,Clock::time_point captured) {
        const std::array<float,3> point{hand[12]-head[12],hand[13]-head[13],hand[14]-head[14]};
        if(!m_points.empty()) {
            const auto& last=m_points.back();
            const float dt=std::chrono::duration<float>(captured-last.time).count();
            if(dt<=0) return; // A repeated cached pose (same capture) carries no new motion.
            const float step=std::hypot(point[0]-last.position[0],point[1]-last.position[1],point[2]-last.position[2]);
            if(dt>WINDOW.count()/1000.0f || step/dt>MAX_SPEED) m_points.clear(); // Tracking gap or jump, never a throw.
        }
        m_points.push_back({point,captured});
        while(captured-m_points.front().time>WINDOW) m_points.pop_front();
        m_state.speed=0;m_state.velocity={};
        if(m_points.size()<2) return;
        const auto& first=m_points.front();
        const float dt=std::chrono::duration<float>(captured-first.time).count();
        if(dt<=0) return;
        const float vx=(point[0]-first.position[0])/dt,vy=(point[1]-first.position[1])/dt,vz=(point[2]-first.position[2])/dt;
        m_state.velocity=vr_body::direction(forward,vx,vy,vz);
        m_state.speed=std::hypot(vx,vy,vz);
    }
    struct Point { std::array<float,3> position; Clock::time_point time; };
    Settings m_settings;
    VrGrenadeAim::Settings m_aim;
    Snapshot m_state;
    bool m_grip{true}; // Start latched: a squeeze held at enable/recovery must be released first.
    std::deque<Point> m_points;
    Clock::time_point m_pulse_until{},m_grabbed{};
};
