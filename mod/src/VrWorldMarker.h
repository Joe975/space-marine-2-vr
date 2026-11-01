#pragma once
#include "VrProjection.h"

namespace vr_marker {
using Point=std::array<float,3>;
// Encode the eye's NDC in the native UI camera's view coordinates. Its later near-point
// projection/viewport conversion then produces the same screen position as the VR scene.
// Preserve eye-space depth for native range/front/back rules; do not clamp peripheral arrows.
inline bool reproject(const Point& world,const vr_camera::Matrix& eye_view,const vr_projection::Tangents& fov,
                      float native_near,const std::array<float,4>& native_rectangle,Point& output) {
    if(!vr_camera::rigid(eye_view,false) || !fov.valid() || !std::isfinite(native_near) || native_near<=0) return false;
    for(float v:world) if(!std::isfinite(v)) return false;
    for(float v:native_rectangle) if(!std::isfinite(v)) return false;
    if(native_rectangle[2]<=0 || native_rectangle[3]<=0) return false;
    Point view{};
    for(int i=0;i<3;++i) view[i]=world[0]*eye_view[i]+world[1]*eye_view[4+i]+world[2]*eye_view[8+i]+eye_view[12+i];
    if(!std::isfinite(view[2]) || std::abs(view[2])<=1e-5f) return false;
    const auto u=(view[0]/view[2]-fov.left)/(fov.right-fov.left);
    const auto v=(view[1]/view[2]-fov.bottom)/(fov.top-fov.bottom);
    const Point encoded{(native_rectangle[0]+u*native_rectangle[2])*view[2]/native_near,
                        (native_rectangle[1]+v*native_rectangle[3])*view[2]/native_near,view[2]};
    for(float value:encoded) if(!std::isfinite(value)) return false;
    output=encoded;return true;
}
}
