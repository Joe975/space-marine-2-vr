#pragma once
#include "VrProjection.h"
#include <algorithm>
#include <array>
#include <limits>

namespace pip_lens {
struct Geometry {
    std::array<float,4> rectangle{};
    std::array<float,8> corners{};
    vr_projection::Tangents tangents;
    vr_camera::Matrix projection{};
    float radius{};
};
inline bool caches_match(const vr_camera::Matrix& projection,const vr_camera::Matrix& inverse_projection,
    const vr_camera::Matrix& view,const vr_camera::Matrix& combined) {
    for(size_t row=0;row<4;++row) for(size_t column=0;column<4;++column) {
        double identity{},expected{};
        for(size_t k=0;k<4;++k) {identity+=double(projection[row*4+k])*inverse_projection[k*4+column];expected+=double(view[row*4+k])*projection[k*4+column];}
        if(!std::isfinite(identity) || !std::isfinite(expected) || !std::isfinite(combined[row*4+column]) ||
            std::abs(identity-(row==column?1.:0.))>1e-4 || std::abs(expected-combined[row*4+column])>1e-3) return false;
    }
    return true;
}
inline bool compact_matches(const Geometry& geometry,const vr_camera::Matrix& world,float near_z,
    const std::array<float,12>& directions,const std::array<float,12>& sides,const std::array<float,3>& normal,
    const std::array<float,3>& origin,float distance) {
    double expected_distance{};
    for(size_t axis=0;axis<3;++axis) {
        if(!std::isfinite(normal[axis]) || !std::isfinite(origin[axis]) || std::abs(normal[axis]+world[8+axis])>1e-5f ||
            std::abs(origin[axis]-world[12+axis])>1e-4f) return false;
        expected_distance-=double(world[8+axis])*world[8+axis]*near_z;
    }
    if(!std::isfinite(distance) || std::abs(distance-expected_distance)>1e-5) return false;
    for(size_t corner=0;corner<4;++corner) {
        std::array<double,3> point{};double length{};
        for(size_t axis=0;axis<3;++axis) {point[axis]=double(world[axis])*geometry.corners[corner*2]+double(world[4+axis])*geometry.corners[corner*2+1]+double(world[8+axis])*near_z;length+=point[axis]*point[axis];}
        length=std::sqrt(length);
        for(size_t axis=0;axis<3;++axis) if(!std::isfinite(directions[corner*3+axis]) || std::abs(directions[corner*3+axis]-point[axis]/length)>1e-5) return false;
        for(size_t axis=0;axis<3;++axis) {
            const size_t a=(axis+1)%3,b=(axis+2)%3,next=(corner+1)%4;
            const auto cross=directions[corner*3+a]*directions[next*3+b]-directions[corner*3+b]*directions[next*3+a];
            if(!std::isfinite(sides[corner*3+axis]) || std::abs(sides[corner*3+axis]-cross)>1e-5f) return false;
        }
    }
    return true;
}
inline bool build(const std::array<float,4>& rectangle,const vr_camera::Matrix& world,
    float near_z,float far_z,float horizontal,float vertical,Geometry& out) {
    if(!vr_camera::rigid(world,false) || !std::isfinite(horizontal) || !std::isfinite(vertical) ||
        std::abs(horizontal)>.25f || std::abs(vertical)>.25f ||
        !std::ranges::all_of(rectangle,[](float x){return std::isfinite(x);}) || rectangle[2]<=0 || rectangle[3]<=0) return false;
    Geometry result;result.rectangle=rectangle;
    result.rectangle[0]+=rectangle[2]*horizontal;result.rectangle[1]+=rectangle[3]*vertical;
    const float left=result.rectangle[0],bottom=result.rectangle[1],right=left+rectangle[2],top=bottom+rectangle[3];
    result.tangents={left/near_z,right/near_z,bottom/near_z,top/near_z};
    if(!vr_projection::build(result.tangents,near_z,far_z,result.projection)) return false;
    result.corners={left,bottom,right,bottom,right,top,left,top};
    double radius_squared{};
    for(size_t corner=0;corner<4;++corner) {
        double length{};
        for(size_t axis=0;axis<3;++axis) {
            const double value=double(world[axis])*result.corners[corner*2]+double(world[4+axis])*result.corners[corner*2+1]+double(world[8+axis])*near_z;
            length+=value*value;
        }
        radius_squared=std::max(radius_squared,length);
    }
    result.radius=std::nextafter(static_cast<float>(std::sqrt(radius_squared)),std::numeric_limits<float>::infinity());
    if(!std::isfinite(result.radius) || result.radius<=0) return false;
    out=result;return true;
}
// Native planes are world-space dot(normal, point) >= distance, six records.
// Probe rays well inside/outside each edge, independently of the projection matrix.
inline bool culling_matches(const Geometry& geometry,const vr_camera::Matrix& world,float near_z,float far_z,
    const std::array<float,24>& planes) {
    if(!std::ranges::all_of(planes,[](float x){return std::isfinite(x);})) return false;
    std::array<double,6> lengths{};
    for(size_t p=0;p<6;++p) {
        double norm{};for(size_t axis=0;axis<3;++axis) norm+=double(planes[p*4+axis])*planes[p*4+axis];
        if(!std::isfinite(norm) || norm<1e-10) return false;
        lengths[p]=std::sqrt(norm); // Native side-plane crosses need not be unit length.
    }
    const auto& t=geometry.tangents;const float width=t.right-t.left,height=t.top-t.bottom;
    const std::array<float,5> xs{t.left-width*.1f,t.left+width*.1f,(t.left+t.right)*.5f,t.right-width*.1f,t.right+width*.1f};
    const std::array<float,5> ys{t.bottom-height*.1f,t.bottom+height*.1f,(t.bottom+t.top)*.5f,t.top-height*.1f,t.top+height*.1f};
    const std::array<float,3> depths{near_z*.5f,std::min(far_z*.5f,std::max(10.f,near_z*4)),far_z*1.1f};
    for(size_t z=0;z<depths.size();++z) for(size_t x=0;x<5;++x) for(size_t y=0;y<5;++y) {
        std::array<double,3> point{};
        for(size_t axis=0;axis<3;++axis) point[axis]=world[12+axis]+double(depths[z])*(world[axis]*xs[x]+world[4+axis]*ys[y]+world[8+axis]);
        bool inside=true;
        for(size_t p=0;p<6;++p) {
            double distance=-planes[p*4+3];for(size_t axis=0;axis<3;++axis) distance+=planes[p*4+axis]*point[axis];
            if(distance/lengths[p]<-.002) inside=false;
        }
        const bool expected=z==1 && x>0 && x<4 && y>0 && y<4;
        if(inside!=expected) return false;
    }
    return true;
}
}
