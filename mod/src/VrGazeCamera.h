#pragma once
#include "VrBodyFrame.h"
#include "VrCameraPose.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <mutex>
#include <optional>

// The game picks what Right B (native B, "Game.Use") interacts with from the native
// gameplay camera, not from the VR head (docs/INTERACTION.md):
//  - use eligibility: a 90 degree cone along the camera's heading from 0.5 m above the
//    player, within the entry point's use radius (2.5 m), or a 0.3 m cylinder around the
//    player (ssl/ai/vision/prop_authority_player_vision.prop, viewer "usable_radius");
//  - focus: the eligible entry point in the camera's 25x25 degree cone nearest the
//    crosshair on screen (prop_authority_entry_point_focus_controller, viewer
//    "3dmarker_focus", GetNearestTargetToCrosshair);
//  - the "hold X" world marker is drawn by the native UI from that camera.
// The mod keeps the native camera's yaw and pitch out of the rendered view, so the native
// camera points wherever the game left it. Alignment measures the gaze relative to it.
namespace vr_gaze {
using Matrix=vr_camera::Matrix;
using Vector=std::array<float,3>;
inline constexpr float DEGREES=57.2957795131f;

// Gaze relative to the native camera, degrees. yaw > 0: the gaze is to the camera's
// right (its row-0 axis); pitch > 0: the gaze is above the camera's forward.
struct Alignment { float yaw_degrees{},pitch_degrees{}; };

// native_camera: the game's camera-to-world matrix (rows right, up, forward; world Y-up).
// gaze_forward: the composed eye's forward (row 2 of the rendered eye) in the same world.
inline std::optional<Alignment> alignment(const Matrix& native_camera,const Vector& gaze_forward) {
    const Vector right{native_camera[0],native_camera[1],native_camera[2]},forward{native_camera[8],native_camera[9],native_camera[10]};
    for(const auto* v:{&right,&forward,&gaze_forward}) for(float x:*v) if(!std::isfinite(x)) return {};
    const auto length=[](const Vector& v){return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);};
    const float gaze_length=length(gaze_forward),forward_length=length(forward);
    if(gaze_length<0.5f || forward_length<0.5f) return {};
    // Heading only: project the camera's forward and right and the gaze onto the floor.
    const float fx=forward[0],fz=forward[2],fh=std::hypot(fx,fz);
    const float gx=gaze_forward[0],gz=gaze_forward[2],gh=std::hypot(gx,gz);
    if(fh<0.05f*forward_length || gh<0.05f*gaze_length) return {}; // Straight up/down: heading undefined.
    // The camera's right axis decides which side is "right", so no world handedness is assumed.
    float rx=right[0],rz=right[2];const float rh=std::hypot(rx,rz);
    if(rh<0.05f) return {};
    rx/=rh;rz/=rh;
    const float along=(gx*fx+gz*fz)/(gh*fh),across=(gx*rx+gz*rz)/gh;
    const float yaw=std::atan2(across,along)*DEGREES;
    const float pitch=(std::asin(std::clamp(gaze_forward[1]/gaze_length,-1.f,1.f))-std::asin(std::clamp(forward[1]/forward_length,-1.f,1.f)))*DEGREES;
    return Alignment{yaw,pitch};
}

// Steering (on by default with -Controls; -NoGazeCamera opts out): drive the native right
// stick so the native camera turns toward the gaze. The view itself is unaffected: the
// rendered eyes are anchored to the stable VR reference, never to the native camera.
//
// The native stick response, measured live on 2026-10-21 (run
// 20261021_120606_master_combined, scripts/fit_gaze_response.py): nothing below about 0.3,
// roughly quadratic to 33 deg/s (yaw) at 0.8, then an edge boost to about 90-140 deg/s from
// 0.85. Pitch turns at a little over half the yaw rate. The law asks for a turn rate
// proportional to the error (response_per_second) and inverts the measured curve to get the
// stick; beyond boost_degrees it uses full stick. Deadband/re-engage hysteresis stops
// hunting. A pure stick-per-degree law (the first version) sat at 0.3-0.4 near the target,
// where the game turns under 2 deg/s: looks took 5-18 s to settle.
class Steering {
public:
    using Clock=std::chrono::steady_clock;
    using Values=vr_body::Parameters<7>;
    enum Index : size_t { RESPONSE,DEADBAND,REENGAGE,BOOST,MAX_STICK,RATE_SCALE,PITCH_SIGN };
    inline static constexpr std::array<Values::Parameter,7> PARAMETERS{{
        {"response_per_second",6,.5f,20},          // Requested turn rate per degree of error.
        {"deadband_degrees",2,0,15},               // An axis stops once inside this error...
        {"reengage_degrees",5,0,30},               // ...and only restarts beyond this one (no hunting).
        {"boost_degrees",15,5,90},                 // Larger errors use full stick (the game's edge boost).
        {"max_stick",1,.1f,1},
        {"rate_scale",1,.25f,4},                   // Native rate multiplier, if the in-game look sensitivity changes.
        {"pitch_sign",1,-1,1}}};                    // -1 if the game's look is set to inverted Y.
    // Measured stick -> degrees/second, below the edge boost (0.85 and up).
    struct Point { float stick,rate; };
    inline static constexpr std::array<Point,11> YAW_RESPONSE{{{.30f,.3f},{.35f,.8f},{.40f,2.2f},{.45f,4.5f},{.50f,7.2f},{.55f,10.6f},{.60f,15.1f},{.65f,19.4f},{.70f,24.4f},{.75f,29.6f},{.80f,33}}};
    inline static constexpr std::array<Point,11> PITCH_RESPONSE{{{.30f,0},{.35f,.5f},{.40f,1.2f},{.45f,2.6f},{.50f,4},{.55f,6.1f},{.60f,8.2f},{.65f,10.4f},{.70f,13.3f},{.75f,17.1f},{.80f,19.4f}}};
    struct Settings {
        Values values=Values::defaults(PARAMETERS);
        bool valid() const { const auto& v=values.values;return values.in_range(PARAMETERS) && v[DEADBAND]<=v[REENGAGE] && v[REENGAGE]<=v[BOOST] && std::abs(v[PITCH_SIGN])==1; }
        static std::optional<Settings> read(std::istream& in) { const auto v=Values::read(in,PARAMETERS);if(!v) return {};Settings s{*v};return s.valid()?std::optional{s}:std::nullopt; }
        static std::optional<Settings> load(const std::filesystem::path& path) { std::ifstream in{path};return read(in); }
        bool save(const std::filesystem::path& path) const { if(!path.is_absolute() || !valid()) return false;std::ofstream out{path};return values.write(out,PARAMETERS); }
        bool operator==(const Settings&) const = default;
    };
    struct Stick { float x{},y{}; }; // Native right stick, -1..1 (x > 0 turns right, y > 0 looks up).
    inline static constexpr auto MAX_AGE=std::chrono::milliseconds(100);

    static Steering& get() { static Steering steering; return steering; }
    bool set_settings(const Settings& s) { if(!s.valid()) return false;std::lock_guard lock{m_mutex};m_settings=s;return true; }
    Settings settings() const { std::lock_guard lock{m_mutex};return m_settings; }
    void enable(bool value) { std::lock_guard lock{m_mutex};m_enabled=value;if(!value) {m_latest.reset();m_holding={};} }
    bool enabled() const { std::lock_guard lock{m_mutex};return m_enabled; }
    // Render thread: the latest native camera and composed eye of a first-person frame.
    void observe(const Matrix& native_camera,const Vector& gaze_forward,Clock::time_point now) {
        const auto a=alignment(native_camera,gaze_forward);
        std::lock_guard lock{m_mutex};
        if(a && m_enabled) m_latest=Sample{*a,now};else m_latest.reset();
    }
    void clear() { std::lock_guard lock{m_mutex};m_latest.reset(); }
    std::optional<Alignment> latest(Clock::time_point now) const {
        std::lock_guard lock{m_mutex};return fresh(now)?std::optional{m_latest->alignment}:std::nullopt;
    }
    // Controller thread. Only gameplay and the hub in first person may steer (the caller decides,
    // scene_mode::Scene::gaze_camera); a stale or missing alignment sends nothing, so the native
    // camera simply stops.
    //
    // precise: the class ability (native Y) is held. The Vanguard's grapnel and the Tactical's
    // auspex scan pick their target from the native camera (docs/CLASS_KITS_2026-10-26.md:
    // the hook's target finder ranks by angle from the camera's centre, the auspex shoots its
    // ray along the camera), and both aim while Y is held and fire on release. At rest the
    // camera may sit anywhere inside the re-engage band (up to 5 degrees per axis, seen live);
    // while Y is held it closes to the deadband instead, so the ability goes where you look.
    std::optional<Stick> stick(bool allowed,Clock::time_point now,bool precise=false) {
        std::lock_guard lock{m_mutex};
        m_last.reset();
        if(!m_enabled || !allowed || !fresh(now)) {m_holding={};return {};}
        auto settings=m_settings;
        if(precise) settings.values.values[REENGAGE]=settings.values.values[DEADBAND];
        // Hold state survives only while the alignment stays fresh (a reset re-approaches).
        m_last=steer(m_latest->alignment,settings,m_holding);++m_steered;return m_last;
    }
    std::optional<Stick> last() const { std::lock_guard lock{m_mutex};return m_last; }
    uint64_t steered() const { std::lock_guard lock{m_mutex};return m_steered; }
    // Smallest stick that turns at least `rate` degrees/second on the measured curve.
    template<size_t N> static float stick_for(const std::array<Point,N>& curve,float rate) {
        if(!(rate>curve.front().rate)) return curve.front().stick;
        for(size_t i=1;i<N;++i) if(rate<=curve[i].rate) {
            const auto& a=curve[i-1];const auto& b=curve[i];return a.stick+(b.stick-a.stick)*(rate-a.rate)/(b.rate-a.rate);
        }
        return curve.back().stick;
    }
    // holding: per axis, inside the deadband since the error last exceeded reengage_degrees.
    template<size_t N> static float axis(float error_degrees,const std::array<Point,N>& curve,const Settings& s,bool& holding) {
        const auto& v=s.values.values;const float error=std::abs(error_degrees);
        if(!std::isfinite(error)) {holding=false;return 0;}
        if(error<=v[DEADBAND]) holding=true;
        else if(error>v[REENGAGE]) holding=false;
        if(holding) return 0;
        // Aim slightly inside the deadband so the approach does not stall at its edge.
        const float stick=error>v[BOOST]?1.f:stick_for(curve,v[RESPONSE]*(error-.5f*v[DEADBAND])/v[RATE_SCALE]);
        return std::copysign(std::min(stick,v[MAX_STICK]),error_degrees);
    }
    static Stick steer(const Alignment& a,const Settings& s,std::array<bool,2>& holding) {
        return {axis(a.yaw_degrees,YAW_RESPONSE,s,holding[0]),s.values.values[PITCH_SIGN]*axis(a.pitch_degrees,PITCH_RESPONSE,s,holding[1])};
    }
    // Age of the newest alignment, for logs (nullopt: none since enable/clear).
    std::optional<double> age_ms(Clock::time_point now) const {
        std::lock_guard lock{m_mutex};
        if(!m_latest || now<m_latest->time) return {};
        return std::chrono::duration<double,std::milli>(now-m_latest->time).count();
    }
private:
    struct Sample { Alignment alignment; Clock::time_point time; };
    bool fresh(Clock::time_point now) const { return m_latest && now>=m_latest->time && now-m_latest->time<=MAX_AGE; }
    mutable std::mutex m_mutex; bool m_enabled{}; Settings m_settings; std::optional<Sample> m_latest; std::optional<Stick> m_last; uint64_t m_steered{}; std::array<bool,2> m_holding{};
};
// The periodic log line. A missing or stale alignment is named (never "nan"); the stick the
// game actually received (the published pad) is reported apart from the computed one,
// because a disarmed pad (awaiting neutral controls) or a menu never carries the look.
inline std::string status_line(uint64_t steered,const std::optional<Alignment>& fresh,std::optional<double> age_ms,
    const std::optional<Steering::Stick>& computed,int16_t sent_x,int16_t sent_y) {
    std::string alignment;
    if(fresh) alignment=std::format("yaw_error={:.1f} pitch_error={:.1f}",fresh->yaw_degrees,fresh->pitch_degrees);
    else if(age_ms) alignment=std::format("alignment=stale age_ms={:.0f}",*age_ms);
    else alignment="alignment=none";
    const auto c=computed.value_or(Steering::Stick{});
    return std::format("steered={} {} computed=[{:.2f},{:.2f}] sent=[{:.2f},{:.2f}]",steered,alignment,c.x,c.y,sent_x/32767.f,sent_y/32767.f);
}
} // namespace vr_gaze
