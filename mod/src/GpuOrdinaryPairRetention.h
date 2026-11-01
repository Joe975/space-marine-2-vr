#pragma once
#include <string>
#include "NativeOrdinaryPairColour.h"
#include "NativeOrdinaryPacket.h"
#include "GpuCompositorOutput.h"
#include "PerformanceTrace.h"
#include <dxgi1_4.h>

// Bounded two-pair control. Four private destinations are never reused. This
// preserves per-eye images but grants no native history/runtime readiness claim.
// Source leases come exclusively from the actual live swapchain GetBuffer call.
class GpuOrdinaryPairRetention {
public:
    using Event=NativeOrdinaryPairColour::Event;using Scope=ordinary_scene_scope::Scope;
    using Finish=std::function<GpuInlineCopy::Sample(uint64_t)>;
    enum class Refusal:uint32_t {none,configuration,scope,order,busy,source,copy,redirect,changed,cpu,submission,exception};
    struct Eye {
        uint64_t tag{},begin_serial{},end_serial{};uint32_t slot{},buffer_index{},thread{};
        uintptr_t swapchain{},native_buffer{},native_buffer_identity{},private_output{};
        bool begun{},ended{};std::optional<Event> begin,end;
        std::optional<GpuInlineCopy::Sample> copy;std::optional<GpuCompositorOutput::Receipt> redirect;
    };
    struct Pair {
        Scope origin;std::array<Eye,2> eyes;std::optional<NativeOrdinaryPacket::Sdk> sdk;
        Refusal refusal{};bool cpu_verified{},gpu_retention_verified{},same_submission{},gpu_submission_verified{};
        size_t output_slot{};
    };
    struct Status {bool ready{},stopped{},active{};size_t pairs{};Refusal refusal{};};
    static uint64_t status_signature(const Status& state,bool enabled){return uint64_t(state.pairs)*16+uint64_t(state.refusal)*128+uint64_t(state.ready)+2*uint64_t(state.stopped)+4*uint64_t(state.active)+8*uint64_t(enabled);}
    Status status()const{std::lock_guard lock{m_mutex};return {m_ready,m_stopped,m_active.has_value(),m_pairs.size(),m_first_refusal};}
    std::string first_detail()const{std::lock_guard lock{m_mutex};return m_first_detail;}
    bool retire(uint64_t dispatch){
        std::lock_guard lock{m_mutex};if(m_active || !m_copy)return false;
        const auto pair=std::ranges::find_if(m_pairs,[&](const auto& p){return p.origin.scene.dispatch_sequence==dispatch;});
        if(pair==m_pairs.end() || !pair->gpu_retention_verified || pair->refusal!=Refusal::none)return false;
        for(const auto& eye:pair->eyes)if(!m_copy->retire(eye.tag)){stop(Refusal::copy);return false;}
        m_pairs.erase(pair);return true;
    }
    static constexpr size_t CAPACITY=2;
    static bool requested(std::string_view request,std::span<const std::string_view> dependencies,bool desktop_second,bool old_inline){
        return request=="1" && desktop_second && !old_inline && dependencies.size()==7 && std::ranges::all_of(dependencies,[](auto value){return value=="1";});
    }
    void invalidate(){std::lock_guard lock{m_mutex};if(s_active==this && m_active)abort_locked(Refusal::changed);
        else if(m_active){m_copy->invalidate_active(GpuInlineCopy::Refusal::configuration);fail(m_pairs[m_active->pair],Refusal::changed);}
        for(auto& pair:m_pairs)if(pair.refusal==Refusal::none)pair.refusal=Refusal::changed;stop(Refusal::changed);}
    // After invalidate() for a swap-chain reset, or a stop the cold restart clears: resume on the
    // SAME swap chain object (its buffers were recreated; the four private destinations were shaped
    // from a back buffer of the same size and stay valid) and the same device/queue. The refused
    // pairs are dropped; their copies were released by the consumer's own reset. A different swap
    // chain, or a configuration that never happened, is refused: that needs a full configure, which
    // this control does not redo.
    // The callers reset the inline copy first (no copy of a refused pair may still be pending), and
    // that reset drops the copy's device, queue and sources. Without them every later eye is refused
    // (Refusal::copy): every re-arm and cold restart failed that way on 27 Oct (08:51 and 09:39
    // runs). So an unconfigured copy is configured again here onto the same four destinations, on
    // the queue this control was configured with and no other.
    bool restart(IDXGISwapChain3* swap,ID3D12CommandQueue* queue){
        std::lock_guard lock{m_mutex};
        if(!m_ready || m_active || !swap || GpuObjectIdentity::get(swap)!=m_swapchain_identity || !m_outputs[0].texture())return false;
        Microsoft::WRL::ComPtr<ID3D12Resource> shape;
        if(FAILED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&shape))))return false;
        const auto want=shape->GetDesc(),have=m_outputs[0].texture()->GetDesc();
        if(want.Width!=have.Width || want.Height!=have.Height || want.Format!=have.Format)return false;
        // A poisoned output (an unsafe end) would refuse every eye: give it a fresh texture. The copy
        // is configured with the outputs' textures, so a renewal needs the copy reset first.
        for(auto& output:m_outputs)if(output.poisoned() && (m_copy->configured() || !output.renew()))return false;
        if(!m_copy->configured()){
            if(!queue || reinterpret_cast<uintptr_t>(queue)!=m_queue_identity)return false;
            std::array<ID3D12Resource*,CAPACITY*2> sources{};for(size_t i=0;i<sources.size();++i)sources[i]=m_outputs[i].texture();
            if(!m_copy->configure(m_device.Get(),queue,sources))return false;
        }
        m_pairs.clear();m_stopped=false;m_first_refusal=Refusal::none;m_first_detail.clear();return true;
    }
    bool consume(const Event* event,IDXGISwapChain3* swap){
        if(!event || event->refusal!=NativeOrdinaryPairColour::Refusal::none){abort();return false;}
        if(event->boundary.stage==NativeOrdinaryProbe::Stage::post)return true;
        return event->boundary.stage==NativeOrdinaryProbe::Stage::compositor_before?begin(*event,swap):end(*event);
    }
    static std::optional<uint64_t> tag(uint64_t dispatch,uint32_t slot){if(!dispatch || dispatch>UINT64_MAX/2 || slot>1)return {};return dispatch*2+slot;}
    bool configure(ID3D12Device* device,ID3D12CommandQueue* queue,IDXGISwapChain3* swap,GpuInlineCopy& copy,RenderTargetProbe& targets,
                   GpuCompositorOutput::Open open,GpuCompositorOutput::Clear clear,Finish finish){
        std::lock_guard lock{m_mutex};if(m_ready || !device || !queue || !swap || !open || !clear || !finish)return false;
        Microsoft::WRL::ComPtr<ID3D12Resource> shape;const auto index=swap->GetCurrentBackBufferIndex();
        if(FAILED(swap->GetBuffer(index,IID_PPV_ARGS(&shape))))return false;
        std::array<ID3D12Resource*,CAPACITY*2> sources{};
        for(size_t i=0;i<sources.size();++i){if(!m_outputs[i].configure(device,shape.Get(),open,clear))return false;sources[i]=m_outputs[i].texture();
            const auto identity=GpuObjectIdentity::get(sources[i]);if(!identity)return false;
            for(size_t j=0;j<i;++j)if(GpuObjectIdentity::get(sources[j])==identity)return false;}
        if(!copy.configure(device,queue,sources))return false;
        m_device=device;m_queue_identity=reinterpret_cast<uintptr_t>(queue);m_swapchain_identity=GpuObjectIdentity::get(swap);
        if(!m_swapchain_identity)return false;m_copy=&copy;m_targets=&targets;m_finish=std::move(finish);m_pairs.reserve(CAPACITY);m_ready=true;return true;
    }
    bool begin(const Event& event,IDXGISwapChain3* swap){
        std::lock_guard lock{m_mutex};if(!m_ready || m_stopped)return false;
        if(m_active){if(s_active==this)abort_locked(Refusal::busy);else{m_copy->invalidate_active(GpuInlineCopy::Refusal::busy);fail(m_pairs[m_active->pair],Refusal::busy);}return false;}
        if(s_active){stop(Refusal::busy);return false;}
        const auto& s=event.scope.scene;const auto id=tag(s.dispatch_sequence,s.slot);
        if(!id || !valid(event,NativeOrdinaryProbe::Stage::compositor_before)){stop(Refusal::scope);return false;}
        // After both admitted jobs have completed their native/SDK transaction,
        // skip BOTH eyes of later jobs. A later right eye must never be compared
        // against (and poison) the already retained second job.
        if(m_pairs.size()==CAPACITY && s.dispatch_sequence>m_pairs.back().origin.scene.dispatch_sequence &&
           m_pairs.back().cpu_verified && m_pairs.back().eyes[1].ended)return false;
        size_t pair_index{};
        if(!s.slot){
            if(m_pairs.size()==CAPACITY)return false;
            if(s.dispatch_sequence<=m_last_started){stop(Refusal::order);return false;}
            if(!m_pairs.empty() && (!m_pairs.back().cpu_verified || !m_pairs.back().eyes[1].ended || m_pairs.back().origin.scene.dispatch_sequence>=s.dispatch_sequence)){stop(Refusal::order);return false;}
            Pair pair;pair.origin=event.scope;
            for(size_t candidate=0;candidate<CAPACITY;++candidate)if(std::ranges::none_of(m_pairs,[&](const auto& p){return p.output_slot==candidate;})){pair.output_slot=candidate;break;}
            m_pairs.push_back(std::move(pair));pair_index=m_pairs.size()-1;m_last_started=s.dispatch_sequence;
        }else{
            if(m_pairs.empty()){stop(Refusal::order);return false;}pair_index=m_pairs.size()-1;auto& pair=m_pairs.back();
            auto left=event.scope;left.scene.slot=0;left.scene.ordinal=1;left.scene.entry=left.pair.left.entry;
            if(pair.origin.scene.dispatch_sequence!=s.dispatch_sequence || !ordinary_scene_scope::same(pair.origin,left) || !pair.eyes[0].ended || pair.eyes[1].begun){fail(pair,Refusal::order);return false;}
        }
        auto& pair=m_pairs[pair_index];auto& eye=pair.eyes[s.slot];
        if(pair.refusal!=Refusal::none || eye.begun || !swap || GpuObjectIdentity::get(swap)!=m_swapchain_identity){fail(pair,Refusal::source);return false;}
        Microsoft::WRL::ComPtr<ID3D12Resource> source;eye.buffer_index=swap->GetCurrentBackBufferIndex();
        if(FAILED(swap->GetBuffer(eye.buffer_index,IID_PPV_ARGS(&source)))){fail(pair,Refusal::source);return false;}
        eye.tag=*id;eye.slot=s.slot;eye.begin_serial=event.serial;eye.thread=GetCurrentThreadId();eye.swapchain=m_swapchain_identity;
        eye.native_buffer=reinterpret_cast<uintptr_t>(source.Get());eye.native_buffer_identity=GpuObjectIdentity::get(source.Get());
        const auto output_index=pair.output_slot*2+s.slot;eye.private_output=GpuObjectIdentity::get(m_outputs[output_index].texture());eye.begin=event;
        if(!eye.native_buffer_identity || eye.native_buffer_identity==eye.private_output || (s.slot && (eye.buffer_index!=pair.eyes[0].buffer_index || eye.native_buffer_identity!=pair.eyes[0].native_buffer_identity || eye.thread!=pair.eyes[0].thread))){fail(pair,Refusal::source);return false;}
        if(!m_copy->begin(*id,m_outputs[output_index].texture())){fail(pair,Refusal::copy);return false;}
        if(!m_outputs[output_index].begin(*id,source.Get())){m_copy->invalidate_active(GpuInlineCopy::Refusal::configuration);try{eye.copy=m_finish(*id);}catch(...){fail(pair,Refusal::exception);return false;}fail(pair,Refusal::redirect);return false;}
        eye.begun=true;m_active=Active{pair_index,s.slot,output_index};s_active=this;return true;
    }
    std::optional<RenderTargetProbe::Binding> transform(ID3D12GraphicsCommandList* command,const RenderTargetProbe::Binding& binding,std::span<const RenderTargetProbe::View> views){
        if(s_active!=this)return {};std::lock_guard lock{m_mutex};if(!m_active)return {};return m_outputs[m_active->output].transform(command,binding,views);
    }
    bool end(const Event& event){
        std::lock_guard lock{m_mutex};if(s_active!=this || !m_active)return false;
        auto& pair=m_pairs[m_active->pair];auto& eye=pair.eyes[m_active->slot];
        const bool current=valid(event,NativeOrdinaryProbe::Stage::compositor_after) && eye.begin && ordinary_scene_scope::same(eye.begin->scope,event.scope) &&
            event.serial==eye.begin_serial+1 && eye.thread==GetCurrentThreadId();
        if(!current){abort_locked(Refusal::changed);return false;}
        eye.end=event;eye.end_serial=event.serial;
        try{eye.copy=m_finish(eye.tag);}catch(...){m_copy->invalidate_active(GpuInlineCopy::Refusal::configuration);eye.redirect=m_outputs[m_active->output].end(eye.tag,false,*m_targets);eye.ended=true;fail(pair,Refusal::exception);s_active=nullptr;m_active.reset();return false;}
        eye.redirect=m_outputs[m_active->output].end(eye.tag,eye.copy->appended,*m_targets);eye.ended=true;
        s_active=nullptr;m_active.reset();
        if(!eye.copy->appended || eye.copy->refusal!=GpuInlineCopy::Refusal::none){
            note("copy slot="+std::to_string(eye.slot)+" appended="+std::to_string(eye.copy->appended)+" copy_refusal="+std::to_string(uint32_t(eye.copy->refusal))+" changed_site="+std::to_string(eye.copy->changed_site)+" other_thread="+std::to_string(eye.copy->changed_elsewhere)+" writes="+std::to_string(eye.copy->writes));
            fail(pair,Refusal::copy);return false;}
        if(eye.redirect->refusal!=GpuCompositorOutput::Refusal::none || !eye.redirect->restored || !eye.redirect->bindings ||
           eye.redirect->target!=eye.private_output || eye.redirect->source!=eye.native_buffer_identity || eye.redirect->tag!=eye.tag ||
           eye.redirect->commands!=eye.copy->commands || eye.redirect->generation!=eye.copy->generation || eye.copy->identity!=eye.private_output){
            const auto& r=*eye.redirect;const auto& c=*eye.copy;
            note("redirect slot="+std::to_string(eye.slot)+" output_refusal="+std::to_string(uint32_t(r.refusal))+" restored="+std::to_string(r.restored)+" bindings="+std::to_string(r.bindings)+
                " target_ok="+std::to_string(r.target==eye.private_output)+" source_ok="+std::to_string(r.source==eye.native_buffer_identity)+" tag_ok="+std::to_string(r.tag==eye.tag)+
                " commands_ok="+std::to_string(r.commands==c.commands)+" generation="+std::to_string(r.generation)+"/"+std::to_string(c.generation)+" identity_ok="+std::to_string(c.identity==eye.private_output));
            fail(pair,Refusal::redirect);return false;}
        return true;
    }
    void abort(){std::lock_guard lock{m_mutex};if(s_active==this && m_active)abort_locked(Refusal::scope);}
    void complete(const NativeRenderProbe::Snapshot& snapshot,const NativeOrdinaryPacket::Receipt& receipt){
        std::lock_guard lock{m_mutex};auto found=std::ranges::find_if(m_pairs,[&](const auto& pair){return pair.origin.scene.dispatch_sequence==snapshot.sequence;});if(found==m_pairs.end())return;auto& pair=*found;
        if(pair.refusal!=Refusal::none)return;const auto& origin=pair.origin;
        bool accepted=!m_active && pair.eyes[0].ended && pair.eyes[1].ended && !pair.cpu_verified && snapshot.ordinary_pair && snapshot.scene_calls==2 && !snapshot.scene_refused && !snapshot.scene_dropped && snapshot.scenes.size()==2 &&
            snapshot.list==origin.scene.list && snapshot.renderer==origin.scene.renderer && receipt.dispatch==snapshot.sequence && receipt.render_id==origin.pair.left.render_id && receipt.renderer==snapshot.renderer &&
            receipt.context.list==snapshot.list && receipt.context.counter==origin.scene.render_counter_before && receipt.accepted && !receipt.refusal && receipt.reference && receipt.applied && (receipt.source_unchanged || (receipt.tracked && receipt.left_patched)) &&
            !receipt.rollback && !receipt.rollback_failed && receipt.sdk && receipt.sdk->epoch && receipt.sdk->ordinal;
        if(accepted){auto joined=origin;joined.pair=*snapshot.ordinary_pair;accepted=ordinary_scene_scope::same(origin,joined) && NativeCameraEntries::get().current(*snapshot.ordinary_pair);}
        if(accepted)for(size_t slot=0;slot<2;++slot){const auto& scene=snapshot.scenes[slot];const auto& eye=pair.eyes[slot];const auto& before=eye.begin->scope.scene;
            if(!scene.valid() || scene.dispatch_sequence!=snapshot.sequence || scene.ordinal!=slot+1 || scene.slot!=slot || scene.entry!=before.entry || scene.renderer!=before.renderer || scene.list!=before.list ||
               scene.render_counter_before!=before.render_counter_before || scene.render_counter_after!=before.render_counter_before || scene.camera_revision_before!=before.camera_revision_before || scene.camera_revision_after!=before.camera_revision_before ||
               scene.view_flags_before!=before.view_flags_before || scene.view_flags_after!=before.view_flags_before || scene.active_entry_after!=scene.entry)accepted=false;}
        if(!accepted){fail(pair,Refusal::cpu);return;}pair.sdk=receipt.sdk;pair.cpu_verified=true;
        if(PerformanceTrace::get().probes())PerformanceTrace::get().record("ordinary_pair_cpu_verified",PerformanceTrace::now_ms(),0,double(snapshot.sequence));
    }
    std::vector<Pair> samples(){
        std::lock_guard lock{m_mutex};if(!m_copy)return {};const auto copies=m_copy->samples();
        for(auto& pair:m_pairs){bool complete=pair.cpu_verified && pair.refusal==Refusal::none;
            for(auto& eye:pair.eyes){const auto found=std::ranges::find(copies,eye.tag,&GpuInlineCopy::Sample::tag);if(found!=copies.end())eye.copy=*found;
                if(found==copies.end() || !eye.copy || eye.copy->tag!=eye.tag || eye.tag!=tag(pair.origin.scene.dispatch_sequence,eye.slot) || eye.copy->thread!=eye.thread ||
                   !eye.copy->appended || !eye.copy->submitted || eye.copy->refusal!=GpuInlineCopy::Refusal::none || eye.copy->queue!=m_queue_identity ||
                   eye.copy->identity!=eye.private_output || !eye.copy->texture || !eye.copy->fence || !eye.copy->fence_value)complete=false;}
            if(complete){const auto& a=*pair.eyes[0].copy;const auto& b=*pair.eyes[1].copy;
                complete=a.tag!=b.tag && a.identity!=b.identity && a.submission<=b.submission && GpuObjectIdentity::get(a.texture.Get())!=GpuObjectIdentity::get(b.texture.Get()) &&
                    a.width==b.width && a.height==b.height && a.format==b.format;pair.same_submission=a.submission==b.submission;}
            pair.gpu_submission_verified=complete;
            pair.gpu_retention_verified=complete && pair.eyes[0].copy->completed && pair.eyes[1].copy->completed;
        }return m_pairs;
    }
    static void write(std::ostream& out,const Pair& pair){
        const auto& p=pair.origin.pair;out<<"{\"model\":\"ordinary_pair_gpu_retention_v1\",\"dispatch\":"<<pair.origin.scene.dispatch_sequence<<",\"render_id\":"<<p.left.render_id<<",\"revision\":"<<p.left.revision<<",\"token\":"<<p.left.token<<",\"list\":"<<pair.origin.scene.list<<",\"renderer\":"<<pair.origin.scene.renderer<<",\"producer_list\":"<<p.producer_list<<",\"producer_revision\":"<<p.producer_revision<<",\"refusal\":"<<uint32_t(pair.refusal)<<",\"cpu_verified\":"<<(pair.cpu_verified?"true":"false")<<",\"gpu_retention_verified\":"<<(pair.gpu_retention_verified?"true":"false")<<",\"same_submission\":"<<(pair.same_submission?"true":"false")<<",\"native_gpu_histories_verified\":false,\"gpu_pair_verified\":false,\"runtime_submission\":false,\"retirement\":\"process_lifetime\",\"sdk\":";
        if(pair.sdk)out<<"{\"epoch\":"<<pair.sdk->epoch<<",\"ordinal\":"<<pair.sdk->ordinal<<",\"token\":"<<pair.sdk->token<<'}';else out<<"null";out<<",\"eyes\":[";
        for(size_t i=0;i<2;++i){if(i)out<<',';const auto& e=pair.eyes[i];out<<"{\"slot\":"<<e.slot<<",\"tag\":"<<e.tag<<",\"begin_serial\":"<<e.begin_serial<<",\"end_serial\":"<<e.end_serial<<",\"buffer_index\":"<<e.buffer_index<<",\"thread\":"<<e.thread<<",\"swapchain\":"<<e.swapchain<<",\"native_buffer\":"<<e.native_buffer<<",\"native_buffer_identity\":"<<e.native_buffer_identity<<",\"private_output\":"<<e.private_output<<",\"begun\":"<<(e.begun?"true":"false")<<",\"ended\":"<<(e.ended?"true":"false")<<",\"copy\":";
            if(e.copy)GpuInlineCopy::write_sample(out,*e.copy);else out<<"null";out<<",\"fence\":"<<(e.copy?reinterpret_cast<uintptr_t>(e.copy->fence.Get()):0)<<",\"redirect\":";if(e.redirect)GpuCompositorOutput::write_receipt(out,*e.redirect);else out<<"null";out<<'}';}out<<"]}";
    }
private:
    static bool valid(const Event& event,NativeOrdinaryProbe::Stage stage){
        const auto expected=ordinary_scene_scope::inspect(event.scope);return event.refusal==NativeOrdinaryPairColour::Refusal::none && event.serial && event.thread==GetCurrentThreadId() &&
            event.boundary.stage==stage && event.before==event.after && event.before==expected && !expected.mask && ordinary_scene_scope::inspect(event.scope)==expected &&
            event.output.status==NativeRenderProbe::OutputStatus::observed && event.boundary.wrapper && !event.boundary.entry && event.output.resource &&
            event.boundary.renderer_state==event.scope.scene.renderer+0x10;
    }
    void stop(Refusal refusal){if(m_first_refusal==Refusal::none)m_first_refusal=refusal;m_stopped=true;}
    // What the first copy or redirect refusal saw (29 Oct: a copy refusal stopped stereo for the
    // session, and after each cold restart the redirect refused; neither said why).
    void note(std::string detail){if(m_first_detail.empty())m_first_detail=std::move(detail);}
    void fail(Pair& pair,Refusal refusal){if(pair.refusal==Refusal::none)pair.refusal=refusal;stop(refusal);}
    void abort_locked(Refusal refusal){auto& pair=m_pairs[m_active->pair];auto& eye=pair.eyes[m_active->slot];m_copy->invalidate_active(GpuInlineCopy::Refusal::configuration);
        try{eye.copy=m_finish(eye.tag);}catch(...){refusal=Refusal::exception;}eye.redirect=m_outputs[m_active->output].end(eye.tag,false,*m_targets);eye.ended=true;fail(pair,refusal);s_active=nullptr;m_active.reset();}
    struct Active {size_t pair{};uint32_t slot{};size_t output{};};
    mutable std::mutex m_mutex;bool m_ready{},m_stopped{};Refusal m_first_refusal{};std::optional<Active> m_active;std::vector<Pair> m_pairs;
    uint64_t m_last_started{};
    std::string m_first_detail;
    std::array<GpuCompositorOutput,CAPACITY*2> m_outputs;Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    GpuInlineCopy* m_copy{};RenderTargetProbe* m_targets{};Finish m_finish;uintptr_t m_queue_identity{},m_swapchain_identity{};
    inline static thread_local GpuOrdinaryPairRetention* s_active{};
};
