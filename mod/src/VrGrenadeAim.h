#pragma once
#include "VrBodyFrame.h"
#include <numbers>

// Launch velocity of a thrown grenade, chosen at release in the head's yaw-only frame
// (height up, lateral right, forward along gaze yaw). The native game aims grenades along
// its own gameplay camera, whose yaw never follows the headset, snap turns or the hand,
// and always throws at one speed. The native launch override (NativeGrenadeLaunch)
// replaces that velocity with this direction at native_speed * speed_scale.
//
// Joe (21 Oct): the grenade follows the hand, direction and speed; distance comes from
// the angle and speed of release. Defaults: both hand weights 1 (exactly the hand's
// release direction) and speed proportional to the hand's release speed.
//
// Yaw: the gaze yaw pulled toward the hand's release velocity by hand_yaw_weight
// (0 = where you look, 1 = where the hand moved), at most max_yaw_from_gaze (180 = no
// limit). Pitch: base_pitch (the native lob, about 10 degrees) pulled toward the hand's
// release elevation by hand_pitch_weight, clamped to min..max pitch. The gaze pitch is
// never used: players look down at the belt while throwing (-15 to -52 degrees).
// Speed: native speed * hand speed / reference_hand_speed, clamped to
// min..max_speed_fraction of the native speed. The hand speed is the 100 ms chord at
// release (VrGrenadeThrow); Joe's six throws measured 2.4-4.2 m/s, median 4.0. A literal
// 1:1 metres-per-second launch would be about a sixth of the native speed (grenades at
// the feet), so the hand speed scales the game's own throw instead.
class VrGrenadeAim {
public:
    using Values=vr_body::Parameters<9>;
    enum Index { HAND_YAW_WEIGHT,MAX_YAW,HAND_PITCH_WEIGHT,BASE_PITCH,MIN_PITCH,MAX_PITCH,REFERENCE_SPEED,MIN_SPEED,MAX_SPEED };
    static constexpr std::array<Values::Parameter,9> PARAMETERS{{
        {"hand_yaw_weight",1,0,1}, {"max_yaw_from_gaze_degrees",180,0,180},
        {"hand_pitch_weight",1,0,1}, {"base_pitch_degrees",10,-30,60},
        {"min_pitch_degrees",-30,-60,60}, {"max_pitch_degrees",60,-60,80},
        {"reference_hand_speed_metres_per_second",4,1,10},
        {"min_speed_fraction",0.35f,0.1f,1}, {"max_speed_fraction",1.5f,1,3}}};
    // Below this horizontal hand speed the hand carries no usable yaw.
    static constexpr float MIN_HORIZONTAL_SPEED=0.3f;
    struct Settings {
        Values values=Values::defaults(PARAMETERS);
        bool operator==(const Settings&) const = default;
        float operator[](Index i) const { return values.values[i]; }
        bool valid() const { return values.in_range(PARAMETERS) && values.values[MIN_PITCH]<=values.values[MAX_PITCH]; }
        static std::optional<Settings> read(std::istream& in) { const auto v=Values::read(in,PARAMETERS);if(!v) return {};Settings s{*v};return s.valid()?std::optional{s}:std::nullopt; }
        bool write(std::ostream& out) const { return valid() && values.write(out,PARAMETERS); }
        static std::optional<Settings> load(const std::filesystem::path& path) { std::ifstream in{path};return read(in); }
        bool save(const std::filesystem::path& path) const { if(!path.is_absolute() || !valid()) return false;std::ofstream out{path};if(!write(out)) return false;out.flush();return bool(out); }
    };
    struct Aim {
        vr_body::Offset direction{0,0,1}; // Unit vector, head-yaw frame.
        float yaw_degrees{},pitch_degrees{};           // Chosen, yaw relative to the gaze (right positive).
        float hand_yaw_degrees{},hand_pitch_degrees{}; // The hand's own release direction.
        float hand_speed{};                            // m/s at release.
        float speed_scale{1};                          // Launch speed / native speed.
        bool hand_yaw_valid{};
        bool operator==(const Aim&) const = default;
    };
    // velocity: hand release velocity in the head-yaw frame (m/s).
    static Aim aim(const vr_body::Offset& velocity,const Settings& s) {
        constexpr float degrees=180/std::numbers::pi_v<float>;
        Aim a;
        const float horizontal=std::hypot(velocity.lateral,velocity.forward);
        const bool finite=std::isfinite(horizontal) && std::isfinite(velocity.height);
        a.hand_yaw_valid=finite && horizontal>=MIN_HORIZONTAL_SPEED;
        a.hand_yaw_degrees=a.hand_yaw_valid?std::atan2(velocity.lateral,velocity.forward)*degrees:0;
        a.hand_pitch_degrees=finite && (horizontal>0 || velocity.height!=0)?std::atan2(velocity.height,horizontal)*degrees:s[BASE_PITCH];
        a.yaw_degrees=std::clamp(s[HAND_YAW_WEIGHT]*a.hand_yaw_degrees,-s[MAX_YAW],s[MAX_YAW]);
        a.pitch_degrees=std::clamp(s[BASE_PITCH]+s[HAND_PITCH_WEIGHT]*(a.hand_pitch_degrees-s[BASE_PITCH]),s[MIN_PITCH],s[MAX_PITCH]);
        const float yaw=a.yaw_degrees/degrees,pitch=a.pitch_degrees/degrees;
        a.direction={std::sin(pitch),std::cos(pitch)*std::sin(yaw),std::cos(pitch)*std::cos(yaw)};
        a.hand_speed=finite?std::hypot(horizontal,velocity.height):0;
        a.speed_scale=std::clamp(a.hand_speed/s[REFERENCE_SPEED],s[MIN_SPEED],s[MAX_SPEED]);
        return a;
    }
    // Head-yaw frame back to tracking space (inverse of vr_body::direction).
    static std::array<float,3> tracking(const std::array<float,2>& forward,const vr_body::Offset& d) {
        return {d.forward*forward[0]-d.lateral*forward[1],d.height,d.forward*forward[1]+d.lateral*forward[0]};
    }
};
