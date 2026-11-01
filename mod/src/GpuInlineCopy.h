#pragma once
#include "GpuStateEvidence.h"
#include "GpuObjectIdentity.h"
#include "GpuTargetAccess.h"
#include "GpuAllocationProbe.h"
#include "InlineCopyAllocation.h"
#include "GpuImageHeapPool.h"
#include <wrl/client.h>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include <ostream>

// Every headset launch's eye copy (pair retention; began as a bounded desktop control). Appends a retained copy to the actual native open
// recording, then signals only after that exact recording is submitted. The
// caller obtains source leases from GetBuffer, never from descriptor-cache POD.
class GpuInlineCopy {
public:
    static constexpr size_t CAPACITY=4;
    enum class Refusal:uint32_t {none,configuration,busy,no_writer,multiple_recordings,recording_changed,coverage,state,hazard,allocation,thread,submission,signal};
    struct Access {
        uint64_t aliases{},alias_null{},alias_target{},alias_other{};
        uintptr_t alias_before{},alias_after{};
        std::optional<GpuAllocationProbe::Record> before_allocation,after_allocation;
        std::array<uint64_t,GpuTargetAccess::extended_count> extended_calls{},extended_blocked{};
        uint32_t first_extended{};uintptr_t extended_resource{},extended_callsite{};
        std::optional<GpuAllocationProbe::Record> extended_allocation;
    };
    struct Sample {
        uint64_t tag{},generation{},writes{},submission{},fence_value{};
        uintptr_t source{},identity{},commands{},queue{};
        uint32_t width{},height{},format{},source_flags{},state{},hazards{},thread{};
        uint32_t dimension{},array{},mips{},samples{},quality{},layout{};uint64_t alignment{};
        uint32_t heap_flags{},heap_type{},heap_cpu{},heap_pool{},creation_node{},visible_node{};
        uintptr_t destination_heap{};bool heap_reused{};
        uint64_t excluded_aliases{},excluded_uavs{};bool heap_known{},private_display{};
        Access access;
        Refusal refusal{};uint32_t changed_site{};bool changed_elsewhere{},appended{},submitted{},completed{};
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    };
    bool configure(ID3D12Device*,ID3D12CommandQueue*,std::span<ID3D12Resource* const>);
    // reset() drops the configuration (device, queue, sources): a configured copy is one that can begin.
    bool configured(){std::lock_guard lock{m_mutex};return m_device!=nullptr;}
    bool begin(uint64_t tag,ID3D12Resource* source);
    void invalidate_active(Refusal);
    // Called under the hook coordinator's internal-command guard.
    Sample finish(uint64_t tag);
    std::vector<Sample> samples();
    // Drop only the producer's completed copy lease. Consumer-held ComPtrs keep
    // the immutable destination alive; new captures allocate distinct storage.
    bool retire(uint64_t tag);
    bool reset(); // Refuses to release any appended copy without GPU completion.
    ~GpuInlineCopy();
    static void write_sample(std::ostream&,const Sample&);
    static bool display_backing(const D3D12_RESOURCE_DESC&,const D3D12_HEAP_PROPERTIES&,D3D12_HEAP_FLAGS);
    void recording(ID3D12GraphicsCommandList*,uint64_t generation,bool covered);
    void closed(ID3D12GraphicsCommandList*,bool);
    void bound(ID3D12GraphicsCommandList*,std::span<const uintptr_t>,bool);
    void raster(ID3D12GraphicsCommandList*,uintptr_t explicit_target=0);
    void barriers(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
    void hazard(ID3D12GraphicsCommandList*,uint32_t);
    void indirect(ID3D12GraphicsCommandList*,uint32_t action,UINT count);
    void extended(ID3D12GraphicsCommandList*,GpuTargetAccess::Extended,uintptr_t resource,uintptr_t callsite,std::optional<GpuAllocationProbe::Record>);
    void submitted(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
    void recording_capacity(size_t value){std::lock_guard lock{m_mutex};m_recording_capacity=value;} // Tests: a small table.
private:
    using Resource=Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Source {D3D12_RESOURCE_DESC desc{};D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};bool known{},private_display{};};
    struct State {GpuStateEvidence evidence;uint64_t writes{},excluded_aliases{},excluded_uavs{};Access access;};
    struct Recording {uint64_t generation{};bool covered{},closed{},binding_known{};std::vector<uintptr_t> bound;std::map<uintptr_t,State> states;};
    struct Slot {
        Sample sample;Resource source;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    };
    struct Active {uint64_t tag{};Resource source;Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command;uintptr_t identity{};uint32_t thread{};std::map<uintptr_t,uint64_t> writers;Refusal refusal{};uint32_t changed_site{};bool changed_elsewhere{};};
    // Refusal::recording_changed, with the first place that saw it (Sample::changed_site, 29 Oct):
    // 1 the eye's writer list began a new recording, 2 the recording table overflowed, 3-7 an unknown
    // command list (never seen beginning a recording) drew, set barriers, hazarded, ran indirect or
    // extended work, 8-11 at finish: the writer's recording unknown, not the leased list, closed, or
    // of another generation.
    // A command list never seen beginning a recording, used on another thread than the active eye's:
    // the game's other render threads (29 Oct: barriers on such a list refused the left eye at
    // startup and stereo stopped). It cannot touch the eye's source: that is a committed,
    // render-target-only texture of the mod's (never aliased, never a UAV) whose RTV only the eye's
    // own writer list is redirected to. So such calls refuse the eye only when they name the source.
    bool foreign()const{return m_active && GetCurrentThreadId()!=m_active->thread;}
    bool names(UINT count,const D3D12_RESOURCE_BARRIER* values)const{
        if(!m_active || !values || count>4096)return true;
        for(UINT i=0;i<count;++i){const auto& v=values[i];
            if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && GpuObjectIdentity::get(v.Transition.pResource)==m_active->identity)return true;
            if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING && (GpuObjectIdentity::get(v.Aliasing.pResourceBefore)==m_active->identity || GpuObjectIdentity::get(v.Aliasing.pResourceAfter)==m_active->identity))return true;
            if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV && v.UAV.pResource && GpuObjectIdentity::get(v.UAV.pResource)==m_active->identity)return true;}
        return false;
    }
    void changed(uint32_t site){if(!m_active)return;if(m_active->refusal==Refusal::none){m_active->changed_site=site;m_active->changed_elsewhere=GetCurrentThreadId()!=m_active->thread;}m_active->refusal=Refusal::recording_changed;}
    void poll();
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    std::map<uintptr_t,Source> m_sources;std::map<uintptr_t,Recording> m_recordings;size_t m_recording_capacity{2048};
    std::optional<Active> m_active;std::vector<Slot> m_slots;
    uint64_t m_submission{};std::mutex m_mutex;
    InlineCopyAllocation m_allocation;
    GpuImageHeapPool m_image_heaps;
};
