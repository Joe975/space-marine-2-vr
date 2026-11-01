#pragma once
#include "VrCameraPose.h"
#include "VrProjection.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

// Pure owned camera math. No native reads/writes, allocations, SDK state or GPU
// ownership. Callers must separately prove producing-frame and prior-eye scope.
namespace ordinary_camera_math {
using Matrix=vr_camera::Matrix;
inline bool finite(const Matrix& m){return std::ranges::all_of(m,[](float v){return std::isfinite(v);});}
inline Matrix transpose(const Matrix& m){Matrix out{};for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c)out[r*4+c]=m[c*4+r];return out;}
inline std::optional<Matrix> inverse(const Matrix& m){
    if(!finite(m))return {};
    double rows[4][8]{};for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c){rows[r][c]=m[r*4+c];rows[r][c+4]=r==c;}
    for(size_t c=0;c<4;++c){size_t pivot=c;for(size_t r=c+1;r<4;++r)if(std::abs(rows[r][c])>std::abs(rows[pivot][c]))pivot=r;
        if(std::abs(rows[pivot][c])<1e-9)return {};for(size_t k=0;k<8;++k)std::swap(rows[pivot][k],rows[c][k]);
        const auto scale=rows[c][c];for(auto& v:rows[c])v/=scale;
        for(size_t r=0;r<4;++r)if(r!=c){const auto factor=rows[r][c];for(size_t k=0;k<8;++k)rows[r][k]-=factor*rows[c][k];}}
    Matrix result{};for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c)result[r*4+c]=float(rows[r][c+4]);
    return finite(result)?std::optional{result}:std::nullopt;
}
inline Matrix depth_free(const Matrix& m){Matrix out{};for(size_t r=0;r<4;++r){out[r*4]=m[r*4];out[r*4+1]=m[r*4+1];out[r*4+2]=m[r*4+3];out[r*4+3]=float(r==3);}return out;}
inline std::array<float,3> angular(const Matrix& relative){
    // Shipping main derives w,x,y,z from previous-world * current-view.
    std::array<float,4> q{};const float trace=relative[0]+relative[5]+relative[10];
    if(trace>0){const float s=std::sqrt(trace+1.f);q[0]=s*.5f;const float f=.5f/s;q[1]=(relative[6]-relative[9])*f;q[2]=(relative[8]-relative[2])*f;q[3]=(relative[1]-relative[4])*f;}
    else {size_t i=relative[5]>relative[0]?1:0;if(relative[10]>relative[i*4+i])i=2;const size_t j=(i+1)%3,k=(j+1)%3;
        const float s=std::sqrt(std::max(0.f,1.f+relative[i*4+i]-relative[j*4+j]-relative[k*4+k]));
        if(s<=1e-6f)return {};q[i+1]=s*.5f;const float f=.5f/s;q[0]=(relative[j*4+k]-relative[k*4+j])*f;q[j+1]=(relative[i*4+j]+relative[j*4+i])*f;q[k+1]=(relative[i*4+k]+relative[k*4+i])*f;}
    float norm{};for(float f:q)norm+=f*f;if(norm>=1e-12f){const auto scale=1.f/std::sqrt(norm);for(float& f:q)f*=scale;}else q={1,0,0,0};
    const auto [w,x,y,z]=q;const float singular=std::bit_cast<float>(uint32_t{0x3effffde}),s=x*y+z*w;
    std::array<float,3> radians{};
    if(s>singular)radians={0,2*std::atan2(x,w),1.57079632679f};
    else if(s<-singular)radians={0,-2*std::atan2(x,w),-1.57079632679f};
    else radians={std::atan2(2*(x*w-y*z),1-2*(x*x+z*z)),std::atan2(2*(w*y-x*z),1-2*(y*y+z*z)),std::asin(std::clamp(2*s,-1.f,1.f))};
    // Native helper returns degrees, then main multiplies its exact radian scale.
    const float degrees=std::bit_cast<float>(uint32_t{0x42652ee0}),to_radians=std::bit_cast<float>(uint32_t{0x3c8efa35});
    for(auto& v:radians)v=(v*degrees)*to_radians;return radians;
}
struct Previous {Matrix world{},view{},projection{},combined{},raster_combined{};std::array<float,3> delta{};};
struct Input {Matrix world{},view{},projection{};std::array<float,2> jitter{};uint32_t width{},height{},jitter_index{};float auxiliary_scale{},depth_remap{};std::optional<Previous> previous;bool cut{};};
struct Result {
    // Unknown entry+0x5dc and opaque previous +0x874/+0x8b4 deliberately
    // absent. No guessed semantics or fabricated matching of native fields.
    inline static constexpr std::array<size_t,23> OFFSETS{0x34,0x3dc,0x41c,0x45c,0x49c,0x4dc,0x51c,0x55c,0x59c,0x61c,0x65c,0x6b4,0x6f4,0x734,0x774,0x7b4,0x7f4,0x834,0x8f4,0x934,0x974,0x9b4,0x9f4};
    std::array<Matrix,23> matrices{};std::array<float,3> delta{},angles{};std::array<float,2> jitter{};uint32_t jitter_index{};bool moved{},cut{},reset{};
    static size_t index(size_t offset){const auto found=std::ranges::find(OFFSETS,offset);if(found==OFFSETS.end())throw std::out_of_range("unverified native camera offset");return size_t(found-OFFSETS.begin());}
    Matrix& at(size_t offset){return matrices[index(offset)];}
    const Matrix& at(size_t offset)const{return matrices[index(offset)];}
    Previous previous()const{return {at(0x34),at(0x3dc),at(0x49c),at(0x59c),at(0x55c),delta};}
};
namespace detail {
inline std::optional<Result> calculate(const Input& in){
    if(!vr_camera::rigid(in.world,false) || !vr_camera::rigid(in.view,false) || !finite(in.projection) || in.projection[0]<=0 || in.projection[5]<=0 ||
        !in.width || !in.height || in.width>32768 || in.height>32768 ||
        !std::isfinite(in.auxiliary_scale) || in.auxiliary_scale<=0 || !std::isfinite(in.depth_remap) || in.depth_remap<0 || in.depth_remap>1 ||
        std::ranges::any_of(in.jitter,[](float v){return !std::isfinite(v) || std::abs(v)>.501f;}))return {};
    auto product=vr_camera::multiply(in.world,in.view);for(size_t i=0;i<16;++i)if(std::abs(product[i]-vr_camera::IDENTITY[i])>1e-3f)return {};
    Result result;result.jitter=in.jitter;result.jitter_index=in.jitter_index;result.at(0x34)=in.world;result.at(0x3dc)=in.view;
    auto rotation=in.view;rotation[12]=rotation[13]=rotation[14]=0;result.at(0x41c)=rotation;
    auto jitter=vr_camera::IDENTITY;jitter[12]=2*in.jitter[0]/float(in.width);jitter[13]=-2*in.jitter[1]/float(in.height);
    auto remap=vr_camera::IDENTITY;remap[0]=remap[5]=in.auxiliary_scale/in.projection[0];remap[10]=in.depth_remap;remap[14]=1-in.depth_remap;
    result.at(0x49c)=in.projection;result.at(0x45c)=vr_camera::multiply(in.projection,jitter);
    result.at(0x59c)=vr_camera::multiply(in.view,in.projection);result.at(0x55c)=vr_camera::multiply(result.at(0x59c),jitter);
    result.at(0x65c)=vr_camera::multiply(rotation,in.projection);result.at(0x61c)=vr_camera::multiply(result.at(0x65c),jitter);
    result.at(0x6b4)=vr_camera::multiply(vr_camera::multiply(in.projection,remap),jitter);
    result.at(0x7b4)=vr_camera::multiply(result.at(0x65c),remap);result.at(0x734)=vr_camera::multiply(result.at(0x7b4),jitter);
    result.at(0x774)=vr_camera::multiply(result.at(0x59c),remap);
    for(const auto [dest,source]:std::array<std::pair<size_t,size_t>,3>{{{0x4dc,0x45c},{0x51c,0x49c},{0x6f4,0x6b4}}}){auto value=inverse(result.at(source));if(!value)return {};result.at(dest)=*value;}
    const Previous own=in.previous.value_or(result.previous());
    if(!vr_camera::rigid(own.world,false) || !vr_camera::rigid(own.view,false) || !finite(own.projection) || !finite(own.combined) || !finite(own.raster_combined) ||
        std::ranges::any_of(own.delta,[](float v){return !std::isfinite(v);}))return {};
    result.reset=!in.previous || in.cut;
    const auto& previous=result.reset?result.previous():own;
    result.at(0x834)=previous.projection;result.at(0x8f4)=previous.view;
    result.at(0x934)=vr_camera::multiply(previous.combined,jitter);result.at(0x974)=previous.raster_combined;
    result.at(0x7f4)=vr_camera::multiply(vr_camera::multiply(previous.combined,remap),jitter);
    const auto current_free=depth_free(result.at(0x59c)),previous_free=depth_free(previous.combined);const auto ci=inverse(current_free),pi=inverse(previous_free);if(!ci || !pi)return {};
    result.at(0x9b4)=transpose(vr_camera::multiply(*ci,previous_free));result.at(0x9f4)=transpose(vr_camera::multiply(*pi,current_free));
    float delta2{},prior_delta2{},orientation{};
    for(size_t i=0;i<3;++i){result.delta[i]=in.world[12+i]-previous.world[12+i];delta2+=result.delta[i]*result.delta[i];prior_delta2+=previous.delta[i]*previous.delta[i];orientation+=in.world[8+i]*previous.world[8+i];}
    result.moved=delta2>=4;result.cut=result.reset || std::abs(delta2-prior_delta2)>=4 || orientation<.5f;
    result.angles=angular(vr_camera::multiply(previous.world,in.view));
    for(const auto& m:result.matrices)if(!finite(m))return {};return result;
}
}
// Preserve the shipping desktop reference's symmetric-only admission contract.
inline std::optional<Result> build(const Input& in){
    if(in.projection[8]!=0 || in.projection[9]!=0)return {};
    return detail::calculate(in);
}
struct TrackedLens {
    vr_projection::Tangents fov;float near_z{},far_z{};
    bool operator==(const TrackedLens& other)const{return fov.left==other.fov.left && fov.right==other.fov.right &&
        fov.bottom==other.fov.bottom && fov.top==other.fov.top && near_z==other.near_z && far_z==other.far_z;}
};
struct TrackedHistory {
    Previous camera;TrackedLens lens;std::array<float,2> jitter{};uint32_t width{},height{};
};
struct TrackedInput {Input native;TrackedLens lens;std::optional<TrackedHistory> previous;};
struct TrackedResult {
    Result packet;TrackedLens lens;std::array<float,2> jitter{};uint32_t width{},height{};
    TrackedHistory history()const{return {packet.previous(),lens,jitter,width,height};}
};
// A distinct, explicit off-centre lens boundary. Mathematical history metadata
// does not establish native eye/job/SDK/GPU provenance; the transaction caller
// must prove those independently. Do not pass this result to the desktop path.
inline std::optional<TrackedResult> build_tracked(const TrackedInput& in){
    Matrix projection{};
    if(in.native.previous || !vr_projection::build(in.lens.fov,in.lens.near_z,in.lens.far_z,projection) || in.native.projection!=projection)return {};
    auto native=in.native;
    if(in.previous){const auto& p=*in.previous;
        Matrix prior_projection{};
        if(!p.width || !p.height || p.width>32768 || p.height>32768 ||
           std::ranges::any_of(p.jitter,[](float v){return !std::isfinite(v) || std::abs(v)>.501f;}) ||
           !vr_projection::build(p.lens.fov,p.lens.near_z,p.lens.far_z,prior_projection) || p.camera.projection!=prior_projection ||
           !vr_camera::rigid(p.camera.world,false) || !vr_camera::rigid(p.camera.view,false))return {};
        const auto identity=vr_camera::multiply(p.camera.world,p.camera.view);
        for(size_t i=0;i<16;++i)if(std::abs(identity[i]-vr_camera::IDENTITY[i])>1e-3f)return {};
        auto jitter=vr_camera::IDENTITY;jitter[12]=2*p.jitter[0]/float(p.width);jitter[13]=-2*p.jitter[1]/float(p.height);
        const auto combined=vr_camera::multiply(p.camera.view,prior_projection);
        const auto raster=vr_camera::multiply(combined,jitter);
        for(size_t i=0;i<16;++i)if(!std::isfinite(p.camera.combined[i]) || !std::isfinite(p.camera.raster_combined[i]) ||
            std::abs(combined[i]-p.camera.combined[i])>3e-4f || std::abs(raster[i]-p.camera.raster_combined[i])>3e-4f)return {};
        if((in.lens!=p.lens || in.native.width!=p.width || in.native.height!=p.height) && !in.native.cut)return {};
        native.previous=p.camera;
    }
    const auto result=detail::calculate(native);if(!result)return {};
    return TrackedResult{*result,in.lens,in.native.jitter,in.native.width,in.native.height};
}
}
