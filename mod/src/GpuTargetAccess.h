#pragma once
#include <d3d12.h>
#include "GpuAllocationEvidence.h"
#include "GpuIndirectEvidence.h"

// Restricted proof for one observed private committed GPU render target. It
// excludes only access classes that cannot reach this allocation. It proves
// neither frame ownership nor the completeness of command/API observation.
struct GpuTargetAccess {
    GpuAllocationEvidence allocation;
    D3D12_RESOURCE_DESC desc{};
    D3D12_HEAP_PROPERTIES properties{};
    uint32_t actual_heap_flags{};
    bool properties_known{};
    bool private_render_target() const {
        return allocation.valid() && allocation.kind==GpuAllocationEvidence::committed &&
            // The runtime automatically adds DENY_BUFFERS/NON_RT_DS to an
            // implicit RT heap. Only those restrictive bits may differ.
            properties_known && !((allocation.heap_flags|actual_heap_flags)&~0x84u) &&
            (actual_heap_flags&allocation.heap_flags)==allocation.heap_flags &&
            properties.Type==D3D12_HEAP_TYPE_DEFAULT &&
            properties.CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
            properties.MemoryPoolPreference==D3D12_MEMORY_POOL_UNKNOWN &&
            properties.CreationNodeMask==1 && properties.VisibleNodeMask==1 &&
            desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.Layout==D3D12_TEXTURE_LAYOUT_UNKNOWN && desc.SampleDesc.Quality==0 &&
            desc.Flags==D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET &&
            desc.Width && desc.Height && desc.MipLevels==1 && desc.DepthOrArraySize==1 && desc.SampleDesc.Count==1;
    }
    bool exclude_alias(uint64_t before,uint64_t after,bool identities_known) const {
        // NULL names arbitrary aliased resources; private committed backing
        // has no exposed heap to overlap. Explicit target names still refuse.
        return private_render_target() && identities_known &&
            before!=allocation.resource && after!=allocation.resource;
    }
    bool exclude_compute() const {return private_render_target();}
    bool exclude_indirect(bool binding_known,bool target_bound,bool implementation_covered) const {
        return private_render_target() && binding_known && !target_bound && implementation_covered;
    }
    enum IndirectUse {refuse,compute_excluded,raster_excluded,raster_possible};
    IndirectUse indirect(GpuIndirectEvidence::Action action,bool known,bool bound,bool covered,uint32_t max_count) const {
        if(!private_render_target()) return refuse;
        if(action==GpuIndirectEvidence::dispatch || action==GpuIndirectEvidence::rays) return compute_excluded;
        if(action!=GpuIndirectEvidence::draw && action!=GpuIndirectEvidence::indexed && action!=GpuIndirectEvidence::mesh) return refuse;
        if(!known || !covered) return refuse;
        return !bound || !max_count?raster_excluded:raster_possible;
    }
    enum Extended:uint32_t {discard,depth,resolve_region,initialize_meta,execute_meta,dispatch_rays,dispatch_mesh,copy_tiles,extended_count};
    bool exclude_extended(Extended kind,uint64_t resource,bool known,bool bound,bool covered) const {
        if(!private_render_target()) return false;
        switch(kind) {
            case depth:case dispatch_rays:return true;
            case discard:case resolve_region:case copy_tiles:return resource && resource!=allocation.resource;
            case dispatch_mesh:return known && !bound && covered;
            default:return false; // Opaque meta commands may access resource parameters directly.
        }
    }
};
