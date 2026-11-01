#pragma once
#include <d3d12.h>
#include <cstdint>
#include <functional>

// Native/debug implementations may forward one API through another hooked
// implementation. Deduplicate only writer classification, never queue/barrier
// observation; the outer unsupported writer has already tainted its recording.
struct GpuWriterForwardScope {
    bool& active;bool previous;
    explicit GpuWriterForwardScope(bool& value):active(value),previous(value) {active=true;}
    ~GpuWriterForwardScope() {active=previous;}
    GpuWriterForwardScope(const GpuWriterForwardScope&)=delete;
    GpuWriterForwardScope& operator=(const GpuWriterForwardScope&)=delete;
};

// CPU-only policy shared by the actual command hooks and deterministic tests.
// Unsupported accesses taint a recording until Reset; draws/full transitions
// must not silently turn an alias/layout/content uncertainty into copy evidence.
struct GpuStateEvidence {
    enum Hazard:uint32_t {aliasing=1,enhanced=2,malformed=4,uav=8,unsupported_writer=16,implicit_binding=32,extended_writer=64,untracked_target_writer=128,foreign_target_writer=256,unknown_foreign_list=512};
    enum Writer:uint32_t {dispatch=1,indirect=2,bundle=4,clear_uav_uint=8,clear_uav_float=16};
    static constexpr uint32_t BASE_WRITER_HOOKS=31;
    bool seen{},unknown{},legacy_seen{},raster{};
    D3D12_RESOURCE_STATES end{};
    uint32_t hazards{},writer_kinds{};
    void invalidate(uint32_t reason) {seen=unknown=true;raster=false;hazards|=reason;}
    static bool base_writer_coverage(uint32_t installed) {return installed==BASE_WRITER_HOOKS;}
    void unmodeled_writer(Writer kind) {writer_kinds|=kind;invalidate(Hazard::unsupported_writer);}
    void raster_write(bool single_subresource) {
        if(single_subresource && !unknown && !hazards) {seen=true;end=D3D12_RESOURCE_STATE_RENDER_TARGET;raster=true;}
    }
    struct Counts {uint64_t transitions{},aliases{},uavs{},malformed{},excluded_aliases{},excluded_uavs{};};
    static Counts legacy(GpuStateEvidence* state,ID3D12Resource* target,bool single,UINT count,const D3D12_RESOURCE_BARRIER* values,
                         const std::function<bool(const D3D12_RESOURCE_BARRIER&)>& exclude={},
                         const std::function<bool(ID3D12Resource*)>& matches={}) {
        Counts counts;
        if(count>4096 || (count && !values)) {++counts.malformed;if(state) state->invalidate(Hazard::malformed);return counts;}
        for(UINT i=0;i<count;++i) {
            const auto& value=values[i];
            if(value.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                ++counts.aliases;
                if(value.Flags==D3D12_RESOURCE_BARRIER_FLAG_NONE && exclude && exclude(value)) ++counts.excluded_aliases;
                else if(state) state->invalidate(Hazard::aliasing);
            } else if(value.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV) {
                if(!value.UAV.pResource || (matches?matches(value.UAV.pResource):value.UAV.pResource==target)) {
                    ++counts.uavs;
                    if(value.Flags==D3D12_RESOURCE_BARRIER_FLAG_NONE && exclude && exclude(value)) ++counts.excluded_uavs;
                    else if(state) state->invalidate(Hazard::uav);
                }
            } else if(value.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                if(!value.Transition.pResource) {++counts.malformed;if(state) state->invalidate(Hazard::malformed);continue;}
                if(!(matches?matches(value.Transition.pResource):value.Transition.pResource==target)) continue;
                ++counts.transitions;
                if(state) {
                    state->seen=state->legacy_seen=true;state->raster=false;
                    state->unknown=state->hazards || value.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE ||
                        (value.Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
                         (value.Transition.Subresource!=0 || !single));
                    state->end=value.Transition.StateAfter;
                }
            } else {++counts.malformed;if(state) state->invalidate(Hazard::malformed);}
        }
        return counts;
    }
    static void enhanced_barriers(GpuStateEvidence* state,UINT count,const D3D12_BARRIER_GROUP* groups) {
        if(!count) return;
        // Layout/access/sync interop is not yet modeled. Even unrelated named
        // groups can activate aliased memory. Observe the call and refuse.
        if(state) state->invalidate(Hazard::enhanced | (count>4096 || !groups?Hazard::malformed:0));
    }
    static bool coverage(bool capabilities_known,bool enhanced_supported,bool hook_installed) {
        return capabilities_known && (!enhanced_supported || hook_installed);
    }
    static uint32_t epoch_hazards(bool observed_queue,bool tracked,uint32_t hazards) {
        return !observed_queue || !tracked?hazards:0;
    }
};
