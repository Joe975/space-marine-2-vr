#pragma once
#include "OrdinaryDlssInputs.h"
#include "SdkFrameTokens.h"
#include "VrCameraPose.h"
#include <sl.h>
#include <sl_dlss.h>
#include <sl_matrix_helpers.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <ostream>
#include <vector>
#include <cstring>

// Bounded desktop diagnostic. A separate SDK viewport is not proof of separate
// native histories or completed GPU eye images. Never free pending SDK work.
class OrdinaryDlssContext {
public:
    static constexpr uint32_t VIEWPORT=0x534d3201u;
    struct Tracked {uint64_t dispatch{};std::array<vr_camera::Matrix,2> world,projection;bool operator==(const Tracked&)const=default;};
    struct Cameras {std::array<vr_camera::Matrix,2> world,projection,inverse_projection;uint32_t counter{};uintptr_t renderer{};std::optional<Tracked> tracked;bool operator==(const Cameras&)const=default;};
    struct Request {uint64_t dispatch{};SdkFrameTokens::Returned sdk;Cameras cameras;sl::Constants source;sl::DLSSOptions options;uint32_t render_width{},render_height{};};
    using SetOptions=std::function<sl::Result(const sl::ViewportHandle&,const sl::DLSSOptions&)>;
    using SetConstants=std::function<sl::Result(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&)>;
    using Evaluate=std::function<sl::Result(const sl::BaseStructure**,uint32_t)>;
    using Current=std::function<bool()>;
    struct Receipt {uint64_t dispatch{},sdk_ordinal{},previous_ordinal{};uint32_t token{},viewport{},render_width{},render_height{};std::array<float,2> source_jitter{},right_jitter{};bool reset{},continued{},raster_relation{},jitter_parity{};unsigned refusal{};std::optional<sl::Result> options,constants;sl::Result evaluation{};};
    void arm(SetOptions options,SetConstants constants,bool continuous=false){m_options=std::move(options);m_constants=std::move(constants);m_continuous=continuous;}
    void reset(){++m_epoch;if(m_owned)m_poison=true;}
    // Cold restart of owned stereo (OrdinaryColdRestart): clear a poisoned context as at first use.
    // The next accepted request sets the options on the private viewport again and starts a fresh
    // history (reset=true); pending SDK work is never freed. Refused while a run is in flight.
    bool restart(){
        if(m_busy.test_and_set(std::memory_order_acquire))return false;
        ++m_epoch;m_previous.reset();m_owned=false;m_poison=false;m_calls=0;m_restarts.fetch_add(1);
        m_busy.clear(std::memory_order_release);return true;
    }
    bool poisoned()const{return m_poison.load();}
    uint64_t restarts()const{return m_restarts.load();}
    sl::Result run(const std::optional<Request>& request,const sl::FrameToken& frame,const sl::BaseStructure** inputs,uint32_t count,const Current& current,const Evaluate& evaluate){
        if(m_busy.test_and_set(std::memory_order_acquire))return evaluate(inputs,count);
        struct Release{std::atomic_flag& flag;~Release(){flag.clear(std::memory_order_release);}} release{m_busy};
        Receipt receipt;const auto epoch=m_epoch.load();OrdinaryDlssInputs owned;bool prepared=false;sl::Constants constants;
        const auto still_current=[&]{try{return current && current();}catch(...){return false;}};
        if(request){receipt.dispatch=request->dispatch;receipt.sdk_ordinal=request->sdk.ordinal;receipt.token=request->sdk.token;}
        try{
            if(!request || !m_options || !m_constants)receipt.refusal=1;
            else if(m_poison || (!m_continuous && m_calls>=4))receipt.refusal=2;
            else if(!owned.build(inputs,count,0,VIEWPORT))receipt.refusal=4;
            else if(!valid(*request,frame,owned.render_width(),owned.render_height(),receipt.right_jitter) || !still_current() || (m_previous && request->sdk.epoch==m_previous->sdk.epoch && (request->dispatch<=m_previous->dispatch || request->sdk.ordinal<=m_previous->sdk.ordinal)))receipt.refusal=3;
            else {
                // The game's DLSS options or renderer changed (25 Oct 19:07: a resolution change in
                // the settings, render 2560x1440 -> 1920x1080). That used to refuse (5) and poison the
                // context, which stopped stereo for good. Set the new options on the private viewport
                // and start a fresh history instead.
                const bool rebind=m_owned && (!same_options(m_owned_options,request->options) || m_renderer!=request->cameras.renderer);
                const auto& r=*request;constants=r.source;constants.next=nullptr;receipt.render_width=owned.render_width();receipt.render_height=owned.render_height();receipt.source_jitter={r.source.jitterOffset.x,r.source.jitterOffset.y};receipt.raster_relation=true;receipt.jitter_parity=std::abs(receipt.source_jitter[0]-receipt.right_jitter[0])<=1e-3f && std::abs(receipt.source_jitter[1]-receipt.right_jitter[1])<=1e-3f;constants.jitterOffset={receipt.right_jitter[0],receipt.right_jitter[1]};
                const auto& world=r.cameras.world[1];constants.cameraPos={world[12],world[13],world[14]};constants.cameraRight={world[0],world[1],world[2]};constants.cameraUp={world[4],world[5],world[6]};constants.cameraFwd={world[8],world[9],world[10]};
                if(r.cameras.tracked){const auto& p=r.cameras.tracked->projection[1];std::memcpy(&constants.cameraViewToClip,p.data(),64);sl::matrixFullInvert(constants.clipToCameraView,constants.cameraViewToClip);
                    constants.cameraFOV=std::atan((1-p[9])/p[5])-std::atan((-1-p[9])/p[5]);constants.cameraAspectRatio=p[5]/p[0];}
                bool continued=!rebind && m_previous && m_previous->epoch==epoch && m_previous->sdk.epoch==r.sdk.epoch && m_previous->sdk.ordinal+1==r.sdk.ordinal && uint32_t(m_previous->sdk.token+1)==r.sdk.token && uint32_t(m_previous->counter+1)==r.cameras.counter && r.dispatch>m_previous->dispatch && r.source.reset==sl::eFalse;
                receipt.reset=!continued;receipt.continued=continued;receipt.previous_ordinal=continued?m_previous->sdk.ordinal:0;constants.reset=continued?sl::eFalse:sl::eTrue;
                const sl::float4x4 identity{{sl::float4{1,0,0,0},sl::float4{0,1,0,0},sl::float4{0,0,1,0},sl::float4{0,0,0,1}}};
                constants.clipToPrevClip=constants.prevClipToClip=identity;
                if(continued){sl::float4x4 now,previous,relative,temporary,previous_projection;std::memcpy(&now,world.data(),64);std::memcpy(&previous,m_previous->world.data(),64);std::memcpy(&previous_projection,m_previous->projection.data(),64);
                    sl::calcCameraToPrevCamera(relative,now,previous);sl::matrixMul(temporary,constants.clipToCameraView,relative);sl::matrixMul(constants.clipToPrevClip,temporary,previous_projection);sl::matrixFullInvert(constants.prevClipToClip,constants.clipToPrevClip);
                    // The camera jumped so far that the reprojection has no usable inverse (25 Oct
                    // 18:30: the first frames of a level load, refusal 6 poisoned the context and
                    // stopped stereo for good). That is a camera cut: reset the history instead.
                    if(!inverse_pair(constants.clipToPrevClip,constants.prevClipToClip)){
                        continued=false;receipt.reset=true;receipt.continued=false;receipt.previous_ordinal=0;constants.reset=sl::eTrue;constants.clipToPrevClip=constants.prevClipToClip=identity;}
                }
                if(!inverse_pair(constants.clipToPrevClip,constants.prevClipToClip))receipt.refusal=6;
                else {
                    const sl::ViewportHandle viewport{VIEWPORT};
                    if(!m_owned || rebind){receipt.options=m_options(viewport,r.options);if(*receipt.options!=sl::Result::eOk)receipt.refusal=7;else {m_owned=true;m_owned_options=r.options;m_owned_options.next=nullptr;m_renderer=r.cameras.renderer;}}
                    if(!receipt.refusal && (epoch!=m_epoch || !still_current()))receipt.refusal=8;
                    if(!receipt.refusal){receipt.constants=m_constants(constants,frame,viewport);if(*receipt.constants!=sl::Result::eOk)receipt.refusal=9;}
                    if(!receipt.refusal && (epoch!=m_epoch || !still_current()))receipt.refusal=8;
                    if(!receipt.refusal)prepared=true;
                }
            }
        }catch(...){receipt.refusal=10;}
        if(receipt.refusal)m_poison=true;
        // Exactly one actual evaluation. Setup refusal preserves the native call;
        // an actual evaluation failure is never retried on another context.
        receipt.viewport=prepared?VIEWPORT:0;receipt.evaluation=evaluate(prepared?owned.data():inputs,prepared?owned.count():count);
        if(prepared){++m_calls;if(receipt.evaluation==sl::Result::eOk && epoch==m_epoch && still_current()){
                const auto& r=*request;vr_camera::Matrix projection;std::memcpy(projection.data(),&constants.cameraViewToClip,64);m_previous=Previous{epoch,r.dispatch,r.sdk,r.cameras.counter,r.cameras.world[1],projection};
            }else {m_poison=true;m_previous.reset();receipt.refusal=11;}}
        {std::lock_guard lock{m_mutex};if(m_continuous && m_receipts.size()==4)m_receipts.erase(m_receipts.begin());if(m_receipts.size()<4)m_receipts.push_back(receipt);}return receipt.evaluation;
    }
    std::vector<Receipt> receipts()const{std::lock_guard lock{m_mutex};return m_receipts;}
    void write(std::ostream& out)const{const auto values=receipts();out<<"{\"viewport\":"<<VIEWPORT<<",\"private_options_submitted\":"<<(m_owned?"true":"false")<<",\"poisoned\":"<<(m_poison?"true":"false")<<",\"retained_until_shutdown\":true,\"histories_verified\":false,\"receipts\":[";
        for(size_t i=0;i<values.size();++i){if(i)out<<',';const auto& r=values[i];out<<"{\"dispatch\":"<<r.dispatch<<",\"sdk_ordinal\":"<<r.sdk_ordinal<<",\"token\":"<<r.token<<",\"viewport\":"<<r.viewport<<",\"reset\":"<<(r.reset?"true":"false")<<",\"continued\":"<<(r.continued?"true":"false")<<",\"previous_ordinal\":"<<r.previous_ordinal<<",\"refusal\":"<<r.refusal<<",\"raster_relation\":"<<(r.raster_relation?"true":"false")<<",\"jitter_parity\":"<<(r.jitter_parity?"true":"false")<<",\"render_width\":"<<r.render_width<<",\"render_height\":"<<r.render_height<<",\"source_jitter\":["<<r.source_jitter[0]<<','<<r.source_jitter[1]<<"],\"right_jitter\":["<<r.right_jitter[0]<<','<<r.right_jitter[1]<<"],\"options_result\":";if(r.options)out<<int(*r.options);else out<<"null";out<<",\"constants_result\":";if(r.constants)out<<int(*r.constants);else out<<"null";out<<",\"evaluation_result\":"<<int(r.evaluation)<<'}';}out<<"]}";
    }
private:
    static bool inverse_pair(const sl::float4x4& a,const sl::float4x4& b,float tolerance=1e-3f){sl::float4x4 product;sl::matrixMul(product,a,b);for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col){const auto f=(&product[row].x)[col];if(!std::isfinite(f) || std::abs(f-float(row==col))>tolerance)return false;}return true;}
    static bool valid(const Request& r,const sl::FrameToken& frame,uint32_t width,uint32_t height,std::array<float,2>& right_jitter){
        const auto& c=r.source;if(!r.dispatch || !r.sdk.epoch || !r.sdk.ordinal || r.sdk.object!=reinterpret_cast<uintptr_t>(&frame) || r.sdk.token!=uint32_t(frame) || !r.cameras.renderer ||
            c.structType!=sl::Constants::s_structType || (c.structVersion!=1 && c.structVersion!=2) || c.next || r.options.structType!=sl::DLSSOptions::s_structType || r.options.structVersion<1 || r.options.structVersion>3 || r.options.next || r.options.mode==sl::DLSSMode::eOff || uint32_t(r.options.mode)>=uint32_t(sl::DLSSMode::eCount) || !r.options.outputWidth || r.options.outputWidth==sl::INVALID_UINT || !r.options.outputHeight || r.options.outputHeight==sl::INVALID_UINT)return false;
        const auto finite=[](float value){return std::isfinite(value) && value!=sl::INVALID_FLOAT;};
        for(auto value:{c.jitterOffset.x,c.jitterOffset.y,c.mvecScale.x,c.mvecScale.y,c.cameraNear,c.cameraFar,c.cameraFOV,c.cameraAspectRatio,r.options.sharpness,r.options.preExposure,r.options.exposureScale})if(!finite(value))return false;
        if(c.cameraNear<=0 || c.cameraFar<=c.cameraNear || c.cameraFOV<=0 || c.cameraFOV>=3.141593f || c.cameraAspectRatio<=0 || r.options.sharpness<0 || r.options.sharpness>1 || r.options.preExposure<=0 || r.options.exposureScale<=0)return false;
        for(auto value:{c.reset,c.depthInverted,c.cameraMotionIncluded,c.motionVectors3D,c.orthographicProjection,c.motionVectorsDilated,c.motionVectorsJittered,r.options.colorBuffersHDR,r.options.indicatorInvertAxisX,r.options.indicatorInvertAxisY,r.options.useAutoExposure,r.options.alphaUpscalingEnabled})if(value!=sl::eFalse && value!=sl::eTrue)return false;
        if(!width || !height || width!=r.render_width || height!=r.render_height)return false;
        vr_camera::Matrix base;std::memcpy(base.data(),&c.cameraViewToClip,64);
        if(r.cameras.tracked && (r.cameras.tracked->dispatch!=r.dispatch || r.cameras.tracked->world!=r.cameras.world || r.cameras.tracked->projection[0]!=base))return false;
        for(unsigned eye=0;eye<2;++eye){if(!vr_camera::rigid(r.cameras.world[eye],false))return false;const auto& native=r.cameras.projection[eye];
            const auto& own=r.cameras.tracked?r.cameras.tracked->projection[eye]:base;
            for(unsigned i=0;i<16;++i)if(!finite(native[i]) || !finite(own[i]) || (i!=8 && i!=9 && std::abs(native[i]-own[i])>3e-5f))return false;
            if(r.cameras.tracked && (own[0]<=0 || own[5]<=0 || own[11]!=1 || own[15]!=0 || own[10]>=0 || own[14]<=0))return false;
            const std::array<float,2> jitter{(native[8]-own[8])*width*.5f,-(native[9]-own[9])*height*.5f};
            if(!finite(jitter[0]) || !finite(jitter[1]) || std::abs(jitter[0])>.501f || std::abs(jitter[1])>.501f)return false;
            sl::float4x4 projection,inverse;std::memcpy(&projection,native.data(),64);std::memcpy(&inverse,r.cameras.inverse_projection[eye].data(),64);if(!inverse_pair(projection,inverse,1e-5f))return false;
            if(!eye){if(std::abs(jitter[0]-c.jitterOffset.x)>1e-3f || std::abs(jitter[1]-c.jitterOffset.y)>1e-3f)return false;}else right_jitter=jitter;
        }
        const auto& left=r.cameras.world[0];const auto& right=r.cameras.world[1];if(!r.cameras.tracked)for(unsigned i=0;i<12;++i)if(std::abs(left[i]-right[i])>1e-5f)return false;
        const std::array<sl::float3,4> vectors{c.cameraRight,c.cameraUp,c.cameraFwd,c.cameraPos};for(unsigned row=0;row<4;++row)for(unsigned col=0;col<3;++col)if(!std::isfinite((&vectors[row].x)[col]) || std::abs(left[row*4+col]-(&vectors[row].x)[col])>3e-4f)return false;
        if(!r.cameras.tracked)for(unsigned i=0;i<3;++i)if(std::abs((right[12+i]-left[12+i])-.064f*left[i])>3e-4f)return false;
        return inverse_pair(c.cameraViewToClip,c.clipToCameraView,1e-5f);
    }
    static bool same_options(const sl::DLSSOptions& a,const sl::DLSSOptions& b){return a.structVersion==b.structVersion && a.mode==b.mode && a.outputWidth==b.outputWidth && a.outputHeight==b.outputHeight && a.sharpness==b.sharpness && a.preExposure==b.preExposure && a.exposureScale==b.exposureScale && a.colorBuffersHDR==b.colorBuffersHDR && a.indicatorInvertAxisX==b.indicatorInvertAxisX && a.indicatorInvertAxisY==b.indicatorInvertAxisY && a.dlaaPreset==b.dlaaPreset && a.qualityPreset==b.qualityPreset && a.balancedPreset==b.balancedPreset && a.performancePreset==b.performancePreset && a.ultraPerformancePreset==b.ultraPerformancePreset && a.ultraQualityPreset==b.ultraQualityPreset && a.useAutoExposure==b.useAutoExposure && a.alphaUpscalingEnabled==b.alphaUpscalingEnabled;}
    struct Previous {uint64_t epoch{},dispatch{};SdkFrameTokens::Returned sdk;uint32_t counter{};vr_camera::Matrix world,projection;};
    SetOptions m_options;SetConstants m_constants;std::optional<Previous> m_previous;sl::DLSSOptions m_owned_options;uintptr_t m_renderer{};uint64_t m_calls{};bool m_continuous{};
    std::atomic_bool m_owned{},m_poison{};std::atomic<uint64_t> m_epoch{1},m_restarts{};std::atomic_flag m_busy=ATOMIC_FLAG_INIT;
    mutable std::mutex m_mutex;std::vector<Receipt> m_receipts;
};
