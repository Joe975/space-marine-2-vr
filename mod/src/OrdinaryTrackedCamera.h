#pragma once
#include "VrSession.h"
#include <algorithm>
#include <cstring>
#include <span>

// Owned sources for the future tracked ordinary add path. No native access or
// mutation: the caller supplies one untouched source and ONE resolved game
// anchor. Each returned source needs its own projection-builder scope when added.
namespace ordinary_tracked_camera {
using Source=std::array<uint8_t,EngineCameraProbe::SOURCE_CAMERA_SIZE>;
struct Eye {
    Source source;
    EngineCameraProbe::VrEye tracking;
    vr_camera::Matrix world,view,projection;
    std::array<float,4> rectangle;
    std::array<float,3> lens;
};
struct Pair {
    uint32_t game_token{};uint64_t render_id{};
    vr_camera::Matrix anchor;
    std::array<Eye,2> eyes;
    // The game's own camera this pair replaced: its world matrix and frustum tangents (left, right,
    // bottom, top), from the untouched source. The HUD canvas locks to it (VrNativePrompt::camera_locked).
    vr_camera::Matrix native_world{vr_camera::IDENTITY};std::array<float,4> native_tangents{};bool native_valid{};
};
namespace detail {
inline std::optional<Pair> prepare_eyes(std::span<const uint8_t> source,
        const vr_camera::Matrix& anchor,const FrameSyncClock::Frame& job,
        const std::array<EngineCameraProbe::VrEye,2>& tracking,std::chrono::steady_clock::time_point now){
    if(source.size()!=EngineCameraProbe::SOURCE_CAMERA_SIZE || job.invalid || !job.render_id ||
       !vr_camera::rigid(anchor,false))return {};
    const auto& left=tracking[0];const auto& right=tracking[1];
    if(!left.tracking_epoch || !left.reference_epoch || left.captured!=right.captured ||
       left.runtime_head!=right.runtime_head || left.relative_head!=right.relative_head ||
       left.tracking_epoch!=right.tracking_epoch || left.reference_epoch!=right.reference_epoch ||
       left.units_per_metre!=right.units_per_metre || left.view_settings!=right.view_settings)return {};
    float separation{};for(size_t i=12;i<15;++i){const auto delta=left.runtime_eye_to_head[i]-right.runtime_eye_to_head[i];separation+=delta*delta;}
    if(!std::isfinite(separation) || separation<=1e-8f)return {};
    for(size_t i=0;i<2;++i){const auto& eye=tracking[i];
        if(eye.eye!=int(i) || eye.render_token!=job.render_id || !eye.valid_at(now) || eye.synthetic ||
           now-eye.captured>std::chrono::milliseconds(100) || !eye.fov || !eye.fov->valid() ||
           !vr_camera::rigid(eye.runtime_head) || !vr_camera::rigid(eye.runtime_eye_to_head) || !vr_camera::rigid(eye.relative_head))return {};}
    vr_camera::Matrix original_world{},original_view{};
    std::memcpy(original_world.data(),source.data(),64);
    std::memcpy(original_view.data(),source.data()+0x40,64);
    if(!vr_camera::rigid(original_world,false) || !vr_camera::rigid(original_view,false))return {};
    const auto product=vr_camera::multiply(original_world,original_view);
    for(size_t i=0;i<16;++i)if(std::abs(product[i]-vr_camera::IDENTITY[i])>1e-3f)return {};
    float near_z{},far_z{};std::memcpy(&near_z,source.data()+0x80,4);std::memcpy(&far_z,source.data()+0x84,4);
    Pair result;result.game_token=job.token;result.render_id=job.render_id;result.anchor=anchor;
    {   // Near-plane rectangle (left, bottom, width, height at near_z) of the game's own camera.
        std::array<float,4> rect{};std::memcpy(rect.data(),source.data()+0x12c,sizeof(rect));
        const std::array<float,4> t{rect[0]/near_z,rect[0]/near_z+rect[2]/near_z,rect[1]/near_z,rect[1]/near_z+rect[3]/near_z};
        result.native_world=original_world;result.native_tangents=t;
        result.native_valid=near_z>0 && std::ranges::all_of(t,[](float v){return std::isfinite(v);}) && t[1]>t[0] && t[3]>t[2];
    }
    for(size_t slot=0;slot<2;++slot){
        auto& eye=result.eyes[slot];eye.tracking=tracking[slot];
        if(!vr_projection::build(*eye.tracking.fov,near_z,far_z,eye.projection) ||
           !vr_camera::compose(anchor,eye.tracking.relative_eye,eye.tracking.units_per_metre,
               eye.world,eye.view,eye.tracking.view_settings.mode!=VrViewMode::diorama))return {};
        const auto& fov=*eye.tracking.fov;
        eye.rectangle={near_z*fov.left,near_z*fov.bottom,near_z*(fov.right-fov.left),near_z*(fov.top-fov.bottom)};
        eye.lens={(std::atan(fov.right)-std::atan(fov.left))*57.2957795f,
            (std::atan(fov.top)-std::atan(fov.bottom))*57.2957795f,(fov.top-fov.bottom)/(fov.right-fov.left)};
        if(std::ranges::any_of(eye.lens,[](float value){return !std::isfinite(value) || value<=0;}))return {};
        std::copy(source.begin(),source.end(),eye.source.begin());
        std::memcpy(eye.source.data(),eye.world.data(),64);
        std::memcpy(eye.source.data()+0x40,eye.view.data(),64);
        // Native add copies this polygon verbatim; its frustum builder reads it
        // independently of the projection and rectangle (RVA 0x2225a20).
        const uint32_t corner_count=4;
        const auto& r=eye.rectangle;
        const std::array<float,8> corners{r[0],r[1],r[0]+r[2],r[1],r[0]+r[2],r[1]+r[3],r[0],r[1]+r[3]};
        std::memcpy(eye.source.data()+0x88,&corner_count,sizeof(corner_count));
        std::memcpy(eye.source.data()+0x8c,corners.data(),sizeof(corners));
        std::memcpy(eye.source.data()+0x12c,eye.rectangle.data(),sizeof(eye.rectangle));
        std::memcpy(eye.source.data()+0x2d0,eye.lens.data(),sizeof(eye.lens));
    }
    return result;
}
}
inline std::optional<Pair> prepare(std::span<const uint8_t> source,
        const vr_camera::Matrix& anchor,const FrameSyncClock::Frame& job,
        const VrSession::OrdinaryFrame& tracking,std::chrono::steady_clock::time_point now){
    if(job.render_id!=tracking.render_id || job.token!=tracking.game_token || !tracking.valid_at(now))return {};
    return detail::prepare_eyes(source,anchor,job,tracking.eyes,now);
}
// A consumer revalidates an owned provider result against the exact untouched
// native input, rather than trusting mutable matrices or a different job's copy.
inline bool matches(const Pair& pair,std::span<const uint8_t> source,
        const FrameSyncClock::Frame& job,std::chrono::steady_clock::time_point now){
    if(pair.render_id!=job.render_id || pair.game_token!=job.token)return false;
    const auto expected=detail::prepare_eyes(source,pair.anchor,job,{pair.eyes[0].tracking,pair.eyes[1].tracking},now);
    if(!expected)return false;
    for(size_t i=0;i<2;++i){const auto& actual=pair.eyes[i];const auto& eye=expected->eyes[i];
        if(actual.source!=eye.source || actual.world!=eye.world || actual.view!=eye.view || actual.projection!=eye.projection ||
           actual.rectangle!=eye.rectangle || actual.lens!=eye.lens)return false;}
    return true;
}
}
