#pragma once
#include "VrGuard.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// Guard guide (docs/GUARD_GUIDE.md): a ghost bar in front of the face that shows where the
// left controller must be held, pointing across, to raise the guard (native parry/block).
// It is drawn into both eye images while the sword is out, and its colour says what the
// detector sees: idle (white), in the zone but not turned across (yellow), the pose is right
// but the guard will not start (orange: both hands on the sword, trigger pulled here, or a
// guard released for an attack must be lowered first), guard held (green).
//
// Every number that places it comes from VrGuard::Settings, so a retuned or loosened guard
// moves the guide with it. VrGuardGuide::entry repeats VrGuard's start test for a guard not
// yet raised; tests/test_vr_guard_guide.cpp pins it to VrGuard::update and pins the drawn
// target to a pose VrGuard accepts. This header only reads VrGuard; it never changes the rule.
struct VrGuardGuide {
    enum class State : uint8_t { hidden, idle, turn, blocked, active };
    static constexpr size_t STATES=5;
    static constexpr const char* name(State s) {
        switch(s) { case State::idle: return "idle"; case State::turn: return "turn"; case State::blocked: return "blocked"; case State::active: return "active"; default: return "hidden"; }
    }

    // Look only; where it sits is the guard's own zone. config/index_guard_guide.calibration.
    using Values=vr_body::Parameters<5>;
    enum Index : size_t { ENABLED,LENGTH,THICKNESS,GRIP,OPACITY };
    inline static constexpr std::array<Values::Parameter,5> PARAMETERS{{
        {"enabled",1,0,1},
        {"bar_length_metres",.16f,.08f,.6f},     // Whole bar, centred on the grip marker: about a controller held across.
        {"bar_thickness_metres",.01f,.005f,.06f},
        {"grip_metres",.03f,.02f,.12f},          // Square marker where the controller goes.
        {"opacity",.35f,.1f,1}}};                // Idle alpha; the other states are brighter.
    struct Settings {
        Values values=Values::defaults(PARAMETERS);
        bool operator==(const Settings&) const = default;
        float operator[](Index i) const { return values.values[i]; }
        bool enabled() const { return values.values[ENABLED]>=.5f; }
        bool valid() const { return values.in_range(PARAMETERS) && (values.values[ENABLED]==0 || values.values[ENABLED]==1); }
        static std::optional<Settings> read(std::istream& in) { const auto v=Values::read(in,PARAMETERS);if(!v) return {};Settings s{*v};return s.valid()?std::optional{s}:std::nullopt; }
        bool write(std::ostream& out) const { return valid() && values.write(out,PARAMETERS); }
        static std::optional<Settings> load(const std::filesystem::path& path) { std::ifstream in{path};return read(in); }
        bool save(const std::filesystem::path& path) const { if(!path.is_absolute() || !valid()) return false;std::ofstream out{path};if(!write(out)) return false;out.flush();return bool(out); }
    };

    // Where the guide puts the controller: inside the zone a guard may start in. Lateral is
    // the zone's middle and forward the middle of min_forward..max_entry_forward. Height is
    // 10 cm below the eyes, kept 5 cm inside the zone: the zone's middle (-0.175 m by default)
    // sits 38 degrees below the gaze at 0.225 m, the bottom edge of the Index view, while
    // -0.10 m is 24 degrees down. VrGuard::Settings::valid keeps every range at least 10 cm.
    static constexpr float PREFERRED_HEIGHT=-.1f,HEIGHT_INSET=.05f;
    static vr_body::Offset target(const VrGuard::Settings& g) {
        return {std::clamp(PREFERRED_HEIGHT,g[VrGuard::MIN_HEIGHT]+HEIGHT_INSET,g[VrGuard::MAX_HEIGHT]-HEIGHT_INSET),(g[VrGuard::MIN_LATERAL]+g[VrGuard::MAX_LATERAL])/2,
            (g[VrGuard::MIN_FORWARD]+g[VrGuard::MAX_ENTRY_FORWARD])/2};
    }
    // The drawn pointing axis: across (left or right both count), 90 degrees from the gaze,
    // which meets any min_angle (0..90). The bar is centred on the grip, so it shows the
    // controller held horizontally across the face without favouring a direction.
    static constexpr vr_body::Offset AXIS{0,1,0};

    // The left hand as VrGuard::update measures it.
    struct Measure { vr_body::Offset hand{}; float angle_degrees{}; };
    static std::optional<Measure> measure(const vr_camera::Matrix& head,const vr_camera::Matrix& hand) {
        if(!vr_camera::rigid(head) || !vr_camera::rigid(hand)) return {};
        const auto offset=vr_body::locate(head,hand);const auto axis=vr_body::pointing(head,hand);
        if(!offset || !axis) return {};
        return Measure{*offset,std::acos(std::clamp(axis->forward,-1.0f,1.0f))*57.2957795f};
    }
    // VrGuard's start test for a guard not yet raised (no exit margin or angle slack).
    struct Entry {
        bool in_zone{},close{},across{};
        bool pose() const { return in_zone && close && across; }
    };
    static Entry entry(const VrGuard::Settings& g,const Measure& m) {
        const auto& h=m.hand;
        Entry e;
        e.in_zone=h.height>=g[VrGuard::MIN_HEIGHT] && h.height<=g[VrGuard::MAX_HEIGHT] && h.forward>=g[VrGuard::MIN_FORWARD] && h.forward<=g[VrGuard::MAX_FORWARD] &&
            h.lateral>=g[VrGuard::MIN_LATERAL] && h.lateral<=g[VrGuard::MAX_LATERAL];
        e.close=h.forward<=g[VrGuard::MAX_ENTRY_FORWARD];
        e.across=m.angle_degrees>=g[VrGuard::MIN_ANGLE];
        return e;
    }

    struct Inputs {
        bool guide_enabled{},guard_enabled{};
        bool sword{};   // VrCombatRouting::sword_out: the pistol/chainsword loadout, debounced.
        bool allowed{}; // Gameplay input owned (foreground, armed, fresh sample).
        VrGuard::Snapshot guard{}; // After this poll's VrGuard::update.
        std::optional<Measure> left;
    };
    static State classify(const VrGuard::Settings& g,const Inputs& in) {
        if(!in.guide_enabled || !in.guard_enabled || !in.sword || !in.allowed) return State::hidden;
        if(in.guard.held) return State::active; // The detector's own output: LB is held.
        if(!in.left) return State::idle;
        const auto e=entry(g,*in.left);
        if(!e.in_zone || !e.close) return State::idle;
        return e.across?State::blocked:State::turn;
    }

    static std::array<float,4> colour(State s,float opacity) {
        const auto a=[&](float scale){return std::clamp(opacity*scale,0.f,1.f);};
        switch(s) {
        case State::idle: return {.85f,.9f,1,a(1)};
        case State::turn: return {1,.82f,.2f,a(1.4f)};
        case State::blocked: return {1,.38f,.18f,a(1.4f)};
        case State::active: return {.3f,1,.45f,a(1.8f)};
        default: return {0,0,0,0};
        }
    }

    // A flat quad facing the player, corners in the head-yaw frame.
    struct Quad { std::array<vr_body::Offset,4> corners{}; };
    struct Draw {
        State state{};
        std::array<Quad,3> quads{}; // Grip marker, then the bar to its right and to its left.
        std::array<float,4> colour{};
    };
    static Quad rectangle(float lateral0,float lateral1,float height0,float height1,float forward) {
        return {{{{height0,lateral0,forward},{height1,lateral0,forward},{height1,lateral1,forward},{height0,lateral1,forward}}}};
    }
    static std::optional<Draw> draw(State s,const VrGuard::Settings& g,const Settings& look) {
        if(s==State::hidden || !look.valid() || !look.enabled() || !g.valid()) return {};
        const auto t=target(g);
        const float grip=look[GRIP]/2,bar=look[THICKNESS]/2;
        Draw d;d.state=s;d.colour=colour(s,look[OPACITY]);
        d.quads[0]=rectangle(t.lateral-grip,t.lateral+grip,t.height-grip,t.height+grip,t.forward);
        const float half=std::max(look[LENGTH]/2,grip);
        d.quads[1]=rectangle(t.lateral+grip,t.lateral+half,t.height-bar,t.height+bar,t.forward);
        d.quads[2]=rectangle(t.lateral-half,t.lateral-grip,t.height-bar,t.height+bar,t.forward);
        return d;
    }

    // Head-yaw frame offset to standing space, the inverse of vr_body::locate.
    static std::optional<std::array<float,3>> world(const vr_camera::Matrix& head,const vr_body::Offset& o) {
        const auto f=vr_body::yaw_forward(head);if(!f) return {};
        return std::array<float,3>{head[12]+o.forward*(*f)[0]-o.lateral*(*f)[1],head[13]+o.height,head[14]+o.forward*(*f)[1]+o.lateral*(*f)[0]};
    }
    // Clip-space corners (x,y,z,w per corner) of a quad in one eye. head is the eye image's
    // runtime head, eye_to_head its eye offset, projection the game-style (+Z forward) eye
    // projection. z is set so the rasterizer clips the quad 1 cm in front of the eye.
    static constexpr float NEAR_METRES=.01f;
    static std::optional<std::array<float,16>> clip(const Quad& q,const vr_camera::Matrix& head,const vr_camera::Matrix& eye_to_head,const vr_camera::Matrix& projection) {
        if(!vr_camera::rigid(head) || !vr_camera::rigid(eye_to_head)) return {};
        for(float v:projection) if(!std::isfinite(v)) return {};
        const auto view=vr_camera::inverse(vr_camera::multiply(eye_to_head,head));
        std::array<float,16> out{};
        for(size_t c=0;c<4;++c) {
            const auto p=world(head,q.corners[c]);if(!p) return {};
            float e[3];
            for(int j=0;j<3;++j) e[j]=(*p)[0]*view[j]+(*p)[1]*view[4+j]+(*p)[2]*view[8+j]+view[12+j];
            const float g[4]{e[0],e[1],-e[2],1}; // XR eye (right, up, back) to game (right, up, forward).
            for(int j=0;j<4;++j) out[c*4+j]=g[0]*projection[j]+g[1]*projection[4+j]+g[2]*projection[8+j]+g[3]*projection[12+j];
            out[c*4+2]=out[c*4+3]-NEAR_METRES*projection[11];
            for(int j=0;j<4;++j) if(!std::isfinite(out[c*4+j])) return {};
        }
        return out;
    }

    // Where the grip marker's centre lands in a width x height eye image (snapshot evidence).
    static std::optional<std::array<float,2>> pixel(const Draw& d,const vr_camera::Matrix& head,const vr_camera::Matrix& eye_to_head,const vr_camera::Matrix& projection,float width,float height) {
        const auto c=clip(d.quads[0],head,eye_to_head,projection);if(!c) return {};
        float x=0,y=0;for(int k=0;k<4;++k) { if(!((*c)[k*4+3]>0)) return {};x+=(*c)[k*4]/(*c)[k*4+3]/4;y+=(*c)[k*4+1]/(*c)[k*4+3]/4; }
        return std::array<float,2>{(x+1)/2*width,(1-y)/2*height};
    }
    // JSON fields for a snapshot's pair.json eye entry: ,"guard_guide":"state"[,"guard_guide_pixel":[x,y]].
    static std::string json(const std::optional<Draw>& d,const vr_camera::Matrix& head,const vr_camera::Matrix& eye_to_head,const vr_camera::Matrix& projection,float width,float height) {
        std::string out=std::string{",\"guard_guide\":\""}+name(d?d->state:State::hidden)+"\"";
        if(d) if(const auto p=pixel(*d,head,eye_to_head,projection,width,height)) out+=",\"guard_guide_pixel\":["+std::to_string((*p)[0])+","+std::to_string((*p)[1])+"]";
        return out;
    }

    // VrSession's per-poll state (under its mutex). The renderer only draws a fresh frame, so
    // pause, viewer, stalled tracking and lost focus (polls that skip update) hide the guide.
    class Tracker {
    public:
        using Clock=std::chrono::steady_clock;
        static constexpr auto FRESH=std::chrono::milliseconds(150);
        struct Status { State state{}; std::array<uint64_t,STATES> polls{}; uint64_t changes{}; };
        void enable(bool value) { m_enabled=value;m_draw.reset(); }
        bool enabled() const { return m_enabled; }
        bool set_settings(const Settings& value) { if(!value.valid()) return false;m_settings=value;m_draw.reset();return true; }
        Settings settings() const { return m_settings; }
        State update(const VrGuard::Settings& guard,Inputs in,Clock::time_point now) {
            in.guide_enabled=m_enabled && m_settings.enabled();
            const auto state=classify(guard,in);
            ++m_status.polls[size_t(state)];
            if(state!=m_status.state) { ++m_status.changes;m_status.state=state; }
            m_draw=draw(state,guard,m_settings);m_at=now;
            return state;
        }
        std::optional<Draw> current(Clock::time_point now) const {
            return m_draw && now>=m_at && now-m_at<=FRESH?m_draw:std::nullopt;
        }
        Status status() const { return m_status; }
    private:
        bool m_enabled{};Settings m_settings;std::optional<Draw> m_draw;Clock::time_point m_at{};Status m_status;
    };
};
