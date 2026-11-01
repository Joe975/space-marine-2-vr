#include "GpuCompletionProbe.h"
#include "GpuQueueTimeline.h"
#include "GpuObjectIdentity.h"
#include "GpuCommandCoverage.h"
#include "RenderTargetProbe.h"
#include "GpuClosedListProbe.h"
#include "GpuApiForwardScope.h"
#include <span>
#include <MinHook.h>
#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <cmath>

namespace {
constexpr auto command_slots=GpuCommandCoverage::slots;
struct RecordingStamp {uint64_t magic{0x534d325245434f52},generation{},epoch{};uintptr_t identity{};};
}

bool GpuCompletionProbe::install(ID3D12Device* device,ID3D12CommandQueue* queue,std::function<uintptr_t(uintptr_t)> resolve,std::function<std::optional<Origin>()> origin,std::function<bool(uintptr_t)> binding_coverage) {
    if(s_probe || !device || !queue || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    if(FAILED(queue->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device) return false;
    m_device=device;m_queue=queue;m_resolve=std::move(resolve);m_origin=std::move(origin);m_binding_coverage=std::move(binding_coverage);
    if(!GpuClosedListProbe::get().install(device)) return false;
    // Native private-history creation can install the same process-lifetime
    // allocation observer first. Reuse its device-checked metadata producer.
    if(!GpuAllocationProbe::observes(device) && !m_allocation.install(device)) return false;
    for(auto& work:m_work) if(!create_work(work)) return false;
    if(!m_extended_writers.install(m_work[0].markers[0].Get(),[this](uintptr_t command,GpuTargetAccess::Extended kind,uintptr_t resource,uintptr_t callsite,std::optional<GpuAllocationProbe::Record> allocation){extended_writer(command,kind,resource,callsite,std::move(allocation));})) return false;
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return false;
    auto** commands=*reinterpret_cast<void***>(m_work[0].markers[0].Get());auto** queues=*reinterpret_cast<void***>(queue);
    if(commands[9]!=GpuClosedListProbe::get().address()) return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
    m_barrier_capabilities_known=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,&options,sizeof(options)));
    m_enhanced_supported=m_barrier_capabilities_known && options.EnhancedBarriersSupported;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> extended;
    const bool has_extended=SUCCEEDED(m_work[0].markers[0].As(&extended));
    if(!GpuStateEvidence::coverage(m_barrier_capabilities_known,m_enhanced_supported,has_extended)) return false;
    std::vector<void*> addresses{commands[10],commands[9],commands[12],commands[13],queues[10],commands[48],commands[17],commands[16],commands[19],commands[26]};
    std::vector<void*> hooks{reinterpret_cast<void*>(&on_reset),reinterpret_cast<void*>(&on_close),reinterpret_cast<void*>(&on_draw),reinterpret_cast<void*>(&on_indexed),reinterpret_cast<void*>(&on_execute),reinterpret_cast<void*>(&on_clear),reinterpret_cast<void*>(&on_copy),reinterpret_cast<void*>(&on_copy_texture),reinterpret_cast<void*>(&on_resolve),reinterpret_cast<void*>(&on_barriers)};
    addresses.insert(addresses.end(),{commands[14],commands[59],commands[27],commands[49],commands[50]});
    hooks.insert(hooks.end(),{reinterpret_cast<void*>(&on_dispatch),reinterpret_cast<void*>(&on_indirect),reinterpret_cast<void*>(&on_bundle),reinterpret_cast<void*>(&on_uav_uint),reinterpret_cast<void*>(&on_uav_float)});
    if(has_extended) {addresses.push_back((*reinterpret_cast<void***>(extended.Get()))[80]);hooks.push_back(reinterpret_cast<void*>(&on_enhanced));}
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> pass;
    const auto pass_hr=m_work[0].markers[0].As(&pass);m_binding_capabilities_known=SUCCEEDED(pass_hr) || pass_hr==E_NOINTERFACE;
    if(!m_binding_capabilities_known) return false;
    m_list4_supported=SUCCEEDED(pass_hr);const auto binding_index=addresses.size();
    m_binding_addresses[0]=commands[11];addresses.push_back(commands[11]);hooks.push_back(reinterpret_cast<void*>(&on_clear_state));
    if(m_list4_supported) {
        auto** table=*reinterpret_cast<void***>(pass.Get());m_binding_addresses[1]=table[68];m_binding_addresses[2]=table[69];
        addresses.insert(addresses.end(),{table[68],table[69]});hooks.insert(hooks.end(),{reinterpret_cast<void*>(&on_begin_pass),reinterpret_cast<void*>(&on_end_pass)});
    }
    for(size_t i=0;i<command_slots.size();++i) m_command_addresses[i]=commands[command_slots[i]];
    if(has_extended) m_enhanced_address=(*reinterpret_cast<void***>(extended.Get()))[80];
    std::vector<void*> originals(addresses.size());size_t made{};
    for(;made<addresses.size();++made) {
        if(made==1) {originals[made]=reinterpret_cast<void*>(GpuClosedListProbe::get().original());continue;}
        if(MH_CreateHook(addresses[made],hooks[made],&originals[made])!=MH_OK) break;
    }
    if(made!=addresses.size()) {for(size_t i=0;i<made;++i) if(i!=1) MH_RemoveHook(addresses[i]);return false;}
    s_reset=reinterpret_cast<Reset>(originals[0]);s_close=reinterpret_cast<Close>(originals[1]);s_draw=reinterpret_cast<Draw>(originals[2]);
    s_indexed=reinterpret_cast<Indexed>(originals[3]);s_execute=reinterpret_cast<Execute>(originals[4]);s_clear=reinterpret_cast<Clear>(originals[5]);s_probe=this;
    s_copy=reinterpret_cast<Copy>(originals[6]);s_copy_texture=reinterpret_cast<CopyTexture>(originals[7]);s_resolve=reinterpret_cast<Resolve>(originals[8]);
    s_barriers=reinterpret_cast<Barriers>(originals[9]);
    s_dispatch=reinterpret_cast<Dispatch>(originals[10]);s_indirect=reinterpret_cast<Indirect>(originals[11]);s_bundle=reinterpret_cast<Bundle>(originals[12]);
    s_uav_uint=reinterpret_cast<UavUint>(originals[13]);s_uav_float=reinterpret_cast<UavFloat>(originals[14]);
    if(has_extended) s_enhanced=reinterpret_cast<Enhanced>(originals[15]);
    s_clear_state=reinterpret_cast<ClearState>(originals[binding_index]);
    if(m_list4_supported) {s_begin_pass=reinterpret_cast<BeginPass>(originals[binding_index+1]);s_end_pass=reinterpret_cast<EndPass>(originals[binding_index+2]);}
    for(size_t i=0;i<addresses.size();++i) if(i!=1 && MH_EnableHook(addresses[i])!=MH_OK) {
        for(size_t j=0;j<addresses.size();++j) if(j!=1) MH_DisableHook(addresses[j]);s_probe=nullptr;
        for(size_t j=0;j<addresses.size();++j) if(j!=1) MH_RemoveHook(addresses[j]);return false;
    }
    GpuClosedListProbe::get().observer([this](uintptr_t command,bool ok) {
        if(!s_internal && m_inline)m_inline->closed(reinterpret_cast<ID3D12GraphicsCommandList*>(command),ok);
        if(s_internal) return;std::lock_guard lock{m_mutex};const auto it=m_recordings.find(command);if(it!=m_recordings.end()) it->second.closed=ok;
    });
    GpuClosedListProbe::get().creation_observer([this](uintptr_t command,bool closed) {
        if(s_internal) return;reset(command);if(m_inline)m_inline->closed(reinterpret_cast<ID3D12GraphicsCommandList*>(command),closed);std::lock_guard lock{m_mutex};const auto found=m_recordings.find(command);if(found!=m_recordings.end()) found->second.closed=closed;
    });
    m_enhanced_hook_installed=has_extended;m_base_writer_hook_mask=GpuStateEvidence::BASE_WRITER_HOOKS;m_binding_hook_mask=m_list4_supported?7:1;return true;
}
bool GpuCompletionProbe::create_work(Work& w) {
    if(FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&w.allocator)))) return false;
    for(auto& marker:w.markers) if(FAILED(m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,w.allocator.Get(),nullptr,IID_PPV_ARGS(&marker))) || FAILED(marker->Close())) return false;
    D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;
    if(FAILED(m_device->CreateQueryHeap(&q,IID_PPV_ARGS(&w.queries))) || FAILED(m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&w.fence)))) return false;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=16;buffer.Height=1;
    buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(m_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&w.buffer)));
}
bool GpuCompletionProbe::target(ID3D12Resource* resource) {
    std::lock_guard lock{m_mutex};if(m_target.Get()==resource) return bool(resource);
    if(resource) {Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) || device.Get()!=m_device.Get()) return false;}
    const auto identity=GpuAllocationProbe::identity(resource);if(resource && !identity) return false;
    m_target=resource;++m_epoch;m_latest.reset();m_recordings.clear();m_target_state.reset();m_snapshot_request.reset();m_snapshot.reset();m_snapshot_status={};
    m_epoch_barrier_hazards=0;
    m_has_target.store(bool(m_target),std::memory_order_release);
    m_target_identity=identity;
    const auto allocation=GpuAllocationProbe::read(resource);m_access=allocation?allocation->access():GpuTargetAccess{};
    const auto desc=resource?resource->GetDesc():D3D12_RESOURCE_DESC{};
    m_single_subresource=desc.MipLevels==1 && desc.DepthOrArraySize==1;
    m_simultaneous=(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)!=0;m_state_raster=m_state_decay=false;return bool(resource);
}
void GpuCompletionProbe::clear_target() {target(nullptr);}
void GpuCompletionProbe::reset(uintptr_t command) {
    const auto covered=binding_covered(command);
    std::lock_guard lock{m_mutex};
    if(m_recordings.size()>=2048 && !m_recordings.contains(command)) {m_recordings.clear();++m_snapshot_status.recording_cache_clears;}
    m_recordings[command]={++m_generation,m_epoch,0,false,false};
    m_recordings[command].binding_covered=covered;
    auto* native=reinterpret_cast<ID3D12GraphicsCommandList*>(command);
    RecordingStamp stamp;stamp.generation=m_generation;stamp.epoch=m_epoch;stamp.identity=GpuObjectIdentity::get(native);
    if(!stamp.identity || FAILED(native->SetPrivateData(RECORDING_KEY,sizeof(stamp),&stamp))) m_recordings[command].binding_covered=false;
    if(m_inline)m_inline->recording(native,m_generation,m_recordings[command].binding_covered);
}
bool GpuCompletionProbe::recording_matches(uintptr_t command,const Recording& recording) const {
    if(!command) return false;
    auto* native=reinterpret_cast<ID3D12CommandList*>(command);RecordingStamp stamp{};UINT size=sizeof(stamp);
    return SUCCEEDED(native->GetPrivateData(RECORDING_KEY,&size,&stamp)) && size==sizeof(stamp) &&
        stamp.magic==0x534d325245434f52 && stamp.generation==recording.generation && stamp.epoch==recording.epoch &&
        stamp.identity && stamp.identity==GpuObjectIdentity::get(native);
}
void GpuCompletionProbe::bound(uintptr_t command,std::span<const uintptr_t> resources,bool known) {
    if(s_internal) return;
    if(m_inline)m_inline->bound(reinterpret_cast<ID3D12GraphicsCommandList*>(command),resources,known);
    if(!m_has_target.load(std::memory_order_acquire)) return; // bound and binding_known judge the target; a new target starts a new epoch.
    std::lock_guard lock{m_mutex};auto it=m_recordings.find(command);if(it==m_recordings.end()) return;
    it->second.bound=m_target && std::ranges::find(resources,m_target_identity)!=resources.end();
    it->second.binding_known=known && it->second.binding_covered;
}
bool GpuCompletionProbe::binding_covered(uintptr_t command) const {
    if(!command || !m_binding_capabilities_known || !m_binding_hook_mask || !m_binding_coverage) return false;
    auto* native=reinterpret_cast<ID3D12GraphicsCommandList*>(command);
    auto** base=*reinterpret_cast<void***>(native);
    if(!GpuCommandCoverage::matches(m_command_addresses,base)) return false;
    if(m_enhanced_hook_installed) {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> enhanced;
        if(FAILED(native->QueryInterface(IID_PPV_ARGS(&enhanced))) || (*reinterpret_cast<void***>(enhanced.Get()))[80]!=m_enhanced_address) return false;
    }
    if(!m_extended_writers.covers(native)) return false;
    if((*reinterpret_cast<void***>(native))[11]!=m_binding_addresses[0]) return false;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> pass;const auto hr=native->QueryInterface(IID_PPV_ARGS(&pass));
    if(m_list4_supported) {
        if(FAILED(hr)) return false;auto** table=*reinterpret_cast<void***>(pass.Get());
        if(table[68]!=m_binding_addresses[1] || table[69]!=m_binding_addresses[2]) return false;
    } else if(hr!=E_NOINTERFACE) return false;
    try {return m_binding_coverage(command);} catch(...) {return false;}
}
void GpuCompletionProbe::binding_state(uintptr_t command,uint32_t kind) {
    if(s_internal) return;
    if(m_inline){if(kind)m_inline->hazard(reinterpret_cast<ID3D12GraphicsCommandList*>(command),GpuStateEvidence::implicit_binding);else m_inline->bound(reinterpret_cast<ID3D12GraphicsCommandList*>(command),{},true);}
    std::lock_guard lock{m_mutex};if(!m_target) return;++m_snapshot_status.binding_calls[kind];
    const auto it=m_recordings.find(command);const bool tracked=it!=m_recordings.end() && it->second.epoch==m_epoch && !it->second.closed;
    if(tracked) {
        it->second.bound=false;it->second.binding_known=kind==0 && it->second.binding_covered;
        // Begin/End pass declare implicit RTVs and may clear, resolve or discard.
        // Until modeled, no later legacy bind/transition can erase this hazard.
        if(kind) it->second.state.invalidate(GpuStateEvidence::implicit_binding);
    } else if(kind) {++m_snapshot_status.untracked_hazards;m_epoch_barrier_hazards|=GpuStateEvidence::implicit_binding;}
}
void GpuCompletionProbe::extended_writer(uintptr_t command,GpuTargetAccess::Extended kind,uintptr_t resource,uintptr_t callsite,std::optional<GpuAllocationProbe::Record> allocation) {
    if(s_internal) return;
    // No-UAV swap buffers exclude depth/compute accesses. Other extended APIs
    // remain sticky refusals in the bounded inline control.
    if(m_inline)m_inline->extended(reinterpret_cast<ID3D12GraphicsCommandList*>(command),kind,resource,callsite,std::move(allocation));
    std::lock_guard lock{m_mutex};if(!m_target) return;
    ++m_snapshot_status.extended_calls[kind];m_snapshot_status.extended_kind=kind;m_snapshot_status.extended_callsite=callsite;
    const auto it=m_recordings.find(command);const bool tracked=it!=m_recordings.end() && it->second.epoch==m_epoch && !it->second.closed;
    if(m_access.exclude_extended(kind,resource,tracked && it->second.binding_known,tracked && it->second.bound,tracked && it->second.binding_covered)) {++m_snapshot_status.extended_exclusions[kind];return;}
    if(tracked) it->second.state.invalidate(GpuStateEvidence::extended_writer);
    else {++m_snapshot_status.untracked_hazards;m_epoch_barrier_hazards|=GpuStateEvidence::extended_writer;}
}
void GpuCompletionProbe::observe(Recording& recording,Kind kind,uintptr_t callsite,std::optional<Origin> origin) {
    if(origin) {
        if(!origin->render_id || origin->record<0) origin.reset();
        else for(size_t i=0;i<16;++i) if(!std::isfinite(origin->source_world[i]) || !std::isfinite(origin->render_world[i]) ||
            std::abs(origin->source_world[i]-origin->render_world[i])>1e-4f) {origin.reset();break;}
    }
    if(!recording.writes) recording.origin=origin;
    else if(!origin || recording.origin!=origin) recording.origin.reset();
    ++recording.writes;++recording.kinds[static_cast<size_t>(kind)];
    recording.last_write_callsite=callsite;recording.last_write_thread=GetCurrentThreadId();
}
void GpuCompletionProbe::draw(uintptr_t command,Kind kind,uintptr_t callsite) {
    if(s_internal || !m_has_target.load(std::memory_order_acquire)) return; // Target evidence only.
    uint64_t generation{},epoch{};
    {
        std::lock_guard lock{m_mutex};const auto it=m_recordings.find(command);
        if(it!=m_recordings.end() && it->second.epoch==m_epoch && !it->second.closed && !it->second.binding_known) {
            it->second.state.invalidate(GpuStateEvidence::implicit_binding);return;
        }
        if(it==m_recordings.end() || it->second.epoch!=m_epoch || !it->second.bound || it->second.closed) return;
        generation=it->second.generation;epoch=m_epoch;
    }
    // Camera ownership uses a different mutex. Resolve outside this lock to
    // preserve the Present path's camera -> GPU observer lock order.
    std::optional<Origin> origin;try {if(m_origin) origin=m_origin();} catch(...) {}
    std::lock_guard lock{m_mutex};auto it=m_recordings.find(command);
    if(it!=m_recordings.end() && epoch==m_epoch && it->second.generation==generation && it->second.bound && !it->second.closed) {
        observe(it->second,kind,callsite,origin);
        // An actual write to a single-subresource RTV requires RENDER_TARGET at
        // this command. Binding alone is insufficient. Pending split state is
        // deliberately not repaired by assuming native usage was valid.
        it->second.state.raster_write(m_single_subresource);
    }
}
void GpuCompletionProbe::write(uintptr_t command,uintptr_t resource,Kind kind,uintptr_t callsite) {
    if(s_internal || !resource || !m_has_target.load(std::memory_order_acquire)) return;
    std::lock_guard lock{m_mutex};auto it=m_recordings.find(command);
    if(!m_target || resource!=m_target_identity) return;
    if(it!=m_recordings.end() && it->second.epoch==m_epoch && !it->second.closed && recording_matches(command,it->second)) observe(it->second,kind,callsite);
    else {++m_snapshot_status.untracked_hazards;++m_snapshot_status.untracked_target_writes;m_epoch_barrier_hazards|=GpuStateEvidence::untracked_target_writer;}
}
void GpuCompletionProbe::barriers(uintptr_t command,UINT count,const D3D12_RESOURCE_BARRIER* values,uintptr_t callsite) {
    if(s_internal) return;
    if(m_inline)m_inline->barriers(reinterpret_cast<ID3D12GraphicsCommandList*>(command),count,values);
    if(!m_has_target.load(std::memory_order_acquire)) return;
    std::lock_guard lock{m_mutex};auto it=m_recordings.find(command);
    if(!m_target) return;
    ++m_snapshot_status.barrier_calls;
    const bool tracked=it!=m_recordings.end() && it->second.epoch==m_epoch && !it->second.closed;
    const auto counts=GpuStateEvidence::legacy(tracked?&it->second.state:nullptr,m_target.Get(),m_single_subresource,count,values,[this](const auto& value) {
        if(!m_access.private_render_target()) return false;
        if(value.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            const auto before=GpuAllocationProbe::identity(value.Aliasing.pResourceBefore),after=GpuAllocationProbe::identity(value.Aliasing.pResourceAfter);
            return m_access.exclude_alias(before,after,(!value.Aliasing.pResourceBefore || before) && (!value.Aliasing.pResourceAfter || after));
        }
        // A target-named UAV barrier is inconsistent with this non-UAV target.
        return value.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV && !value.UAV.pResource && m_access.exclude_compute();
    },[this](ID3D12Resource* value){return value && GpuAllocationProbe::identity(value)==m_target_identity;});
    m_snapshot_status.target_transitions+=counts.transitions;m_snapshot_status.aliasing_barriers+=counts.aliases;
    m_snapshot_status.uav_barriers+=counts.uavs;m_snapshot_status.malformed_barriers+=counts.malformed;
    m_snapshot_status.excluded_aliases+=counts.excluded_aliases;m_snapshot_status.excluded_uavs+=counts.excluded_uavs;
    if(values && count<=4096) for(UINT i=0;i<count;++i) if(values[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        const auto& alias=values[i].Aliasing;
        const auto target_identity=m_target_identity;
        const bool named=(alias.pResourceBefore && GpuAllocationProbe::identity(alias.pResourceBefore)==target_identity) ||
            (alias.pResourceAfter && GpuAllocationProbe::identity(alias.pResourceAfter)==target_identity);
        if(named) ++m_snapshot_status.alias_target_named;
        else if(!alias.pResourceBefore && !alias.pResourceAfter) ++m_snapshot_status.alias_both_null;
        else ++m_snapshot_status.alias_other;
        const auto ordinal=m_snapshot_status.alias_target_named+m_snapshot_status.alias_both_null+m_snapshot_status.alias_other;
        // Bounded-rate diagnostic samples only; all barriers feed the tested
        // access policy above. Metadata is copied while arguments
        // are live, never recovered later through potentially reused pointers.
        if(ordinal%4096==1 || (named && m_snapshot_status.alias_target_named%64==1)) {
            m_snapshot_status.alias_sample={ordinal,command,reinterpret_cast<uintptr_t>(alias.pResourceBefore),reinterpret_cast<uintptr_t>(alias.pResourceAfter),callsite,GetCurrentThreadId(),
                GpuAllocationProbe::read(alias.pResourceBefore),GpuAllocationProbe::read(alias.pResourceAfter)};
        }
    }
    if(!tracked) {
        m_snapshot_status.untracked_transitions+=counts.transitions;m_snapshot_status.untracked_hazards+=counts.aliases-counts.excluded_aliases+counts.uavs-counts.excluded_uavs+counts.malformed;
        const auto hazards=(counts.aliases>counts.excluded_aliases?GpuStateEvidence::aliasing:0)|(counts.uavs>counts.excluded_uavs?GpuStateEvidence::uav:0)|(counts.malformed?GpuStateEvidence::malformed:0);
        m_epoch_barrier_hazards|=GpuStateEvidence::epoch_hazards(true,false,hazards);
    }
}
void GpuCompletionProbe::enhanced_barriers(uintptr_t command,UINT count,const D3D12_BARRIER_GROUP* groups) {
    if(s_internal) return;
    std::lock_guard lock{m_mutex};if(!m_target) return;
    ++m_snapshot_status.enhanced_calls;
    const auto it=m_recordings.find(command);
    const bool tracked=it!=m_recordings.end() && it->second.epoch==m_epoch && !it->second.closed;
    GpuStateEvidence::enhanced_barriers(tracked?&it->second.state:nullptr,count,groups);
    if(count && !tracked) {++m_snapshot_status.untracked_hazards;m_epoch_barrier_hazards|=GpuStateEvidence::epoch_hazards(true,false,GpuStateEvidence::enhanced);}
    if(count>4096 || (count && !groups)) ++m_snapshot_status.malformed_barriers;
}
void GpuCompletionProbe::indirect(uintptr_t command,ID3D12CommandSignature* signature,UINT count,uintptr_t callsite) {
    if(s_internal) return;
    const auto value=GpuAllocationProbe::read_signature(signature);
    const auto action=value?value->shape.action():GpuIndirectEvidence::unknown;
    if(m_inline)m_inline->indirect(reinterpret_cast<ID3D12GraphicsCommandList*>(command),action,count);
    uint64_t generation{},epoch{};
    {
        std::lock_guard lock{m_mutex};if(!m_target) return;
        if(!s_writer_forwarding) {
            ++m_snapshot_status.indirect_actions[action];m_snapshot_status.signature=value;
            m_snapshot_status.signature_interface=reinterpret_cast<uintptr_t>(signature);m_snapshot_status.indirect_command=command;m_snapshot_status.indirect_max_count=count;
            ++m_snapshot_status.unsupported_writer_calls[1];m_snapshot_status.unsupported_writer_kind=GpuStateEvidence::indirect;
            m_snapshot_status.unsupported_writer_callsite=callsite;m_snapshot_status.unsupported_writer_thread=GetCurrentThreadId();
        } else ++m_snapshot_status.nested_writer_calls[1];
        const auto it=m_recordings.find(command);const bool tracked=it!=m_recordings.end() && it->second.epoch==m_epoch && !it->second.closed;
        const auto use=m_access.indirect(action,tracked && it->second.binding_known,tracked && it->second.bound,tracked && it->second.binding_covered,count);
        if(use==GpuTargetAccess::compute_excluded || use==GpuTargetAccess::raster_excluded) {
            if(!s_writer_forwarding) {++m_snapshot_status.excluded_writer_calls[1];++m_snapshot_status.indirect_exclusions[use==GpuTargetAccess::raster_excluded];}
            return;
        }
        if(use==GpuTargetAccess::refuse) {
            if(!s_writer_forwarding) {if(!tracked || !it->second.binding_known) ++m_snapshot_status.indirect_binding_unknown;if(!tracked || !it->second.binding_covered) ++m_snapshot_status.indirect_binding_uncovered;}
            if(tracked) it->second.state.unmodeled_writer(GpuStateEvidence::indirect);
            else {++m_snapshot_status.untracked_hazards;m_epoch_barrier_hazards|=GpuStateEvidence::unsupported_writer;}
            return;
        }
        generation=it->second.generation;epoch=m_epoch;
    }
    std::optional<Origin> origin;try {if(m_origin) origin=m_origin();} catch(...) {}
    std::lock_guard lock{m_mutex};const auto it=m_recordings.find(command);
    if(epoch==m_epoch && it!=m_recordings.end() && it->second.generation==generation && !it->second.closed && it->second.bound && it->second.binding_known && it->second.binding_covered) {
        observe(it->second,Kind::indirect_possible,callsite,origin);++m_snapshot_status.indirect_potential;
        // GPU count/arguments may be zero. Do not infer an actual raster write
        // or RENDER_TARGET state from a CPU ExecuteIndirect recording.
    } else if(m_target) {
        if(it!=m_recordings.end()) it->second.state.unmodeled_writer(GpuStateEvidence::indirect);
        else {++m_snapshot_status.untracked_hazards;m_epoch_barrier_hazards|=GpuStateEvidence::unsupported_writer;}
    }
}
void GpuCompletionProbe::unmodeled_writer(uintptr_t command,GpuStateEvidence::Writer kind,uintptr_t callsite) {
    if(s_internal) return;
    if(m_inline && kind==GpuStateEvidence::bundle)m_inline->hazard(reinterpret_cast<ID3D12GraphicsCommandList*>(command),GpuStateEvidence::unsupported_writer);
    std::lock_guard lock{m_mutex};if(!m_target) return;
    auto& counts=s_writer_forwarding?m_snapshot_status.nested_writer_calls:m_snapshot_status.unsupported_writer_calls;
    for(size_t i=0;i<counts.size();++i) if(uint32_t(kind)==(1u<<i)) ++counts[i];
    if(!s_writer_forwarding) {
        m_snapshot_status.unsupported_writer_kind=kind;m_snapshot_status.unsupported_writer_callsite=callsite;
        m_snapshot_status.unsupported_writer_thread=GetCurrentThreadId();
    }
    // Nested observations still invalidate their actual recording (or epoch
    // if untracked). Only counters/origin classification are deduplicated.
    const auto it=m_recordings.find(command);
    const bool tracked=it!=m_recordings.end() && it->second.epoch==m_epoch && !it->second.closed;
    // Private committed non-UAV backing cannot be written by compute or UAV
    // clears, regardless of descriptor bindings. Indirect may draw; bundles
    // may change bindings. Neither inherits this exclusion.
    if((kind==GpuStateEvidence::dispatch || kind==GpuStateEvidence::clear_uav_uint || kind==GpuStateEvidence::clear_uav_float) && m_access.exclude_compute()) {
        if(!s_writer_forwarding) for(size_t i=0;i<counts.size();++i) if(uint32_t(kind)==(1u<<i)) ++m_snapshot_status.excluded_writer_calls[i];
        return;
    }
    if(tracked) it->second.state.unmodeled_writer(kind);
    else {++m_snapshot_status.untracked_hazards;m_epoch_barrier_hazards|=GpuStateEvidence::epoch_hazards(true,false,GpuStateEvidence::unsupported_writer);}
}
void GpuCompletionProbe::poll() {
    for(auto& w:m_work) if(w.busy) {
        const auto completed=w.fence->GetCompletedValue();
        if(completed==UINT64_MAX || completed<w.sample.serial) continue;
        w.busy=false;w.sample.completed=std::chrono::steady_clock::now();
        w.sample.completion_upper_ms=std::chrono::duration<double,std::milli>(w.sample.completed-w.sample.submitted).count();
        void* pointer{};D3D12_RANGE range{0,16};
        if(SUCCEEDED(w.buffer->Map(0,&range,&pointer))) {
            std::memcpy(&w.sample.gpu_begin,pointer,8);std::memcpy(&w.sample.gpu_end,static_cast<char*>(pointer)+8,8);
            D3D12_RANGE empty{};w.buffer->Unmap(0,&empty);
            w.sample.gpu_ms=w.sample.frequency && w.sample.gpu_end>=w.sample.gpu_begin?double(w.sample.gpu_end-w.sample.gpu_begin)*1000/double(w.sample.frequency):-1;
            if(w.epoch==m_epoch && w.sample.gpu_ms>=0 && (!m_latest || m_latest->serial<w.sample.serial)) m_latest=w.sample;
        }
        w.resource.Reset();
    }
}
std::optional<GpuCompletionProbe::Sample> GpuCompletionProbe::latest() {std::lock_guard lock{m_mutex};poll();return m_latest;}
HRESULT GpuCompletionProbe::readback(uint64_t expected_serial,std::span<const GpuTextureReadback::Source> sources,
                                   std::vector<GpuTextureReadback::Pixels>& pixels,Readback& evidence) {
    pixels.clear();evidence={};
    std::lock_guard lock{m_mutex};poll();
    if(!expected_serial || !m_target || sources.empty() || sources.size()>2 ||
       std::ranges::count(sources,m_target.Get(),&GpuTextureReadback::Source::texture)!=1) return E_INVALIDARG;
    if(!m_latest || m_latest->serial!=expected_serial || m_latest->resource!=reinterpret_cast<uintptr_t>(m_target.Get()) ||
       m_latest->submission_sequence!=m_submission) return HRESULT_FROM_WIN32(ERROR_RETRY);
    return capture_locked(*m_latest,sources,pixels,evidence);
}
bool GpuCompletionProbe::request_snapshot(uint64_t tag) {
    std::lock_guard lock{m_mutex};if(!tag || !m_target) return false;
    if(m_snapshot && m_snapshot->tag==tag) return true;
    m_snapshot.reset();m_snapshot_request=tag;return true;
}
std::optional<GpuCompletionProbe::Snapshot> GpuCompletionProbe::take_snapshot(uint64_t tag) {
    std::lock_guard lock{m_mutex};if(!m_snapshot || m_snapshot->tag!=tag) return std::nullopt;
    auto result=std::move(m_snapshot);m_snapshot.reset();return result;
}
HRESULT GpuCompletionProbe::capture_locked(const Sample& writer,std::span<const GpuTextureReadback::Source> sources,
                                         std::vector<GpuTextureReadback::Pixels>& pixels,Readback& evidence) {
    if(GpuQueueProbe::get().errors() || GpuClosedListProbe::get().errors() || RenderTargetProbe::get().status().errors) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const auto device_receipt=GpuQueueProbe::read_device(m_device.Get());const auto queue_receipt=GpuQueueProbe::read(m_queue.Get());
    if(!device_receipt || !queue_receipt || queue_receipt->device!=device_receipt->identity ||
       !GpuQueueProbe::all_observed_methods_match(*device_receipt,queue_receipt->execute)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if(writer.submission_sequence!=m_submission) return HRESULT_FROM_WIN32(ERROR_RETRY);
    if(writer.barrier_hazards || !writer.all_bindings_covered || !GpuStateEvidence::coverage(m_barrier_capabilities_known,m_enhanced_supported,m_enhanced_hook_installed) ||
       !GpuStateEvidence::base_writer_coverage(m_base_writer_hook_mask))
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    Readback receipt;receipt.writer_serial=writer.serial;receipt.writer_submission=writer.submission_sequence;
    receipt.resource=reinterpret_cast<uintptr_t>(m_target.Get());
    for(const auto& source:sources) if(source.texture==m_target.Get()) receipt.source_state=source.state;
    receipt.raster_state=m_state_raster;receipt.common_decay=m_state_decay;
    receipt.barrier_hazards=writer.barrier_hazards;receipt.barrier_capabilities_known=m_barrier_capabilities_known;
    receipt.enhanced_supported=m_enhanced_supported;receipt.enhanced_hook_installed=m_enhanced_hook_installed;
    receipt.base_writer_hook_mask=m_base_writer_hook_mask;
    receipt.allocation=GpuAllocationProbe::read(m_target.Get());
    receipt.excluded_aliases=m_snapshot_status.excluded_aliases;receipt.excluded_uavs=m_snapshot_status.excluded_uavs;receipt.excluded_writers=m_snapshot_status.excluded_writer_calls;
    receipt.binding_capabilities_known=m_binding_capabilities_known;receipt.list4_supported=m_list4_supported;receipt.binding_hook_mask=m_binding_hook_mask;
    receipt.signature_hook_installed=(m_allocation.status().hook_mask&32)!=0;receipt.indirect_exclusions=m_snapshot_status.indirect_exclusions;
    receipt.extended=m_extended_writers.status();receipt.extended_exclusions=m_snapshot_status.extended_exclusions;
    if(!receipt.extended.known) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if((receipt.excluded_aliases || receipt.excluded_uavs || std::ranges::any_of(receipt.excluded_writers,[](auto n){return n!=0;}) ||
        std::ranges::any_of(receipt.extended_exclusions,[](auto n){return n!=0;})) &&
       (!receipt.allocation || !receipt.allocation->access().private_render_target())) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    // Preparation, actual copy submission and its bounded fence wait stay under
    // the same lock as all hooked queue submissions and target invalidation.
    // Readback's own Reset/Close/Execute hooks must bypass that lock on this thread.
    struct Scope {
        bool previous{s_internal};Readback* previous_readback{s_readback};std::atomic_bool& active;
        Scope(Readback& value,std::atomic_bool& flag):active(flag) {s_internal=true;s_readback=&value;active=true;}
        ~Scope(){active=false;s_readback=previous_readback;s_internal=previous;}
    } scope{receipt,m_readback_active};
    const auto hr=GpuTextureReadback::capture(m_queue.Get(),sources,pixels);
    if(FAILED(hr)) return hr;
    if(receipt.executions!=1 || receipt.queue!=reinterpret_cast<uintptr_t>(m_queue.Get()) || !receipt.commands ||
       receipt.copy_submission!=receipt.writer_submission+1 || receipt.copy_submission!=m_submission) {pixels.clear();return E_UNEXPECTED;}
    receipt.fence_completed=true;evidence=receipt;return S_OK;
}
bool GpuCompletionProbe::complete_foreign_copy(ID3D12CommandQueue* queue,uint64_t lists) {
    // Called under the same mutex as every observed primary/foreign Execute.
    // Actual queue completion orders all earlier unknown COPY work before any
    // later primary batch. It does not classify its commands or prove a frame.
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if(m_pending_foreign_fences.size()>=16 || m_snapshot_status.foreign_completions.size()>=128 || FAILED(m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) return false;
    const auto started=std::chrono::steady_clock::now();const uint64_t value=1;
    if(FAILED(queue->Signal(fence.Get(),value))) {m_pending_foreign_fences.push_back({fence,nullptr});return false;}
    const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    const bool ready=event && SUCCEEDED(fence->SetEventOnCompletion(value,event));
    const auto waited=ready?WaitForSingleObject(event,2000):WAIT_FAILED;
    const auto completed=fence->GetCompletedValue();
    if(waited!=WAIT_OBJECT_0 || completed==UINT64_MAX || completed<value) {m_pending_foreign_fences.push_back({fence,event});return false;}
    CloseHandle(event);
    ++m_snapshot_status.foreign_fenced_submissions;m_snapshot_status.foreign_fenced_lists+=lists;
    m_snapshot_status.foreign_fenced_queue=reinterpret_cast<uintptr_t>(queue);m_snapshot_status.foreign_fence_value=value;m_snapshot_status.foreign_completed_value=completed;
    m_snapshot_status.foreign_fence_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    m_snapshot_status.foreign_completions.push_back({m_submission,lists,value,completed,GpuObjectIdentity::get(queue),GpuObjectIdentity::get(m_device.Get()),m_snapshot_status.foreign_fence_ms});return true;
}
void GpuCompletionProbe::execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists,uintptr_t callsite) {
    std::unique_lock lock{m_mutex};++m_submission;
    // No observed target (every play session: only the desktop PiP experiment sets one): the evidence
    // below only judges that target. Submit without the lock, which every recording thread's draw,
    // barrier and Reset hook also takes (MOD_REVIEW_2026-10-29).
    if(!m_target) {lock.unlock();s_execute(queue,count,lists);return;}
    if(queue!=m_queue.Get()) {
        ++m_snapshot_status.foreign_submissions;m_target_state.reset();uint32_t hazards{};bool target_write{};uint64_t unresolved{};
        const auto foreign_queue=GpuQueueProbe::read(queue);const auto observed_device=GpuQueueProbe::read_device(m_device.Get());
        bool fence_eligible=foreign_queue && observed_device && foreign_queue->desc.Type==D3D12_COMMAND_LIST_TYPE_COPY && foreign_queue->device==observed_device->identity &&
            GpuQueueProbe::all_observed_methods_match(*observed_device,foreign_queue->execute) && !GpuClosedListProbe::get().errors() && m_access.private_render_target();
        if(!GpuQueueProbe::read(queue)) ++m_snapshot_status.unknown_queue_submissions;
        if(lists && count<=1024) for(UINT i=0;i<count;++i) {
            const auto found=m_recordings.find(reinterpret_cast<uintptr_t>(lists[i]));
            uint32_t reason{};
            if(found!=m_recordings.end() && found->second.epoch==m_epoch) {
                hazards|=found->second.state.hazards;
                target_write|=found->second.writes!=0;
                if(!found->second.closed) reason|=4;
                if(!recording_matches(found->first,found->second)) reason|=8;
            } else reason=found==m_recordings.end()?1:2;
            if(reason) {
                SnapshotStatus::ForeignList sample;sample.ordinal=++m_snapshot_status.foreign_unknown_lists;
                sample.queue=reinterpret_cast<uintptr_t>(queue);sample.commands=reinterpret_cast<uintptr_t>(lists[i]);
                sample.callsite=callsite;sample.thread=GetCurrentThreadId();sample.reason=reason;
                if(lists[i]) {
                    sample.type=uint32_t(lists[i]->GetType());sample.identity=GpuObjectIdentity::get(lists[i]);
                    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> graphics;
                    if(SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&graphics)))) {
                        sample.graphics=reinterpret_cast<uintptr_t>(graphics.Get());auto** methods=*reinterpret_cast<void***>(graphics.Get());
                        sample.reset=reinterpret_cast<uintptr_t>(methods[10]);sample.close=reinterpret_cast<uintptr_t>(methods[9]);
                        sample.reset_matches=methods[10]==m_command_addresses[0];sample.close_matches=methods[9]==m_command_addresses[1];
                    }
                }
                sample.closure=GpuClosedListProbe::read(lists[i]);sample.birth=GpuClosedListProbe::birth(lists[i]);const auto allocation=GpuAllocationProbe::read(m_target.Get());
                if(lists[i]) {
                    RecordingStamp stamp{};UINT bytes=sizeof(stamp);
                    if(SUCCEEDED(lists[i]->GetPrivateData(RECORDING_KEY,&bytes,&stamp)) && bytes==sizeof(stamp) && stamp.magic==0x534d325245434f52) {
                        sample.recording_stamp_known=true;sample.recording_generation=stamp.generation;sample.recording_epoch=stamp.epoch;sample.recording_identity=stamp.identity;
                    }
                }
                if(allocation) sample.target_creation_begin_ns=allocation->creation_begin_ns;
                const auto queue_receipt=GpuQueueProbe::read(queue);
                sample.precreation_copy=allocation && m_access.private_render_target() && sample.closure && queue_receipt &&
                    sample.reset_matches && sample.close_matches && queue_receipt->device==allocation->range.device &&
                    GpuClosedListProbe::precedes_copy(*sample.closure,allocation->creation_begin_ns,allocation->range.device,queue_receipt->desc.Type) &&
                    !GpuClosedListProbe::get().errors() && !GpuQueueProbe::get().errors();
                if(sample.precreation_copy) ++m_snapshot_status.foreign_precreation_copies;
                else if(m_target) {
                    ++unresolved;
                    fence_eligible=fence_eligible && sample.type==D3D12_COMMAND_LIST_TYPE_COPY && sample.reset_matches && sample.close_matches &&
                        sample.closure && sample.closure->device==foreign_queue->device;
                }
                m_snapshot_status.foreign_list=sample;
            }
        }
        if(target_write) {++m_snapshot_status.foreign_target_submissions;m_epoch_barrier_hazards|=GpuStateEvidence::foreign_target_writer;}
        if(hazards) {++m_snapshot_status.foreign_hazard_submissions;m_epoch_barrier_hazards|=GpuStateEvidence::epoch_hazards(false,true,hazards);}
        if(!lists || count>1024) {if(m_target) m_epoch_barrier_hazards|=GpuStateEvidence::unknown_foreign_list;}
        s_execute(queue,count,lists);
        if(unresolved && (!fence_eligible || !complete_foreign_copy(queue,unresolved))) {
            ++m_snapshot_status.foreign_fence_failures;m_epoch_barrier_hazards|=GpuStateEvidence::unknown_foreign_list;
        }
        return;
    }
    poll();Sample sample;sample.submission_sequence=m_submission;sample.resource=reinterpret_cast<uintptr_t>(m_target.Get());sample.queue=reinterpret_cast<uintptr_t>(queue);sample.lists=count;sample.execute_callsite=callsite;
    if(!lists || count>1024) m_target_state.reset();
    auto status=m_snapshot_status;status.unknown_lists=0;status.transition_lists=0;status.raster_state_lists=0;status.lists=count;status.requested=bool(m_snapshot_request);
    bool ambiguous_state=false;sample.all_lists_tracked=true;
    sample.barrier_hazards=m_epoch_barrier_hazards;
    if(m_target && lists && count<=1024) for(UINT i=0;i<count;++i) {
        auto found=m_recordings.find(reinterpret_cast<uintptr_t>(lists[i]));
        if(found!=m_recordings.end() && found->second.epoch==m_epoch && found->second.closed && recording_matches(found->first,found->second)) {
            sample.all_bindings_covered=sample.all_bindings_covered && found->second.binding_covered;
            const auto& state=found->second.state;sample.barrier_hazards|=state.hazards;
            if(found->second.writes || state.seen) ++sample.target_lists;
            if(state.seen) {
                if(state.legacy_seen) ++status.transition_lists;
                if(state.raster) ++status.raster_state_lists;
                if(state.unknown) {ambiguous_state=true;m_target_state.reset();}
                else {m_target_state=state.end;m_state_raster=state.raster;}
            }
            if(!found->second.writes) continue;
            if(!sample.writes) sample.origin=found->second.origin;
            else if(!found->second.origin || sample.origin!=found->second.origin) sample.origin.reset();
            sample.writes+=found->second.writes;sample.commands=found->first;sample.generation=found->second.generation;
            for(size_t k=0;k<KIND_COUNT;++k) sample.kinds[k]+=found->second.kinds[k];
            sample.last_write_callsite=found->second.last_write_callsite;sample.last_write_thread=found->second.last_write_thread;
        } else {++status.unknown_lists;sample.all_lists_tracked=false;sample.all_bindings_covered=false;m_target_state.reset();}
    }
    // Permit batching only when one known list touches the observed target.
    // Multiple target-touching lists need transition-chain proof of their own.
    if(ambiguous_state || sample.barrier_hazards || !sample.all_lists_tracked || !sample.all_bindings_covered || sample.target_lists>1) m_target_state.reset();
    m_state_decay=false;
    if(m_target_state && m_simultaneous) {m_target_state=D3D12_RESOURCE_STATE_COMMON;m_state_decay=true;}
    if(sample.writes) {status.writes=sample.writes;status.state=m_target_state?uint32_t(*m_target_state):UINT32_MAX;status.barrier_hazards=sample.barrier_hazards;m_snapshot_status=status;}
    auto free=std::ranges::find_if(m_work,[](const Work& w){return !w.busy;});
    if(!sample.writes || free==m_work.end()) {s_execute(queue,count,lists);return;}
    auto& w=*free;sample.serial=++m_serial;sample.submitted=std::chrono::steady_clock::now();
    // D3D12 Reset can call other hooked lifecycle methods. Own marker recording
    // must not re-enter the observer mutex or be attributed to engine work.
    struct Internal {bool previous{s_internal};Internal(){s_internal=true;}~Internal(){s_internal=previous;}} internal;
    if(FAILED(queue->GetTimestampFrequency(&sample.frequency))) {s_execute(queue,count,lists);return;}
    if(FAILED(w.allocator->Reset())) {s_execute(queue,count,lists);return;}
    bool ready=SUCCEEDED(s_reset(w.markers[0].Get(),w.allocator.Get(),nullptr));
    if(ready) {
        w.markers[0]->EndQuery(w.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        ready=SUCCEEDED(s_close(w.markers[0].Get()));
    }
    if(ready) ready=SUCCEEDED(s_reset(w.markers[1].Get(),w.allocator.Get(),nullptr));
    if(ready) {
        w.markers[1]->EndQuery(w.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        w.markers[1]->ResolveQueryData(w.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,w.buffer.Get(),0);
        ready=SUCCEEDED(s_close(w.markers[1].Get()));
    }
    if(!ready) {s_execute(queue,count,lists);return;}
    w.sample=sample;w.epoch=m_epoch;w.resource=m_target;
    ID3D12CommandList* start[]{w.markers[0].Get()};s_execute(queue,1,start);
    s_execute(queue,count,lists);
    ID3D12CommandList* end[]{w.markers[1].Get()};s_execute(queue,1,end);w.busy=true;
    if(FAILED(queue->Signal(w.fence.Get(),sample.serial))) w.sample.serial=UINT64_MAX; // Keep submitted work leased on failure.
    if(m_snapshot_request && m_target_state && w.sample.serial!=UINT64_MAX) {
        Snapshot snapshot;snapshot.tag=*m_snapshot_request;snapshot.writer=sample;
        const std::array<GpuTextureReadback::Source,1> sources{{{m_target.Get(),*m_target_state}}};
        snapshot.result=capture_locked(sample,sources,snapshot.pixels,snapshot.boundary);
        poll();
        if(SUCCEEDED(snapshot.result)) {
            if(m_latest && m_latest->serial==sample.serial) snapshot.writer=*m_latest;
            else {snapshot.result=E_UNEXPECTED;snapshot.pixels.clear();snapshot.boundary={};}
        }
        m_snapshot=std::move(snapshot);m_snapshot_request.reset();
    }
}
HRESULT STDMETHODCALLTYPE GpuCompletionProbe::on_reset(ID3D12GraphicsCommandList* c,ID3D12CommandAllocator* a,ID3D12PipelineState* p) {GpuClosedListProbe::get().invalidate(c);const auto hr=s_reset(c,a,p);if(SUCCEEDED(hr) && s_probe && !s_internal) {if(s_probe->m_sdk)s_probe->m_sdk->reset(c);s_probe->reset(reinterpret_cast<uintptr_t>(c));}return hr;}
HRESULT STDMETHODCALLTYPE GpuCompletionProbe::on_close(ID3D12GraphicsCommandList* c) {const auto hr=s_close(c);if(s_probe && !s_internal) {std::lock_guard lock{s_probe->m_mutex};auto it=s_probe->m_recordings.find(reinterpret_cast<uintptr_t>(c));if(it!=s_probe->m_recordings.end()) it->second.closed=SUCCEEDED(hr);}return hr;}
void STDMETHODCALLTYPE GpuCompletionProbe::on_draw(ID3D12GraphicsCommandList* c,UINT a,UINT b,UINT d,UINT e) {const bool classify=!s_internal && !s_writer_forwarding && !GpuApiForwardScope::active();if(classify && s_probe && a && b)s_probe->draw(reinterpret_cast<uintptr_t>(c),Kind::draw,reinterpret_cast<uintptr_t>(_ReturnAddress()));{GpuApiForwardScope forwarding;s_draw(c,a,b,d,e);}if(classify && s_probe && a && b && s_probe->m_inline)s_probe->m_inline->raster(c);}
void STDMETHODCALLTYPE GpuCompletionProbe::on_indexed(ID3D12GraphicsCommandList* c,UINT a,UINT b,UINT d,INT e,UINT f) {const bool classify=!s_internal && !s_writer_forwarding && !GpuApiForwardScope::active();if(classify && s_probe && a && b)s_probe->draw(reinterpret_cast<uintptr_t>(c),Kind::indexed,reinterpret_cast<uintptr_t>(_ReturnAddress()));{GpuApiForwardScope forwarding;s_indexed(c,a,b,d,e,f);}if(classify && s_probe && a && b && s_probe->m_inline)s_probe->m_inline->raster(c);}
void STDMETHODCALLTYPE GpuCompletionProbe::on_execute(ID3D12CommandQueue* q,UINT n,ID3D12CommandList*const* lists) {
    GpuQueueTimeline::get().executed(q,n); // Profiling diagnostic; a no-op unless installed.
    if(s_internal) {
        if(s_probe && s_readback) {
            ++s_readback->executions;s_readback->copy_submission=++s_probe->m_submission;
            s_readback->queue=reinterpret_cast<uintptr_t>(q);s_readback->commands=n==1 && lists?reinterpret_cast<uintptr_t>(lists[0]):0;
        }
        s_execute(q,n,lists);return;
    }
    if(s_probe) {s_probe->execute(q,n,lists,reinterpret_cast<uintptr_t>(_ReturnAddress()));if(s_probe->m_inline)s_probe->m_inline->submitted(q,n,lists);if(s_probe->m_sdk)s_probe->m_sdk->submitted(q,n,lists);}else s_execute(q,n,lists);
}
void STDMETHODCALLTYPE GpuCompletionProbe::on_clear(ID3D12GraphicsCommandList* c,D3D12_CPU_DESCRIPTOR_HANDLE handle,const FLOAT* colour,UINT count,const D3D12_RECT* rects) {
    const bool classify=!s_internal && !s_writer_forwarding && !GpuApiForwardScope::active();
    if(s_probe && classify && s_probe->m_resolve) {
        s_probe->write(reinterpret_cast<uintptr_t>(c),s_probe->m_resolve(handle.ptr),Kind::clear,reinterpret_cast<uintptr_t>(_ReturnAddress()));
    }
    {GpuApiForwardScope forwarding;s_clear(c,handle,colour,count,rects);}
    if(s_probe && classify && s_probe->m_inline && s_probe->m_resolve){const auto resource=s_probe->m_resolve(handle.ptr);if(resource)s_probe->m_inline->raster(c,resource);else s_probe->m_inline->hazard(c,GpuStateEvidence::implicit_binding);}
}
void STDMETHODCALLTYPE GpuCompletionProbe::on_copy(ID3D12GraphicsCommandList* c,ID3D12Resource* target,ID3D12Resource* source) {if(s_probe && !s_internal && !GpuApiForwardScope::active()) s_probe->write(reinterpret_cast<uintptr_t>(c),GpuAllocationProbe::identity(target),Kind::copy,reinterpret_cast<uintptr_t>(_ReturnAddress()));GpuApiForwardScope forwarding;s_copy(c,target,source);}
void STDMETHODCALLTYPE GpuCompletionProbe::on_copy_texture(ID3D12GraphicsCommandList* c,const D3D12_TEXTURE_COPY_LOCATION* target,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION* source,const D3D12_BOX* box) {if(s_probe && target && !s_internal && !GpuApiForwardScope::active()) s_probe->write(reinterpret_cast<uintptr_t>(c),GpuAllocationProbe::identity(target->pResource),Kind::copy_texture,reinterpret_cast<uintptr_t>(_ReturnAddress()));GpuApiForwardScope forwarding;s_copy_texture(c,target,x,y,z,source,box);}
void STDMETHODCALLTYPE GpuCompletionProbe::on_resolve(ID3D12GraphicsCommandList* c,ID3D12Resource* target,UINT target_sub,ID3D12Resource* source,UINT source_sub,DXGI_FORMAT format) {if(s_probe && !s_internal && !GpuApiForwardScope::active()) s_probe->write(reinterpret_cast<uintptr_t>(c),GpuAllocationProbe::identity(target),Kind::resolve,reinterpret_cast<uintptr_t>(_ReturnAddress()));GpuApiForwardScope forwarding;s_resolve(c,target,target_sub,source,source_sub,format);}
void STDMETHODCALLTYPE GpuCompletionProbe::on_barriers(ID3D12GraphicsCommandList* c,UINT count,const D3D12_RESOURCE_BARRIER* values) {if(s_probe && s_probe->m_sdk && !s_internal)internal([&]{s_probe->m_sdk->barriers(c,count,values);});if(s_probe) s_probe->barriers(reinterpret_cast<uintptr_t>(c),count,values,reinterpret_cast<uintptr_t>(_ReturnAddress()));s_barriers(c,count,values);}
void STDMETHODCALLTYPE GpuCompletionProbe::on_enhanced(ID3D12GraphicsCommandList7* c,UINT count,const D3D12_BARRIER_GROUP* groups) {
    if(s_probe && !s_internal) {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> base;
        // COM need not return the same address for an extended interface.
        const auto key=SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&base)))?reinterpret_cast<uintptr_t>(base.Get()):uintptr_t{};
        if(count && s_probe->m_inline)s_probe->m_inline->hazard(base.Get(),GpuStateEvidence::enhanced);
        s_probe->enhanced_barriers(key,count,groups);
    }
    s_enhanced(c,count,groups);
}
GpuInlineCopy::Sample GpuCompletionProbe::finish_inline(uint64_t tag) {
    struct Guard{bool previous{s_internal};Guard(){s_internal=true;}~Guard(){s_internal=previous;}} guard;
    if(m_inline)return m_inline->finish(tag);GpuInlineCopy::Sample result;result.tag=tag;result.refusal=GpuInlineCopy::Refusal::configuration;return result;
}
std::optional<uint64_t> GpuCompletionProbe::open_recording(ID3D12GraphicsCommandList* command){
    if(!command)return std::nullopt;std::lock_guard lock{m_mutex};const auto found=m_recordings.find(reinterpret_cast<uintptr_t>(command));
    if(found==m_recordings.end() || found->second.epoch!=m_epoch || found->second.closed || !found->second.binding_covered || !recording_matches(found->first,found->second))return std::nullopt;return found->second.generation;
}
void GpuCompletionProbe::internal(const std::function<void()>& call){struct Guard{bool previous{s_internal};Guard(){s_internal=true;}~Guard(){s_internal=previous;}} guard;call();}
void STDMETHODCALLTYPE GpuCompletionProbe::on_dispatch(ID3D12GraphicsCommandList* c,UINT x,UINT y,UINT z) {
    if(s_probe) s_probe->unmodeled_writer(reinterpret_cast<uintptr_t>(c),GpuStateEvidence::dispatch,reinterpret_cast<uintptr_t>(_ReturnAddress()));
    GpuWriterForwardScope forwarding{s_writer_forwarding};
    GpuApiForwardScope api;
    s_dispatch(c,x,y,z);
}
void STDMETHODCALLTYPE GpuCompletionProbe::on_indirect(ID3D12GraphicsCommandList* c,ID3D12CommandSignature* signature,UINT count,ID3D12Resource* arguments,UINT64 offset,ID3D12Resource* counter,UINT64 counter_offset) {
    if(s_probe) s_probe->indirect(reinterpret_cast<uintptr_t>(c),signature,count,reinterpret_cast<uintptr_t>(_ReturnAddress()));
    GpuWriterForwardScope forwarding{s_writer_forwarding};
    GpuApiForwardScope api;
    s_indirect(c,signature,count,arguments,offset,counter,counter_offset);
}
void STDMETHODCALLTYPE GpuCompletionProbe::on_clear_state(ID3D12GraphicsCommandList* command,ID3D12PipelineState* pso) {
    s_clear_state(command,pso);if(s_probe) s_probe->binding_state(reinterpret_cast<uintptr_t>(command),0);
}
void STDMETHODCALLTYPE GpuCompletionProbe::on_begin_pass(ID3D12GraphicsCommandList4* command,UINT count,const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,D3D12_RENDER_PASS_FLAGS flags) {
    if(s_probe && !s_internal) {Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> base;const auto key=SUCCEEDED(command->QueryInterface(IID_PPV_ARGS(&base)))?reinterpret_cast<uintptr_t>(base.Get()):0;s_probe->binding_state(key,1);}
    s_begin_pass(command,count,targets,depth,flags);
}
void STDMETHODCALLTYPE GpuCompletionProbe::on_end_pass(ID3D12GraphicsCommandList4* command) {
    if(s_probe && !s_internal) {Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> base;const auto key=SUCCEEDED(command->QueryInterface(IID_PPV_ARGS(&base)))?reinterpret_cast<uintptr_t>(base.Get()):0;s_probe->binding_state(key,2);}
    s_end_pass(command);
}
void STDMETHODCALLTYPE GpuCompletionProbe::on_bundle(ID3D12GraphicsCommandList* c,ID3D12GraphicsCommandList* bundle) {
    if(s_probe) s_probe->unmodeled_writer(reinterpret_cast<uintptr_t>(c),GpuStateEvidence::bundle,reinterpret_cast<uintptr_t>(_ReturnAddress()));
    GpuWriterForwardScope forwarding{s_writer_forwarding};
    GpuApiForwardScope api;
    s_bundle(c,bundle);
}
void STDMETHODCALLTYPE GpuCompletionProbe::on_uav_uint(ID3D12GraphicsCommandList* c,D3D12_GPU_DESCRIPTOR_HANDLE gpu,D3D12_CPU_DESCRIPTOR_HANDLE cpu,ID3D12Resource* resource,const UINT* values,UINT count,const D3D12_RECT* rects) {
    if(s_probe) s_probe->unmodeled_writer(reinterpret_cast<uintptr_t>(c),GpuStateEvidence::clear_uav_uint,reinterpret_cast<uintptr_t>(_ReturnAddress()));
    GpuWriterForwardScope forwarding{s_writer_forwarding};
    GpuApiForwardScope api;
    s_uav_uint(c,gpu,cpu,resource,values,count,rects);
}
void STDMETHODCALLTYPE GpuCompletionProbe::on_uav_float(ID3D12GraphicsCommandList* c,D3D12_GPU_DESCRIPTOR_HANDLE gpu,D3D12_CPU_DESCRIPTOR_HANDLE cpu,ID3D12Resource* resource,const FLOAT* values,UINT count,const D3D12_RECT* rects) {
    if(s_probe) s_probe->unmodeled_writer(reinterpret_cast<uintptr_t>(c),GpuStateEvidence::clear_uav_float,reinterpret_cast<uintptr_t>(_ReturnAddress()));
    GpuWriterForwardScope forwarding{s_writer_forwarding};
    GpuApiForwardScope api;
    s_uav_float(c,gpu,cpu,resource,values,count,rects);
}
