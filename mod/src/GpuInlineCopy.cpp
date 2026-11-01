#include "GpuInlineCopy.h"
#include "GpuIndirectEvidence.h"
#include "PerformanceTrace.h"
#include <algorithm>
#include <thread>

namespace {
constexpr GUID key{0xe0bc09a8,0x66a0,0x4e2b,{0x92,0x2d,0x13,0x80,0x11,0x30,0x41,0x93}};
struct Stamp {uint64_t generation{};uintptr_t identity{};};
bool matches(ID3D12CommandList* command,uint64_t generation) {
    Stamp stamp{};UINT bytes=sizeof(stamp);return command && SUCCEEDED(command->GetPrivateData(key,&bytes,&stamp)) &&
        bytes==sizeof(stamp) && stamp.generation==generation && stamp.identity && stamp.identity==GpuObjectIdentity::get(command);
}
bool source_desc(const D3D12_RESOURCE_DESC& d) {
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width && d.Width<=4096 && d.Height && d.Height<=2160 &&
        d.MipLevels==1 && d.DepthOrArraySize==1 && d.SampleDesc.Count==1 &&
        (d.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) && !(d.Flags&(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) &&
        (d.Format==DXGI_FORMAT_R8G8B8A8_UNORM || d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format==DXGI_FORMAT_B8G8R8A8_UNORM || d.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
}
}
bool GpuInlineCopy::display_backing(const D3D12_RESOURCE_DESC& desc,const D3D12_HEAP_PROPERTIES& heap,D3D12_HEAP_FLAGS flags) {
    // ALLOW_DISPLAY can only be used with CreateCommittedResource (Microsoft
    // D3D12_HEAP_FLAGS contract). Reject sharing and every unmodelled heap flag.
    return source_desc(desc) && desc.Flags==D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET && (desc.Alignment==0 || desc.Alignment==D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) && desc.SampleDesc.Quality==0 && desc.Layout==D3D12_TEXTURE_LAYOUT_UNKNOWN &&
        (flags&D3D12_HEAP_FLAG_ALLOW_DISPLAY) && !(uint32_t(flags)&~0x8cu) && heap.Type==D3D12_HEAP_TYPE_DEFAULT &&
        heap.CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_UNKNOWN && heap.MemoryPoolPreference==D3D12_MEMORY_POOL_UNKNOWN && heap.CreationNodeMask==1 && heap.VisibleNodeMask==1;
}
bool GpuInlineCopy::configure(ID3D12Device* device,ID3D12CommandQueue* queue,std::span<ID3D12Resource* const> sources) {
    std::lock_guard lock{m_mutex};if(m_device || !device || device->GetNodeCount()!=1 || !queue || sources.empty() || sources.size()>8 || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;if(FAILED(queue->GetDevice(IID_PPV_ARGS(&owner))) || GpuObjectIdentity::get(owner.Get())!=GpuObjectIdentity::get(device)) return false;
    std::map<uintptr_t,Source> identities;
    for(auto* resource:sources) {
        if(!resource || !source_desc(resource->GetDesc()) || FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || GpuObjectIdentity::get(owner.Get())!=GpuObjectIdentity::get(device)) return false;
        const auto identity=GpuObjectIdentity::get(resource);if(!identity || identities.contains(identity)) return false;
        Source source;source.desc=resource->GetDesc();source.known=SUCCEEDED(resource->GetHeapProperties(&source.heap,&source.flags));source.private_display=source.known && display_backing(source.desc,source.heap,source.flags);identities.emplace(identity,source);
    }
    wchar_t control[32768]{};const auto length=GetEnvironmentVariableW(L"SM2VR_INLINE_COPY_ALLOCATION_CONTROL",control,32768);
    if(length>=32768 || !m_allocation.control(length?std::filesystem::path{control}:std::filesystem::path{}))return false;
    m_device=device;m_queue=queue;m_sources=std::move(identities);m_slots.reserve(CAPACITY);return true;
}
void GpuInlineCopy::recording(ID3D12GraphicsCommandList* command,uint64_t generation,bool covered) {
    if(!command) return;std::lock_guard lock{m_mutex};const auto id=reinterpret_cast<uintptr_t>(command);
    if(m_active && m_active->writers.contains(id))changed(1);
    for(auto& slot:m_slots) if(slot.commands.Get()==command && !slot.sample.submitted) slot.sample.refusal=Refusal::recording_changed;
    Recording record;record.generation=generation;record.covered=covered && command->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT;
    Stamp stamp{generation,GpuObjectIdentity::get(command)};if(!generation || !stamp.identity || FAILED(command->SetPrivateData(key,sizeof(stamp),&stamp))) record.covered=false;
    for(const auto& [identity,source]:m_sources) record.states.emplace(identity,State{});
    // Lists are never forgotten when the game destroys them. A full table first drops closed lists that
    // no eye or copy refers to (a closed list records again only after a Reset, which re-adds it); only
    // a table full of open lists is cleared, which refuses the active eye.
    if(m_recordings.size()>=m_recording_capacity && !m_recordings.contains(id)) {
        std::erase_if(m_recordings,[&](const auto& entry){
            if(!entry.second.closed || (m_active && m_active->writers.contains(entry.first)))return false;
            return std::none_of(m_slots.begin(),m_slots.end(),[&](const Slot& slot){return reinterpret_cast<uintptr_t>(slot.commands.Get())==entry.first;});});
        if(m_recordings.size()>=m_recording_capacity) {m_recordings.clear();changed(2);}
    }
    m_recordings[id]=std::move(record);
}
void GpuInlineCopy::closed(ID3D12GraphicsCommandList* command,bool ok) {std::lock_guard lock{m_mutex};auto it=m_recordings.find(reinterpret_cast<uintptr_t>(command));if(it!=m_recordings.end()){it->second.closed=ok;if(!ok)it->second.covered=false;}}
void GpuInlineCopy::bound(ID3D12GraphicsCommandList* command,std::span<const uintptr_t> targets,bool known) {
    std::lock_guard lock{m_mutex};auto it=m_recordings.find(reinterpret_cast<uintptr_t>(command));if(it==m_recordings.end())return;
    it->second.binding_known=known;it->second.bound.assign(targets.begin(),targets.end());
}
void GpuInlineCopy::raster(ID3D12GraphicsCommandList* command,uintptr_t explicit_target) {
    std::lock_guard lock{m_mutex};const auto id=reinterpret_cast<uintptr_t>(command);auto it=m_recordings.find(id);if(it==m_recordings.end()){if(!foreign())changed(3);return;}
    auto& record=it->second;
    if(!explicit_target && !record.binding_known) {for(auto& [target,state]:record.states)state.evidence.invalidate(GpuStateEvidence::implicit_binding);return;}
    for(auto& [target,state]:record.states) if(explicit_target?explicit_target==target:std::ranges::find(record.bound,target)!=record.bound.end()) {
        state.evidence.raster_write(true);++state.writes;
        if(m_active && target==m_active->identity) {
            if(GetCurrentThreadId()!=m_active->thread)m_active->refusal=Refusal::thread;
            if(record.closed || !record.covered || !matches(command,record.generation))m_active->refusal=Refusal::coverage;
            ++m_active->writers[id];
            // Lease only the actual live native callback argument.
            if(!m_active->command)m_active->command=command;
        }
    }
}
void GpuInlineCopy::barriers(ID3D12GraphicsCommandList* command,UINT count,const D3D12_RESOURCE_BARRIER* values) {
    std::lock_guard lock{m_mutex};auto it=m_recordings.find(reinterpret_cast<uintptr_t>(command));if(it==m_recordings.end()){if(!foreign() || names(count,values))changed(4);return;}
    for(auto& [identity,state]:it->second.states){const auto source=m_sources.find(identity);
        const auto counts=GpuStateEvidence::legacy(&state.evidence,nullptr,true,count,values,[&](const D3D12_RESOURCE_BARRIER& value){
            if(source==m_sources.end() || !source->second.private_display)return false;
            if(value.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){const auto before=GpuObjectIdentity::get(value.Aliasing.pResourceBefore),after=GpuObjectIdentity::get(value.Aliasing.pResourceAfter);
                return (!value.Aliasing.pResourceBefore || before) && (!value.Aliasing.pResourceAfter || after) && before!=identity && after!=identity;}
            return value.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV && !value.UAV.pResource;
        },[identity](ID3D12Resource* value){return value && GpuObjectIdentity::get(value)==identity;});state.excluded_aliases+=counts.excluded_aliases;state.excluded_uavs+=counts.excluded_uavs;
        if(values && count<=4096)for(UINT i=0;i<count;++i)if(values[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){const auto& alias=values[i].Aliasing;auto& access=state.access;
            const auto before=GpuObjectIdentity::get(alias.pResourceBefore),after=GpuObjectIdentity::get(alias.pResourceAfter);
            if(!access.aliases){access.alias_before=before;access.alias_after=after;access.before_allocation=GpuAllocationProbe::read(alias.pResourceBefore);access.after_allocation=GpuAllocationProbe::read(alias.pResourceAfter);}
            ++access.aliases;if(before==identity || after==identity)++access.alias_target;else if(!alias.pResourceBefore && !alias.pResourceAfter)++access.alias_null;else ++access.alias_other;
        }
    }
}
void GpuInlineCopy::hazard(ID3D12GraphicsCommandList* command,uint32_t reason) {
    std::lock_guard lock{m_mutex};auto it=m_recordings.find(reinterpret_cast<uintptr_t>(command));if(it==m_recordings.end()){if(!foreign())changed(5);return;}
    for(auto& [identity,state]:it->second.states)state.evidence.invalidate(reason);
}
void GpuInlineCopy::indirect(ID3D12GraphicsCommandList* command,uint32_t action,UINT count) {
    if(!count || action==GpuIndirectEvidence::dispatch || action==GpuIndirectEvidence::rays)return; // Configured sources have no UAV flag; aliases remain sticky refusals.
    std::lock_guard lock{m_mutex};auto it=m_recordings.find(reinterpret_cast<uintptr_t>(command));if(it==m_recordings.end()){if(!foreign())changed(6);return;}
    for(auto& [identity,state]:it->second.states) {
        const bool raster=action==GpuIndirectEvidence::draw || action==GpuIndirectEvidence::indexed || action==GpuIndirectEvidence::mesh;
        if(raster && it->second.binding_known && std::ranges::find(it->second.bound,identity)==it->second.bound.end())continue;
        state.evidence.invalidate(GpuStateEvidence::unsupported_writer);
    }
}
void GpuInlineCopy::extended(ID3D12GraphicsCommandList* command,GpuTargetAccess::Extended kind,uintptr_t resource,uintptr_t callsite,std::optional<GpuAllocationProbe::Record> allocation) {
    if(kind==GpuTargetAccess::depth || kind==GpuTargetAccess::dispatch_rays)return;
    std::lock_guard lock{m_mutex};auto it=m_recordings.find(reinterpret_cast<uintptr_t>(command));if(it==m_recordings.end()){if(!foreign() || (m_active && resource==m_active->identity))changed(7);return;}
    for(auto& [identity,state]:it->second.states){const auto source=m_sources.find(identity);const auto& record=it->second;auto& access=state.access;++access.extended_calls[kind];
        if(source!=m_sources.end() && source->second.private_display && resource && resource!=identity && (kind==GpuTargetAccess::discard || kind==GpuTargetAccess::resolve_region || kind==GpuTargetAccess::copy_tiles))continue;
        if(kind==GpuTargetAccess::dispatch_mesh && record.covered && record.binding_known && std::ranges::find(record.bound,identity)==record.bound.end())continue;
        if(!std::ranges::any_of(access.extended_blocked,[](auto count){return count!=0;})){access.first_extended=kind;access.extended_resource=resource;access.extended_callsite=callsite;access.extended_allocation=allocation;}
        ++access.extended_blocked[kind];state.evidence.invalidate(GpuStateEvidence::extended_writer);
    }
}
bool GpuInlineCopy::begin(uint64_t tag,ID3D12Resource* source) {
    std::lock_guard lock{m_mutex};if(!m_device || !tag || !source || m_active || m_slots.size()>=CAPACITY)return false;
    const auto identity=GpuObjectIdentity::get(source);if(!m_sources.contains(identity) || !source_desc(source->GetDesc()))return false;
    for(const auto& slot:m_slots)if(slot.sample.tag==tag)return false;
    Active active;active.tag=tag;active.source=source;active.identity=identity;active.thread=GetCurrentThreadId();m_active=std::move(active);return true;
}
void GpuInlineCopy::invalidate_active(Refusal reason){std::lock_guard lock{m_mutex};if(m_active)m_active->refusal=reason;}
GpuInlineCopy::Sample GpuInlineCopy::finish(uint64_t tag) {
    PerformanceTrace::Scope timing{"inline_copy_finish_cpu"};
    std::lock_guard lock{m_mutex};Sample sample;sample.tag=tag;if(!m_active || m_active->tag!=tag){sample.refusal=Refusal::configuration;return sample;}
    auto active=std::move(*m_active);m_active.reset();Slot slot;slot.source=active.source;sample.source=reinterpret_cast<uintptr_t>(active.source.Get());sample.identity=active.identity;sample.thread=active.thread;
    const auto desc=active.source->GetDesc();sample.width=uint32_t(desc.Width);sample.height=desc.Height;sample.format=desc.Format;sample.source_flags=desc.Flags;
    sample.dimension=desc.Dimension;sample.array=desc.DepthOrArraySize;sample.mips=desc.MipLevels;sample.samples=desc.SampleDesc.Count;sample.quality=desc.SampleDesc.Quality;sample.layout=desc.Layout;sample.alignment=desc.Alignment;
    const auto backing=m_sources.find(active.identity);if(backing!=m_sources.end()){const auto& b=backing->second;sample.heap_known=b.known;sample.private_display=b.private_display;sample.heap_flags=b.flags;sample.heap_type=b.heap.Type;sample.heap_cpu=b.heap.CPUPageProperty;sample.heap_pool=b.heap.MemoryPoolPreference;sample.creation_node=b.heap.CreationNodeMask;sample.visible_node=b.heap.VisibleNodeMask;}
    auto refuse=[&](Refusal reason){sample.refusal=reason;slot.sample=sample;m_slots.push_back(std::move(slot));return sample;};
    sample.changed_site=active.changed_site;sample.changed_elsewhere=active.changed_elsewhere;
    if(active.refusal!=Refusal::none)return refuse(active.refusal);
    if(GetCurrentThreadId()!=active.thread)return refuse(Refusal::thread);
    if(active.writers.empty())return refuse(Refusal::no_writer);
    if(active.writers.size()!=1)return refuse(Refusal::multiple_recordings);
    const auto command=active.writers.begin()->first;auto it=m_recordings.find(command);slot.commands=active.command;
    if(it==m_recordings.end() || !slot.commands || reinterpret_cast<uintptr_t>(slot.commands.Get())!=command || it->second.closed || !matches(slot.commands.Get(),it->second.generation)){sample.changed_site=it==m_recordings.end()?8:!slot.commands || reinterpret_cast<uintptr_t>(slot.commands.Get())!=command?9:it->second.closed?10:11;return refuse(Refusal::recording_changed);}
    auto& record=it->second;auto& state=record.states[active.identity];sample.commands=command;sample.generation=record.generation;sample.writes=active.writers.begin()->second;
    sample.state=uint32_t(state.evidence.end);sample.hazards=state.evidence.hazards;sample.excluded_aliases=state.excluded_aliases;sample.excluded_uavs=state.excluded_uavs;sample.access=state.access;
    if(!record.covered)return refuse(Refusal::coverage);
    if(state.evidence.unknown || state.evidence.hazards)return refuse(Refusal::hazard);
    if(!state.evidence.seen || !state.evidence.legacy_seen || (state.evidence.end!=D3D12_RESOURCE_STATE_RENDER_TARGET && state.evidence.end!=D3D12_RESOURCE_STATE_COMMON))return refuse(Refusal::state);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    auto destination=desc;destination.Flags=D3D12_RESOURCE_FLAG_NONE;
    {
        PerformanceTrace::Scope allocation{"inline_copy_allocate_cpu"};
        HRESULT result=E_PENDING;
        if(m_allocation.mode()==InlineCopyAllocation::Mode::pooled) {
            GpuImageHeapPool::Allocation image;result=m_image_heaps.allocate(m_device.Get(),destination,image);
            if(SUCCEEDED(result)) {sample.texture=std::move(image.texture);sample.destination_heap=image.heap;sample.heap_reused=image.reused;}
        }
        if(FAILED(result))result=m_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&destination,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&sample.texture));
        allocation.value=sample.destination_heap?1:0;
        if(FAILED(result) ||
           FAILED(m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&slot.fence))))return refuse(Refusal::allocation);
    }
    if(sample.destination_heap) {
        // Activate every new placed resource using the D3D12 simple aliasing
        // model. Only a full CopyResource follows, with the normal completion
        // fence and immutable consumer lifetime unchanged.
        D3D12_RESOURCE_BARRIER activate{};activate.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        activate.Aliasing={nullptr,sample.texture.Get()};slot.commands->ResourceBarrier(1,&activate);
    }
    D3D12_RESOURCE_BARRIER before{};before.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    before.Transition={active.source.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state.evidence.end,D3D12_RESOURCE_STATE_COPY_SOURCE};
    slot.commands->ResourceBarrier(1,&before);slot.commands->CopyResource(sample.texture.Get(),active.source.Get());
    std::swap(before.Transition.StateBefore,before.Transition.StateAfter);slot.commands->ResourceBarrier(1,&before);
    before.Transition={sample.texture.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE};slot.commands->ResourceBarrier(1,&before);
    sample.appended=true;sample.fence_value=1;sample.fence=slot.fence;slot.sample=sample;m_slots.push_back(std::move(slot));
    if(PerformanceTrace::get().probes())PerformanceTrace::get().record("inline_copy_appended",PerformanceTrace::now_ms(),0,double(tag));
    return sample;
}
void GpuInlineCopy::submitted(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
    std::lock_guard lock{m_mutex};++m_submission;
    if(m_active) {
        // Other queues may submit unrelated work while the producer is open.
        // Only submission of this scope's writer can invalidate its capture.
        if(!lists || count>1024)m_active->refusal=Refusal::submission;
        else if(m_active->command)for(UINT i=0;i<count;++i)
            if(GpuObjectIdentity::get(lists[i])==GpuObjectIdentity::get(m_active->command.Get()))
                m_active->refusal=Refusal::submission; // Writer submitted before its copy was appended.
    }
    for(auto& slot:m_slots) if(slot.sample.appended && !slot.sample.submitted && slot.sample.refusal==Refusal::none) {
        if(!lists || count>1024) {slot.sample.refusal=Refusal::submission;continue;}
        size_t found{};for(UINT i=0;i<count;++i)if(GpuObjectIdentity::get(lists[i])==GpuObjectIdentity::get(slot.commands.Get()))++found;
        if(!found)continue;
        auto record=m_recordings.find(reinterpret_cast<uintptr_t>(slot.commands.Get()));
        if(queue!=m_queue.Get() || found!=1 || record==m_recordings.end() || !record->second.closed || !matches(slot.commands.Get(),slot.sample.generation)) {slot.sample.refusal=Refusal::submission;continue;}
        slot.sample.queue=reinterpret_cast<uintptr_t>(queue);slot.sample.submission=m_submission;
        if(FAILED(queue->Signal(slot.fence.Get(),slot.sample.fence_value)))slot.sample.refusal=Refusal::signal;
        else {slot.sample.submitted=true;if(PerformanceTrace::get().probes())PerformanceTrace::get().record("inline_copy_submitted",PerformanceTrace::now_ms(),0,double(slot.sample.tag));}
    }
    poll();
}
void GpuInlineCopy::poll(){for(auto& slot:m_slots)if(slot.sample.submitted && !slot.sample.completed){const auto value=slot.fence->GetCompletedValue();if(value!=UINT64_MAX && value>=slot.sample.fence_value){slot.sample.completed=true;
    if(PerformanceTrace::get().probes())PerformanceTrace::get().record("inline_copy_completion_observed",PerformanceTrace::now_ms(),0,double(slot.sample.tag));
    slot.source.Reset();slot.commands.Reset();}}}
std::vector<GpuInlineCopy::Sample> GpuInlineCopy::samples(){std::lock_guard lock{m_mutex};poll();std::vector<Sample> result;for(const auto& slot:m_slots)result.push_back(slot.sample);return result;}
bool GpuInlineCopy::retire(uint64_t tag){
    std::lock_guard lock{m_mutex};poll();if(!tag || m_active)return false;
    const auto slot=std::ranges::find_if(m_slots,[&](const auto& value){return value.sample.tag==tag;});
    if(slot==m_slots.end() || (slot->sample.appended && !slot->sample.completed))return false;
    m_slots.erase(slot);return true;
}
bool GpuInlineCopy::reset(){std::lock_guard lock{m_mutex};poll();for(const auto& slot:m_slots)if(slot.sample.appended && !slot.sample.completed)return false;if(m_active)return false;m_slots.clear();m_recordings.clear();m_sources.clear();m_image_heaps.reset();m_queue.Reset();m_device.Reset();return true;}
GpuInlineCopy::~GpuInlineCopy(){if(!reset()){struct Retired{decltype(m_slots) slots;decltype(m_active) active;decltype(m_device) device;decltype(m_queue) queue;};new Retired{std::move(m_slots),std::move(m_active),std::move(m_device),std::move(m_queue)};}}
void GpuInlineCopy::write_sample(std::ostream& out,const Sample& s){out<<"{\"tag\":"<<s.tag<<",\"generation\":"<<s.generation<<",\"writes\":"<<s.writes<<",\"submission\":"<<s.submission<<",\"fence_value\":"<<s.fence_value
    <<",\"source\":"<<s.source<<",\"identity\":"<<s.identity<<",\"commands\":"<<s.commands<<",\"queue\":"<<s.queue<<",\"width\":"<<s.width<<",\"height\":"<<s.height<<",\"format\":"<<s.format
    <<",\"source_flags\":"<<s.source_flags<<",\"source_state\":"<<s.state<<",\"hazards\":"<<s.hazards<<",\"thread\":"<<s.thread<<",\"refusal\":"<<uint32_t(s.refusal)
    <<",\"image_identity\":"<<GpuObjectIdentity::get(s.texture.Get())
    <<",\"dimension\":"<<s.dimension<<",\"array\":"<<s.array<<",\"mips\":"<<s.mips<<",\"samples\":"<<s.samples<<",\"quality\":"<<s.quality<<",\"layout\":"<<s.layout<<",\"alignment\":"<<s.alignment
    <<",\"heap_known\":"<<(s.heap_known?"true":"false")<<",\"private_display\":"<<(s.private_display?"true":"false")<<",\"heap_flags\":"<<s.heap_flags<<",\"heap_type\":"<<s.heap_type<<",\"heap_cpu\":"<<s.heap_cpu<<",\"heap_pool\":"<<s.heap_pool<<",\"creation_node\":"<<s.creation_node<<",\"visible_node\":"<<s.visible_node<<",\"excluded_aliases\":"<<s.excluded_aliases<<",\"excluded_uavs\":"<<s.excluded_uavs
    <<",\"destination_heap\":"<<s.destination_heap<<",\"heap_reused\":"<<(s.heap_reused?"true":"false")
    <<",\"appended\":"<<(s.appended?"true":"false")<<",\"submitted\":"<<(s.submitted?"true":"false")<<",\"completed\":"<<(s.completed?"true":"false");
    const auto allocation=[&](const std::optional<GpuAllocationProbe::Record>& value){if(!value){out<<"null";return;}const auto& a=*value;const auto& r=a.range;
        out<<"{\"kind\":"<<r.kind<<",\"serial\":"<<r.serial<<",\"device\":"<<r.device<<",\"resource\":"<<r.resource<<",\"heap\":"<<r.heap<<",\"offset\":"<<r.offset<<",\"size\":"<<r.size<<",\"heap_size\":"<<r.heap_size<<",\"alignment\":"<<r.alignment<<",\"heap_flags\":"<<r.heap_flags<<",\"heap_serial\":"<<r.heap_serial
            <<",\"properties_known\":"<<(a.properties_known?"true":"false")<<",\"actual_heap_flags\":"<<a.actual_heap_flags<<",\"resource_flags\":"<<a.desc.Flags<<",\"width\":"<<a.desc.Width<<",\"height\":"<<a.desc.Height<<'}';};
    const auto& a=s.access;out<<",\"access\":{\"aliases\":"<<a.aliases<<",\"alias_null\":"<<a.alias_null<<",\"alias_target\":"<<a.alias_target<<",\"alias_other\":"<<a.alias_other<<",\"alias_before\":"<<a.alias_before<<",\"alias_after\":"<<a.alias_after<<",\"before_allocation\":";allocation(a.before_allocation);out<<",\"after_allocation\":";allocation(a.after_allocation);
    out<<",\"first_extended\":"<<a.first_extended<<",\"extended_resource\":"<<a.extended_resource<<",\"extended_callsite\":"<<a.extended_callsite<<",\"extended_allocation\":";allocation(a.extended_allocation);
    out<<",\"extended_calls\":[";for(size_t i=0;i<a.extended_calls.size();++i){if(i)out<<',';out<<a.extended_calls[i];}out<<"],\"extended_blocked\":[";for(size_t i=0;i<a.extended_blocked.size();++i){if(i)out<<',';out<<a.extended_blocked[i];}out<<"]}}";
}
