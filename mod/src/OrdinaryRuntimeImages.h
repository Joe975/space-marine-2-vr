#pragma once
#include "GpuOrdinaryPairRetention.h"
#include "PerformanceTrace.h"

// Present-thread bridge from completed native copies to runtime-owned shader
// resources. No raw resource-cache pointers or fabricated AFR frame numbers.
class OrdinaryRuntimeImages {
public:
    // Why capture() refused a pair, counted for the OrdinaryHeadset log line (a refused pair is dropped).
    enum class Refusal : uint8_t { state,tokens,device,copy,isolated,stale,slot_busy,capture_failed,count };
    static constexpr std::array<const char*,size_t(Refusal::count)> REFUSALS{"state","tokens","device","copy","isolated","stale","slot_busy","capture_failed"};
    // Every slot the submit thread can hold at once, plus the new capture (slot_busy otherwise).
    OrdinaryRuntimeImages(){for(auto& copy:m_copies)copy.slots(EyeTextureCapture::MAX_SLOTS);}
    bool capture(const GpuOrdinaryPairRetention::Pair& pair,const NativeOrdinaryPacket::Receipt& packet,ID3D12CommandQueue* queue,ID3D12Resource* hud=nullptr,const VrNativeHud& crop={},const VrCompactHud::Settings& placement={},bool queue_ordered=false,bool isolated=false,
                 ID3D12Resource* menu=nullptr,const VrMenuPanel& panel={},const VrNativePrompt& prompt=VrNativePrompt::off(),
                 const std::optional<VrGuardGuide::Draw>& guide={},const VrNativeHudCorners& corners=VrNativeHudCorners::off(),float vignette=0,
                 const std::optional<VrAimDot::Draw>& aim_dot={},bool hud_camera_lock=true,
                 const VrHudReticle& reticle=VrHudReticle::off(),const std::optional<std::array<float,3>>& reticle_target={},bool reticle_cut=true){
        const auto dispatch=pair.origin.scene.dispatch_sequence;
        if(!queue || m_pending || (menu && hud) || (isolated && (queue_ordered || hud || menu)) || !(queue_ordered?pair.gpu_submission_verified:pair.gpu_retention_verified) || !pair.cpu_verified || pair.refusal!=GpuOrdinaryPairRetention::Refusal::none ||
           !packet.accepted || !packet.reference || !packet.applied || packet.refusal || !packet.tracked || !packet.left_patched || !packet.patched || packet.rollback || packet.rollback_failed ||
           packet.dispatch!=dispatch || packet.render_id!=pair.origin.pair.left.render_id || packet.renderer!=pair.origin.scene.renderer ||
           packet.context.list!=pair.origin.scene.list || packet.context.counter!=pair.origin.scene.render_counter_before ||
           packet.tracked->producer_list!=pair.origin.pair.producer_list || packet.tracked->producer_revision!=pair.origin.pair.producer_revision)return refuse(Refusal::state);
        const auto& prepared=packet.tracked->prepared;
        if(prepared.render_id!=packet.render_id || prepared.game_token!=pair.origin.pair.left.token || pair.origin.pair.left.token!=pair.origin.pair.right.token ||
           pair.origin.pair.left.render_id!=pair.origin.pair.right.render_id)return refuse(Refusal::tokens);
        Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))return refuse(Refusal::device);
        for(size_t i=0;i<2;++i){const auto& copy=pair.eyes[i].copy;const auto& eye=prepared.eyes[i];
            if(!copy || (!queue_ordered && !copy->completed) || !copy->submitted || copy->refusal!=GpuInlineCopy::Refusal::none || !copy->texture || !copy->fence || !copy->fence_value ||
               copy->tag!=GpuOrdinaryPairRetention::tag(dispatch,uint32_t(i)) || copy->queue!=reinterpret_cast<uintptr_t>(queue) ||
               copy->fence->GetCompletedValue()==UINT64_MAX || (!queue_ordered && copy->fence->GetCompletedValue()<copy->fence_value) ||
               eye.tracking.eye!=int(i) || eye.tracking.render_token!=prepared.render_id || eye.tracking.synthetic || !eye.tracking.tracking_epoch)return refuse(Refusal::copy);
        }
        if(pair.eyes[0].copy->texture.Get()==pair.eyes[1].copy->texture.Get())return refuse(Refusal::copy);
        // Source copies are immutable COM leases and both real producer fences
        // have completed above. Only their read-only COPY_SOURCE state crosses
        // queues; all destination writes and runtime transfers use this queue.
        // Live native HUD has no independent completed lease: refuse isolation.
        ID3D12CommandQueue* bridge_queue=queue;
        if(isolated){
            Microsoft::WRL::ComPtr<ID3D12Device> owner;
            if(m_isolated_queue && (FAILED(m_isolated_queue->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device.Get()))return refuse(Refusal::isolated);
            if(!m_isolated_queue){
                D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
                if(device->GetNodeCount()!=1 || FAILED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&m_isolated_queue))))return refuse(Refusal::isolated);
                m_isolated_queue->SetName(L"SM2VR completed-eye runtime transfer");
            }
            bridge_queue=m_isolated_queue.Get();
        }
        const auto begin=PerformanceTrace::get().probes()?PerformanceTrace::now_ms():0;
        for(size_t i=0;i<2;++i){const auto& eye=prepared.eyes[i];EyeTextureCapture::Metadata meta;
            meta.game_token=prepared.game_token;meta.render_id=prepared.render_id;meta.eye=int(i);meta.tracked_stereo=true;meta.world=eye.world;meta.projection=eye.projection;
            meta.runtime_head=eye.tracking.runtime_head;meta.runtime_eye_to_head=eye.tracking.runtime_eye_to_head;meta.tracking_epoch=eye.tracking.tracking_epoch;meta.pose_captured=eye.tracking.captured;
            meta.native_hud=hud!=nullptr;meta.hud_crop=crop;meta.hud_placement=placement;meta.hud_prompt=prompt;meta.hud_corners=corners;
            // Paused/settings: both eyes show the native screen menu on a head-relative panel.
            meta.screen_menu=menu!=nullptr;meta.menu_panel=panel;meta.guard_guide=guide;meta.comfort_vignette=vignette;meta.aim_dot=aim_dot;
            // The HUD canvas (crosshair, markers) on the game's camera, not the head (THIRD_PERSON.md).
            // hud_camera_lock false (diorama, shoulder, chase: the eye is metres from the game camera) keeps it head-locked;
            // the game camera still sizes the crosshair patch at the aim point.
            if(prepared.native_valid){const auto& t=prepared.native_tangents;meta.hud_camera=VrHudCamera{prepared.native_world,t[0],t[1],t[2],t[3],eye.tracking.units_per_metre};}
            meta.hud_camera_lock=hud_camera_lock;meta.hud_reticle=reticle;meta.reticle_target=reticle_target;meta.reticle_cut=reticle_cut;
            const auto captured=m_copies[i].capture(device.Get(),bridge_queue,pair.eyes[i].copy->texture.Get(),meta,D3D12_RESOURCE_STATE_COPY_SOURCE,hud,menu);
            if(captured!=S_OK)return refuse(captured==S_FALSE?Refusal::stale:captured==E_PENDING?Refusal::slot_busy:Refusal::capture_failed);
        }
        m_pending=dispatch;m_probe_started=begin;m_pending_queue=bridge_queue;
        if(begin){PerformanceTrace::get().record("ordinary_bridge_begin",begin,0,double(prepared.render_id));PerformanceTrace::get().record("ordinary_bridge_dispatch",begin,0,double(dispatch));}
        if(begin && queue_ordered)PerformanceTrace::get().record("ordinary_bridge_queue_ordered",begin,0,double(dispatch));
        if(begin)PerformanceTrace::get().record("ordinary_bridge_isolated",begin,0,isolated?1:0);
        return true;
    }
    std::optional<VrSession::OrdinaryImages> take(){
        if(!m_pending)return {};for(auto& copy:m_copies)copy.poll();
        const auto left=m_copies[0].latest(0),right=m_copies[1].latest(1);
        if(!left || !right || !left->ready() || !right->ready() || left->metadata.render_id!=right->metadata.render_id)return {};
        VrSession::OrdinaryImages result{{*left,*right},*m_pending,m_pending_queue};
        if(m_probe_started)PerformanceTrace::get().record("ordinary_bridge_pending_cpu",m_probe_started,PerformanceTrace::now_ms()-m_probe_started,double(*m_pending));
        m_pending.reset();m_pending_queue.Reset();m_probe_started=0;return result;
    }
    // A camera mode photo (EyePhoto): the left eye of the next pair. Left is the game's own eye, the
    // one the right eye's parity work is compared against.
    void tap(EyeTextureCapture::Tap value){m_copies[0].tap(std::move(value));}
    std::optional<EyeTextureCapture::Tapped> take_tapped(){return m_copies[0].take_tapped();}
    bool pending()const{return m_pending.has_value();}
    const std::array<uint64_t,size_t(Refusal::count)>& refusals()const{return m_refusals;}
    // "slot_busy:12 stale:3", only the non-zero reasons ("none" when there are none).
    std::string refusal_summary()const{std::string out;for(size_t i=0;i<m_refusals.size();++i)if(m_refusals[i]){if(!out.empty())out+=' ';out+=REFUSALS[i];out+=':';out+=std::to_string(m_refusals[i]);}return out.empty()?"none":out;}
    bool reset(bool wait=false){for(auto& copy:m_copies)if(!copy.reset(wait))return false;m_pending.reset();m_pending_queue.Reset();m_isolated_queue.Reset();m_probe_started=0;return true;}
private:
    bool refuse(Refusal why){++m_refusals[size_t(why)];return false;}
    std::array<uint64_t,size_t(Refusal::count)> m_refusals{};
    std::array<EyeTextureCapture,2> m_copies;std::optional<uint64_t> m_pending;double m_probe_started{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_pending_queue,m_isolated_queue;
};
