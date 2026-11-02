#include "EyeTextureCapture.h"
#include "PerformanceTrace.h"
#include <Windows.h>
#include <cmath>
#include <vector>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <tuple>

using Microsoft::WRL::ComPtr;
namespace {
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    return barrier;
}
bool same_desc(const D3D12_RESOURCE_DESC& a,const D3D12_RESOURCE_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.Dimension == b.Dimension &&
        a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality;
}
}

HRESULT EyeTextureCapture::configure(ID3D12Device* device,ID3D12CommandQueue* queue,const D3D12_RESOURCE_DESC& desc) {
    if (m_device.Get() == device && m_queue.Get() == queue && same_desc(m_desc,desc)) { return m_poisoned ? E_FAIL : S_OK; }
    if (!reset()) { return E_PENDING; }
    m_device = device; m_queue = queue; m_desc = desc;
    m_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto result = device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence));
    if (FAILED(result)) { m_device.Reset(); m_queue.Reset(); }
    return result;
}
HRESULT EyeTextureCapture::make_slot(std::shared_ptr<Slot>& out) {
    auto slot = std::make_shared<Slot>();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    auto result = m_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&m_desc,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&slot->texture));
    if (FAILED(result)) { return result; }
    result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot->allocator));
    if (FAILED(result)) { return result; }
    result = m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot->allocator.Get(),nullptr,IID_PPV_ARGS(&slot->commands));
    if (FAILED(result)) { return result; }
    result = slot->commands->Close();
    if (FAILED(result)) { return result; }
    if(PerformanceTrace::get().probes() && !slot->probe.configure(m_device.Get(),m_queue.Get()))
        PerformanceTrace::get().record("bridge_gpu_probe_unavailable",PerformanceTrace::now_ms(),0);
    out = std::move(slot);
    return S_OK;
}
HRESULT EyeTextureCapture::capture(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12Resource* source,const Metadata& metadata,D3D12_RESOURCE_STATES input_state,ID3D12Resource* native_hud,ID3D12Resource* menu_image) {
    if(input_state!=D3D12_RESOURCE_STATE_PRESENT && input_state!=D3D12_RESOURCE_STATE_COPY_SOURCE)return E_INVALIDARG;
    if (!device || !queue || !source || metadata.eye < 0 || metadata.eye > 1 || metadata.render_id == 0 || !vr_camera::rigid(metadata.world,false)) { return E_INVALIDARG; }
    for (float value : metadata.projection) { if (!std::isfinite(value)) { return E_INVALIDARG; } }
    if (metadata.tracked_stereo && (metadata.projection[0] <= 0 || metadata.projection[5] <= 0 ||
        std::abs(metadata.projection[11]-1) > 1e-4f || metadata.projection[14] <= 0)) { return E_INVALIDARG; }
    ComPtr<ID3D12Device> source_device, queue_device;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&source_device))) || FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) ||
        source_device.Get() != device || queue_device.Get() != device || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || device->GetNodeCount() != 1) { return E_INVALIDARG; }
    const auto desc = source->GetDesc();
    const bool supported_format = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || !supported_format) { return E_INVALIDARG; }
    std::optional<std::array<float,4>> hud_transform,prompt_transform,corners_transform;std::optional<VrHudHomography> prompt_homography,reticle_homography,ammo_homography;
    std::array<float,4> reticle_rect{},ammo_rect{};
    if(metadata.native_hud){
        if(!native_hud || native_hud==source || metadata.screen_menu || !metadata.tracked_stereo || !metadata.hud_crop.valid())return E_INVALIDARG;
        ComPtr<ID3D12Device> hud_device;const auto hud=native_hud->GetDesc();
        if(FAILED(native_hud->GetDevice(IID_PPV_ARGS(&hud_device))) || hud_device.Get()!=device || hud.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || hud.MipLevels!=1 || hud.DepthOrArraySize!=1 || hud.SampleDesc.Count!=1 ||
           (hud.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && hud.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && hud.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && hud.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) || (hud.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return E_INVALIDARG;
        hud_transform=VrNativeHud::transform(metadata.hud_placement,metadata.projection,metadata.runtime_eye_to_head);if(!hud_transform)return E_INVALIDARG;
        if(!metadata.hud_prompt.valid())return E_INVALIDARG;
        if(metadata.hud_prompt.enabled()){
            prompt_transform=metadata.hud_prompt.transform(float(hud.Width)/float(hud.Height),metadata.projection,metadata.runtime_eye_to_head);
            if(!prompt_transform)return E_INVALIDARG;
            // Camera-locked when the game camera gives a map; otherwise head-locked as before, never a lost eye.
            if(metadata.hud_camera && metadata.hud_camera_lock)prompt_homography=metadata.hud_prompt.camera_locked(*metadata.hud_camera,metadata.world,metadata.projection);
            // The crosshair patch at the aim point: optional, so a refused map leaves the crosshair on the
            // canvas. An empty map (the point is behind this eye) still takes it off the canvas.
            const float aspect=float(hud.Width)/float(hud.Height);
            if(metadata.hud_reticle.enabled() && metadata.reticle_target && metadata.hud_camera)
                if(const auto rect=metadata.hud_reticle.rect(aspect))
                    if((reticle_homography=metadata.hud_reticle.at(*metadata.reticle_target,*metadata.hud_camera,aspect,metadata.world,metadata.projection))){reticle_rect=*rect;
                        if(const auto column=metadata.hud_reticle.ammo_rect(aspect))
                            if((ammo_homography=metadata.hud_reticle.at(*column,*metadata.reticle_target,*metadata.hud_camera,metadata.world,metadata.projection)))ammo_rect=*column;}
        }
        if(!metadata.hud_corners.valid())return E_INVALIDARG;
        if(metadata.hud_corners.enabled()){corners_transform=metadata.hud_corners.transform(metadata.projection,metadata.runtime_eye_to_head);if(!corners_transform)return E_INVALIDARG;}
    }else if(native_hud)return E_INVALIDARG;
    // A separate menu image (e.g. the swapchain back buffer) replaces the scene with the
    // head-relative menu panel; the eye-sized target keeps its own resolution.
    if(menu_image){
        if(!metadata.screen_menu || native_hud || menu_image==source)return E_INVALIDARG;
        ComPtr<ID3D12Device> menu_device;const auto menu=menu_image->GetDesc();
        if(FAILED(menu_image->GetDevice(IID_PPV_ARGS(&menu_device))) || menu_device.Get()!=device || menu.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || menu.MipLevels!=1 || menu.DepthOrArraySize!=1 || menu.SampleDesc.Count!=1 ||
           (menu.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && menu.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && menu.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && menu.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) || (menu.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return E_INVALIDARG;
    }
    auto result = configure(device,queue,desc);
    if (FAILED(result)) { return result; }
    if (metadata.render_id <= m_last_render_id) { return S_FALSE; }
    poll();
    if (m_poisoned) { return E_FAIL; }
    std::shared_ptr<Slot>* available{};
    for (size_t i = 0; i < m_slot_count; ++i) {
        auto& slot = m_slots[metadata.eye][i];
        if (!slot || (slot.use_count() == 1 && slot->pending <= m_fence->GetCompletedValue())) { available = &slot; break; }
    }
    if (!available) { return E_PENDING; } // Never overwrite a consumer-held image or in-flight allocator.
    if (!*available) { result = make_slot(*available); if (FAILED(result)) { return result; } }
    auto& slot = **available;
    // The panel shows the menu canvas at its own aspect, whatever the eye image size.
    const auto canvas=menu_image?menu_image->GetDesc():desc;
    const auto panel=metadata.screen_menu?metadata.menu_panel.transform(metadata.projection,metadata.runtime_eye_to_head,float(canvas.Width)/canvas.Height,metadata.runtime_head):std::nullopt;
    if(metadata.screen_menu && !panel) return E_INVALIDARG;
    if(panel) {result=m_menu_renderer.prepare(device,desc.Format,slot.menu_bindings);if(FAILED(result)) return result;}
    if(hud_transform){result=m_hud_renderer.prepare(device,desc.Format,slot.hud_bindings);if(FAILED(result))return result;}
    // Guard guide: optional, so a refused projection or pipeline skips it and keeps the eye.
    std::array<std::optional<std::array<float,16>>,std::tuple_size_v<decltype(VrGuardGuide::Draw::quads)>> guide_corners;
    if(metadata.guard_guide && metadata.tracked_stereo && !metadata.screen_menu && !m_guide_failed) {
        for(size_t q=0;q<guide_corners.size();++q) guide_corners[q]=VrGuardGuide::clip(metadata.guard_guide->quads[q],metadata.runtime_head,metadata.runtime_eye_to_head,metadata.projection);
        if(std::ranges::any_of(guide_corners,[](const auto& c){return c.has_value();}) && FAILED(m_guide_renderer.prepare(device,desc.Format,slot.guide_bindings))) {m_guide_failed=true;guide_corners={};}
    }
    const bool guide=std::ranges::any_of(guide_corners,[](const auto& c){return c.has_value();});
    // Comfort vignette (riding along): optional in the same way.
    auto vignette=metadata.tracked_stereo && !metadata.screen_menu && !m_vignette_failed?vr_vignette::draw(metadata.comfort_vignette,metadata.projection):std::nullopt;
    if(vignette && FAILED(m_vignette_renderer.prepare(device,desc.Format,slot.vignette_bindings))) {m_vignette_failed=true;vignette.reset();}
    // Aim dot: optional too; it shares the guide's pipeline.
    auto aim_dot=metadata.aim_dot && metadata.tracked_stereo && !metadata.screen_menu && !m_guide_failed?VrAimDot::clip(*metadata.aim_dot,metadata.world,metadata.projection):std::nullopt;
    if(aim_dot && FAILED(m_guide_renderer.prepare(device,desc.Format,slot.guide_bindings))) {m_guide_failed=true;aim_dot.reset();}
    result = slot.allocator->Reset(); if (FAILED(result)) { return result; }
    result = slot.commands->Reset(slot.allocator.Get(),nullptr); if (FAILED(result)) { return result; }
    const bool detail=PerformanceTrace::get().probes() && slot.probe.ready();
    if(detail)slot.probe.begin(slot.commands.Get());
    const bool external_menu=panel && menu_image;
    // An external menu leaves the scene source untouched (no barrier).
    const auto source_state=external_menu?input_state:panel?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_COPY_SOURCE;
    const auto target_state=panel?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COPY_DEST;
    auto source_to_copy = transition(source,input_state,source_state);
    auto target_to_copy = transition(slot.texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,target_state);
    const D3D12_RESOURCE_BARRIER before[]{source_to_copy,target_to_copy};
    slot.commands->ResourceBarrier(input_state==source_state?1:2,before+(input_state==source_state?1:0));
    if(external_menu){
        const auto to_read=transition(menu_image,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);slot.commands->ResourceBarrier(1,&to_read);
        m_menu_renderer.draw(device,slot.commands.Get(),slot.menu_bindings,menu_image,slot.texture.Get(),*panel);
        const auto to_present=transition(menu_image,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PRESENT);slot.commands->ResourceBarrier(1,&to_present);
    }
    else if(panel) m_menu_renderer.draw(device,slot.commands.Get(),slot.menu_bindings,source,slot.texture.Get(),*panel);
    else slot.commands->CopyResource(slot.texture.Get(),source);
    // A photo (Tap): the plain source, already in COPY_SOURCE here, before any panel. Never a menu frame.
    const bool tapping=m_tap && !panel && m_tap->buffer && m_tap->footprint.Footprint.Width==desc.Width && m_tap->footprint.Footprint.Height==desc.Height && m_tap->footprint.Footprint.Format==desc.Format;
    if(tapping){
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=m_tap->buffer.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=m_tap->footprint;
        slot.commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    }
    if(guide || vignette){ // Over the scene, under the HUD.
        if(target_state!=D3D12_RESOURCE_STATE_RENDER_TARGET){const auto to_target=transition(slot.texture.Get(),target_state,D3D12_RESOURCE_STATE_RENDER_TARGET);slot.commands->ResourceBarrier(1,&to_target);}
        for(const auto& corners:guide_corners) if(corners) m_guide_renderer.draw(device,slot.commands.Get(),slot.guide_bindings,slot.texture.Get(),*corners,metadata.guard_guide->colour);
        if(vignette) m_vignette_renderer.draw(device,slot.commands.Get(),slot.vignette_bindings,slot.texture.Get(),*vignette);
        if(target_state!=D3D12_RESOURCE_STATE_RENDER_TARGET){const auto back=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,target_state);slot.commands->ResourceBarrier(1,&back);}
    }
    if(hud_transform){
        const D3D12_RESOURCE_BARRIER hud_before[]{transition(slot.texture.Get(),target_state,D3D12_RESOURCE_STATE_RENDER_TARGET),transition(native_hud,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        slot.commands->ResourceBarrier(2,hud_before);
        // The crosshair patch the canvas leaves out, at the aim point, behind every other panel.
        if(reticle_homography)m_hud_renderer.draw(device,slot.commands.Get(),slot.hud_bindings,native_hud,slot.texture.Get(),*prompt_transform,VrHudReticle::crop(reticle_rect),{},&*reticle_homography);
        if(ammo_homography)m_hud_renderer.draw(device,slot.commands.Get(),slot.hud_bindings,native_hud,slot.texture.Get(),*prompt_transform,VrHudReticle::crop(ammo_rect),{},&*ammo_homography);
        m_hud_renderer.draw(device,slot.commands.Get(),slot.hud_bindings,native_hud,slot.texture.Get(),*hud_transform,metadata.hud_crop);
        // Same pipeline and source view: the prompt crop is a second, centred plane, without what
        // the strip and the corners panel already show.
        if(prompt_transform)m_hud_renderer.draw(device,slot.commands.Get(),slot.hud_bindings,native_hud,slot.texture.Get(),*prompt_transform,metadata.hud_prompt.crop(),
            metadata.reticle_cut?VrNativeHudExcluded::of(metadata.hud_crop,metadata.hud_corners,reticle_rect,ammo_rect):VrNativeHudExcluded::of(metadata.hud_crop,metadata.hud_corners),prompt_homography?&*prompt_homography:nullptr);
        // And the right-hand corners on their own low panel, on top.
        if(corners_transform)m_hud_renderer.draw(device,slot.commands.Get(),slot.hud_bindings,native_hud,slot.texture.Get(),*corners_transform,metadata.hud_corners.regions());
        const D3D12_RESOURCE_BARRIER hud_after[]{transition(native_hud,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PRESENT),transition(slot.texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,target_state)};
        slot.commands->ResourceBarrier(2,hud_after);
    }
    if(aim_dot){ // On top of the HUD: it marks the aim, the HUD's crosshair included.
        if(target_state!=D3D12_RESOURCE_STATE_RENDER_TARGET){const auto to_target=transition(slot.texture.Get(),target_state,D3D12_RESOURCE_STATE_RENDER_TARGET);slot.commands->ResourceBarrier(1,&to_target);}
        m_guide_renderer.draw(device,slot.commands.Get(),slot.guide_bindings,slot.texture.Get(),*aim_dot,metadata.aim_dot->colour,VrAimDot::RING);
        if(target_state!=D3D12_RESOURCE_STATE_RENDER_TARGET){const auto back=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,target_state);slot.commands->ResourceBarrier(1,&back);}
    }
    const D3D12_RESOURCE_BARRIER after[]{transition(source,source_state,input_state),
        transition(slot.texture.Get(),target_state,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    slot.commands->ResourceBarrier(input_state==source_state?1:2,after+(input_state==source_state?1:0));
    if(detail)slot.probe.end(slot.commands.Get());
    result = slot.commands->Close(); if (FAILED(result)) { return result; }
    slot.source = source; // Keep source alive until its copy fence completes.
    slot.hud_source=native_hud;
    slot.menu_source=menu_image;
    slot.tap_target=tapping?m_tap->buffer:ComPtr<ID3D12Resource>{}; // Alive until the copy's fence, whatever the photo does.
    ID3D12CommandList* commands[]{slot.commands.Get()};
    if(detail){slot.probe_enqueue=PerformanceTrace::now_ms();slot.probe_eye=metadata.eye;slot.probe_render=metadata.render_id;slot.probe_pending=true;}
    m_queue->ExecuteCommandLists(1,commands);
    slot.pending = ++m_fence_value;
    result = m_queue->Signal(m_fence.Get(),slot.pending);
    if (FAILED(result)) { m_poisoned = true; return result; }
    if(tapping){m_tapped=Tapped{m_fence,slot.pending,metadata.render_id};m_tap.reset();}
    m_latest[metadata.eye] = Image{metadata,m_generation,slot.pending,m_fence,*available};
    if(!guide) m_latest[metadata.eye]->metadata.guard_guide.reset(); // Metadata says what the image shows.
    if(!vignette) m_latest[metadata.eye]->metadata.comfort_vignette=0;
    if(!aim_dot) m_latest[metadata.eye]->metadata.aim_dot.reset();
    if(!reticle_homography || reticle_homography->empty()) m_latest[metadata.eye]->metadata.reticle_target.reset();
    if (metadata.eye == 1) {
        m_candidate_pair.reset();
        if (m_latest[0] && m_latest[0]->metadata.render_id+1 == metadata.render_id &&
            uint32_t(m_latest[0]->metadata.game_token+1) == metadata.game_token) { m_candidate_pair = Pair{*m_latest[0],*m_latest[1]}; }
    }
    m_last_render_id = metadata.render_id;
    return S_OK;
}
void EyeTextureCapture::poll() {
    if (!m_fence) { return; }
    const auto completed = m_fence->GetCompletedValue();
    if (completed == UINT64_MAX) { m_poisoned = true; return; }
    for (auto& eye : m_slots) for (auto& slot : eye) { if (slot && slot->pending <= completed) {
        if(slot->probe_pending){
            const auto observed=PerformanceTrace::now_ms();
            if(const auto gpu=slot->probe.read(m_queue.Get())){
                auto& trace=PerformanceTrace::get();const bool right=slot->probe_eye==1;
                trace.record(right?"bridge_right_gpu":"bridge_left_gpu",gpu->start_ms,gpu->duration_ms,double(slot->probe_render));
                if(gpu->start_ms>=slot->probe_enqueue)trace.record(right?"bridge_right_queue_delay_cpu":"bridge_left_queue_delay_cpu",slot->probe_enqueue,gpu->start_ms-slot->probe_enqueue,double(slot->probe_render));
                const auto end=gpu->start_ms+gpu->duration_ms;
                if(observed>=end)trace.record(right?"bridge_right_completion_poll_delay_cpu":"bridge_left_completion_poll_delay_cpu",end,observed-end,double(slot->probe_render));
            }else PerformanceTrace::get().record("bridge_gpu_probe_read_failed",observed,0,double(slot->probe_render));
            slot->probe_pending=false;
        }
        slot->source.Reset();slot->hud_source.Reset();slot->menu_source.Reset();slot->tap_target.Reset();
    } }
}
HRESULT EyeTextureCapture::dump_pair(const std::filesystem::path& directory,bool require_tracked) {
    const auto images=pair(require_tracked);
    if(!images || !directory.is_absolute()) return E_INVALIDARG;
    struct Readback {
        Pair images;
        ComPtr<ID3D12Resource> buffers[2];
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12CommandQueue> queue;
    };
    auto work=std::make_unique<Readback>();work->images=*images;work->queue=m_queue;
    auto hr=m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&work->allocator));
    if(FAILED(hr)) return hr;
    hr=m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,work->allocator.Get(),nullptr,IID_PPV_ARGS(&work->commands));
    if(FAILED(hr)) return hr;
    hr=m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&work->fence));if(FAILED(hr)) return hr;
    const Image* eyes[]{&work->images.left,&work->images.right};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2]{};UINT64 sizes[2]{};
    for(int i=0;i<2;++i) {
        const auto desc=eyes[i]->texture()->GetDesc();
        m_device->GetCopyableFootprints(&desc,0,1,0,&layouts[i],nullptr,nullptr,&sizes[i]);
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=sizes[i];buffer.Height=1;
        buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        hr=m_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&work->buffers[i]));
        if(FAILED(hr)) return hr;
        auto before=transition(eyes[i]->texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        work->commands->ResourceBarrier(1,&before);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=eyes[i]->texture();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=work->buffers[i].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=layouts[i];
        work->commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        auto after=transition(eyes[i]->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        work->commands->ResourceBarrier(1,&after);
    }
    hr=work->commands->Close();if(FAILED(hr)) return hr;
    const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event) return HRESULT_FROM_WIN32(GetLastError());
    ID3D12CommandList* lists[]{work->commands.Get()};m_queue->ExecuteCommandLists(1,lists);
    hr=m_queue->Signal(work->fence.Get(),1);
    if(SUCCEEDED(hr)) hr=work->fence->SetEventOnCompletion(1,event);
    const bool done=SUCCEEDED(hr) && WaitForSingleObject(event,1000)==WAIT_OBJECT_0;CloseHandle(event);
    if(!done) {
        // A timed-out GPU may still reference the allocator, leases and readback buffers.
        static auto* quarantine=new std::vector<std::unique_ptr<Readback>>;
        quarantine->push_back(std::move(work));m_poisoned=true;return FAILED(hr)?hr:HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    try {
        if(!std::filesystem::create_directory(directory)) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        std::ofstream json{directory/"pair.json"};json<<std::setprecision(9)<<"{\"generation\":"<<m_generation<<",\"eyes\":[";
        for(int i=0;i<2;++i) {
            const auto desc=eyes[i]->texture()->GetDesc();void* mapped{};D3D12_RANGE range{0,static_cast<SIZE_T>(sizes[i])};
            hr=work->buffers[i]->Map(0,&range,&mapped);if(FAILED(hr)) return hr;
            std::ofstream ppm{directory/(i?"right.ppm":"left.ppm"),std::ios::binary};
            ppm<<"P6\n"<<desc.Width<<' '<<desc.Height<<"\n255\n";
            const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            std::vector<char> row(static_cast<size_t>(desc.Width)*3);
            for(unsigned y=0;y<desc.Height;++y) {
                const auto* pixels=static_cast<const uint8_t*>(mapped)+layouts[i].Offset+y*layouts[i].Footprint.RowPitch;
                for(size_t x=0;x<desc.Width;++x) {row[x*3]=pixels[x*4+(bgra?2:0)];row[x*3+1]=pixels[x*4+1];row[x*3+2]=pixels[x*4+(bgra?0:2)];}
                ppm.write(row.data(),row.size());
            }
            D3D12_RANGE no_writes{};work->buffers[i]->Unmap(0,&no_writes);ppm.close();if(!ppm) return E_FAIL;
            const auto& meta=eyes[i]->metadata;
            if(i) json<<',';
            json<<"{\"eye\":"<<meta.eye<<",\"width\":"<<desc.Width<<",\"height\":"<<desc.Height<<",\"render_id\":"<<meta.render_id
                <<",\"game_token\":"<<meta.game_token<<",\"tracked_stereo\":"<<(meta.tracked_stereo?"true":"false")<<",\"epoch\":"<<meta.tracking_epoch
                <<",\"screen_menu\":"<<(meta.screen_menu?"true":"false")<<",\"menu_width_degrees\":"<<meta.menu_panel.width_degrees<<",\"menu_distance_metres\":"<<meta.menu_panel.distance_metres<<",\"native_hud\":"<<(meta.native_hud?"true":"false")
                <<VrGuardGuide::json(meta.guard_guide,meta.runtime_head,meta.runtime_eye_to_head,meta.projection,float(desc.Width),float(desc.Height))
                <<",\"comfort_vignette\":"<<meta.comfort_vignette;
            if(meta.aim_dot){
                json<<",\"aim_point\":["<<meta.aim_dot->point[0]<<','<<meta.aim_dot->point[1]<<','<<meta.aim_dot->point[2]<<']';
                if(const auto p=VrAimDot::pixel(*meta.aim_dot,meta.world,meta.projection,float(desc.Width),float(desc.Height)))json<<",\"aim_pixel\":["<<(*p)[0]<<','<<(*p)[1]<<']';
            }
            const auto matrix=[&](const char* key,const auto& values) {json<<",\""<<key<<"\":[";for(size_t n=0;n<values.size();++n) {if(n) json<<',';json<<values[n];}json<<']';};
            matrix("world",meta.world);matrix("projection",meta.projection);matrix("runtime_head",meta.runtime_head);matrix("eye_to_head",meta.runtime_eye_to_head);json<<'}';
        }
        json<<"]}\n";json.close();if(!json) return E_FAIL;
    } catch(const std::filesystem::filesystem_error&) {return E_FAIL;}
    return S_OK;
}
std::optional<EyeTextureCapture::Image> EyeTextureCapture::latest(int eye) const { return eye >= 0 && eye <= 1 ? m_latest[eye] : std::nullopt; }
std::optional<EyeTextureCapture::Pair> EyeTextureCapture::pair(bool require_tracked) const {
    if (!m_candidate_pair || m_poisoned) { return std::nullopt; }
    const auto& left = m_candidate_pair->left; const auto& right = m_candidate_pair->right;
    if (!left.ready() || !right.ready() || left.generation != m_generation || right.generation != m_generation ||
        left.metadata.render_id+1 != right.metadata.render_id || uint32_t(left.metadata.game_token+1) != right.metadata.game_token ||
        left.metadata.screen_menu!=right.metadata.screen_menu || (left.metadata.screen_menu && left.metadata.menu_panel!=right.metadata.menu_panel) ||
        left.metadata.native_hud!=right.metadata.native_hud || (left.metadata.native_hud && (left.metadata.hud_crop!=right.metadata.hud_crop || left.metadata.hud_placement.values.values!=right.metadata.hud_placement.values.values || left.metadata.hud_prompt!=right.metadata.hud_prompt || left.metadata.hud_camera!=right.metadata.hud_camera || left.metadata.hud_camera_lock!=right.metadata.hud_camera_lock || left.metadata.hud_reticle!=right.metadata.hud_reticle || left.metadata.hud_corners!=right.metadata.hud_corners)) ||
        (require_tracked && (!left.metadata.tracked_stereo || !right.metadata.tracked_stereo))) { return std::nullopt; }
    return Pair{left,right};
}
bool EyeTextureCapture::reset(bool wait) {
    if (m_fence && m_fence->GetCompletedValue() != UINT64_MAX && m_fence->GetCompletedValue() < m_fence_value) {
        if (!wait || m_poisoned) { return false; }
        const auto event = CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!event) { return false; }
        const auto result = m_fence->SetEventOnCompletion(m_fence_value,event);
        const bool done = SUCCEEDED(result) && WaitForSingleObject(event,2000) == WAIT_OBJECT_0;
        CloseHandle(event);
        if (!done) { return false; }
    }
    for (auto& eye : m_slots) for (auto& slot : eye) { if (slot) { slot->source.Reset();slot->hud_source.Reset();slot->menu_source.Reset();slot->tap_target.Reset(); } }
    m_candidate_pair.reset(); m_latest = {}; m_slots = {}; m_menu_renderer.reset();m_hud_renderer.reset();m_guide_renderer.reset();m_guide_failed=false;m_vignette_renderer.reset();m_vignette_failed=false;m_fence.Reset(); m_device.Reset(); m_queue.Reset();
    m_fence_value = m_last_render_id = 0; m_poisoned = false; ++m_generation;
    return true;
}
EyeTextureCapture::~EyeTextureCapture() {
    if (!reset(true)) {
        // Preserve resources when a failed queue cannot prove completion; process-lifetime quarantine.
        struct Pending { decltype(m_slots) slots; ComPtr<ID3D12Fence> fence; ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12Device> device;MenuPanelRenderer renderer;NativeHudRenderer hud;GuardGuideRenderer guide;ComfortVignetteRenderer vignette; };
        static auto* quarantine = new std::vector<Pending>;
        quarantine->push_back({std::move(m_slots),m_fence,m_queue,m_device,std::move(m_menu_renderer),std::move(m_hud_renderer),std::move(m_guide_renderer),std::move(m_vignette_renderer)});
    }
}
