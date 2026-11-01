#pragma once
#include "OrdinaryTrackedCamera.h"
#include "PipLensGeometry.h"

// Native frustum construction assumes equal corner lengths for its compact
// radius/near cap. Repair only those two values, after validating the actual
// per-eye polygon, world basis and native compact directions/cross products.
namespace ordinary_camera_culling {
using Source=ordinary_tracked_camera::Source;
template<class T> T field(const Source& source,size_t offset){T value{};std::memcpy(&value,source.data()+offset,sizeof(value));return value;}
inline std::optional<Source> correct(const Source& source,const ordinary_tracked_camera::Eye& eye){
    if(field<vr_camera::Matrix>(source,0)!=eye.world || field<uint32_t>(source,0x88)!=4 ||
       field<std::array<float,4>>(source,0x12c)!=eye.rectangle)return {};
    const auto near_z=field<float>(source,0x80),far_z=field<float>(source,0x84);
    if(near_z!=field<float>(eye.source,0x80) || far_z!=field<float>(eye.source,0x84))return {};
    pip_lens::Geometry geometry;
    if(!pip_lens::build(eye.rectangle,eye.world,near_z,far_z,0,0,geometry) ||
       field<std::array<float,8>>(source,0x8c)!=geometry.corners)return {};
    const auto directions=field<std::array<float,12>>(source,0x310),sides=field<std::array<float,12>>(source,0x340);
    const auto normal=field<std::array<float,3>>(source,0x370),origin=field<std::array<float,3>>(source,0x37c);
    float cap{};for(size_t axis=0;axis<3;++axis)cap-=eye.world[8+axis]*eye.world[8+axis]*near_z;
    if(!pip_lens::compact_matches(geometry,eye.world,near_z,directions,sides,normal,origin,cap))return {};
    const auto radius=field<float>(source,0x2f8),native_cap=field<float>(source,0x30c);
    float expected_native_cap{};for(size_t axis=0;axis<3;++axis)expected_native_cap+=normal[axis]*directions[axis]*radius;
    if(!std::isfinite(radius) || radius<=0 || !std::isfinite(native_cap) || std::abs(native_cap-expected_native_cap)>1e-5f)return {};
    Source result=source;const auto conservative=std::max(radius,geometry.radius);
    std::memcpy(result.data()+0x2f8,&conservative,4);std::memcpy(result.data()+0x30c,&cap,4);return result;
}
}
