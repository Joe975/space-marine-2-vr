#pragma once
#include "VrCameraPose.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

// Comfort vignette while first person rides along with fast travel (VrBodyFollowing ride_along,
// the jump pack: docs/ANIMATION_CAMERA.md). Black at the edge of each eye, clear around the eye's
// own forward axis, so the periphery, where the streaming world does the most for motion sickness,
// is dimmed while the centre is kept. Head-fixed: it never moves with the world.
//
// strength (VrBodyFollowing Snapshot::vignette, [0,1]) sets the clear half-angle: CLEAR_WIDE at 0
// down to CLEAR_NARROW at 1, fully black FEATHER further out. An Index eye reaches about 50-55
// degrees to its outer edge, so a weak vignette only shades the corners.
namespace vr_vignette {
constexpr float CLEAR_WIDE_DEGREES=60,CLEAR_NARROW_DEGREES=25,FEATHER_DEGREES=15;
constexpr float MIN_STRENGTH=.005f; // below this nothing is drawn
struct Draw {
    // Tangent of the view ray per NDC coordinate: tan_x = ndc_x*map[0]+map[1], tan_y = ndc_y*map[2]+map[3].
    std::array<float,4> map{};
    float clear_tangent{},black_tangent{};
};
inline float clear_degrees(float strength) {return CLEAR_WIDE_DEGREES+(CLEAR_NARROW_DEGREES-CLEAR_WIDE_DEGREES)*std::clamp(strength,0.f,1.f);}
// projection: the eye's game-style projection (row vectors, +Z forward, w = z; vr_projection::build).
inline std::optional<Draw> draw(float strength,const vr_camera::Matrix& projection) {
    if(!std::isfinite(strength) || strength<MIN_STRENGTH) return std::nullopt;
    for(float v:projection) if(!std::isfinite(v)) return std::nullopt;
    const float sx=projection[0],sy=projection[5],w=projection[11];
    if(std::abs(sx)<1e-6f || std::abs(sy)<1e-6f || std::abs(w)<1e-6f) return std::nullopt;
    constexpr float radians=.01745329252f;
    const float clear=clear_degrees(strength);
    Draw d;
    d.map={w/sx,-projection[8]/sx,w/sy,-projection[9]/sy};
    d.clear_tangent=std::tan(clear*radians);
    d.black_tangent=std::tan(std::min(clear+FEATHER_DEGREES,89.f)*radians);
    return d;
}
// The shader's opacity at an NDC position (the CPU reference the tests check the GPU against).
inline float alpha(const Draw& d,float ndc_x,float ndc_y) {
    const float tx=ndc_x*d.map[0]+d.map[1],ty=ndc_y*d.map[2]+d.map[3];
    const float x=std::clamp((std::hypot(tx,ty)-d.clear_tangent)/(d.black_tangent-d.clear_tangent),0.f,1.f);
    return x*x*(3-2*x);
}
}
