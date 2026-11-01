#pragma once
#include "VrSession.h"
#include "GpuTextureReadback.h"
#include "OrdinaryJoinedCapture.h"

// Diagnostic readback of the same immutable, completed leases offered to OpenVR.
// Caller owns the Present queue and keeps the images alive until capture returns.
class OrdinaryEyeSnapshot {
public:
    static HRESULT capture(const VrSession::OrdinaryImages& images,ID3D12CommandQueue* queue,
                           std::vector<GpuTextureReadback::Pixels>& pixels){
        const auto& a=images.images.left;const auto& b=images.images.right;
        if(!images.dispatch || !queue || !a.ready() || !b.ready() || !a.texture() || !b.texture() || a.texture()==b.texture() ||
           !a.metadata.tracked_stereo || !b.metadata.tracked_stereo || a.metadata.eye!=0 || b.metadata.eye!=1 ||
           !a.metadata.render_id || a.metadata.render_id!=b.metadata.render_id || a.metadata.game_token!=b.metadata.game_token ||
           !a.metadata.tracking_epoch || a.metadata.tracking_epoch!=b.metadata.tracking_epoch)return E_INVALIDARG;
        const std::array<GpuTextureReadback::Source,2> sources{{
            {a.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{b.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
        return GpuTextureReadback::capture(queue,sources,pixels);
    }
    static HRESULT save(const VrSession::OrdinaryImages& images,ID3D12CommandQueue* queue,
                        const std::filesystem::path& directory,bool submitted,
                        const std::optional<OrdinaryJoinedCapture::Target>& joined={}){
        if(!directory.is_absolute() || std::filesystem::exists(directory))return E_INVALIDARG;
        if(joined && (!submitted || !OrdinaryJoinedCapture::safe(joined->id) || directory.filename().string()!=joined->id ||
            !joined->matches(images.dispatch,images.images.left.metadata.render_id,images.images.right.metadata.render_id)))return E_INVALIDARG;
        std::vector<GpuTextureReadback::Pixels> pixels;const auto hr=capture(images,queue,pixels);
        if(FAILED(hr))return hr;
        std::filesystem::create_directories(directory);
        if(!GpuTextureReadback::save(directory/"left.ppm",pixels[0]) || !GpuTextureReadback::save(directory/"right.ppm",pixels[1]))return E_FAIL;
        std::ofstream out{directory/"pair.json"};out<<"{\"dispatch\":"<<images.dispatch<<",\"submitted\":"<<(submitted?"true":"false")<<",\"eyes\":[";
        const std::array<const EyeTextureCapture::Image*,2> eyes{&images.images.left,&images.images.right};
        for(size_t i=0;i<2;++i){if(i)out<<',';const auto& m=eyes[i]->metadata;
            out<<"{\"eye\":"<<m.eye<<",\"render_id\":"<<m.render_id<<",\"tracking_epoch\":"<<m.tracking_epoch<<",\"native_hud\":"<<(m.native_hud?"true":"false")<<",\"world\":[";
            for(size_t j=0;j<16;++j){if(j)out<<',';out<<m.world[j];}out<<"],\"projection\":[";
            for(size_t j=0;j<16;++j){if(j)out<<',';out<<m.projection[j];}out<<"]";
            const auto size=eyes[i]->texture()->GetDesc(); // The guard guide drawn into this image, if any.
            out<<VrGuardGuide::json(m.guard_guide,m.runtime_head,m.runtime_eye_to_head,m.projection,float(size.Width),float(size.Height))<<",\"comfort_vignette\":"<<m.comfort_vignette;
            if(m.reticle_target){ // The game's crosshair patch drawn at this point (game world).
                out<<",\"reticle_point\":["<<(*m.reticle_target)[0]<<','<<(*m.reticle_target)[1]<<','<<(*m.reticle_target)[2]<<']';
                if(const auto p=VrAimDot::pixel(VrAimDot::Draw{*m.reticle_target,1,{}},m.world,m.projection,float(size.Width),float(size.Height)))out<<",\"reticle_pixel\":["<<(*p)[0]<<','<<(*p)[1]<<']';
            }
            if(m.aim_dot){ // The aim dot drawn into this image: its game-world point and centre pixel.
                out<<",\"aim_point\":["<<m.aim_dot->point[0]<<','<<m.aim_dot->point[1]<<','<<m.aim_dot->point[2]<<']';
                if(const auto p=VrAimDot::pixel(*m.aim_dot,m.world,m.projection,float(size.Width),float(size.Height)))out<<",\"aim_pixel\":["<<(*p)[0]<<','<<(*p)[1]<<']';
            }
            out<<'}';
        }out<<"],\"joined_capture\":";
        if(joined)out<<"{\"id\":\""<<joined->id<<"\",\"draw_dispatch\":"<<joined->dispatch<<",\"draw_render_id\":"<<joined->render_id<<",\"verified\":true}";
        else out<<"null";
        out<<"}\n";return out?S_OK:E_FAIL;
    }
};
