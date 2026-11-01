#pragma once
#include <cstdint>
#include <limits>

// Observed physical placement only. This says nothing about active alias,
// resource state, queue ordering, shader access or safe retirement.
struct GpuAllocationEvidence {
    enum Kind:uint32_t {unknown,committed,placed};
    Kind kind{};uint32_t heap_flags{};
    uint64_t serial{},device{},resource{},heap{},offset{},size{},heap_size{},alignment{};
    uint64_t heap_serial{}; // Only actual CreateHeap/CreateHeap1 observation establishes private heap origin.
    bool valid() const {
        if(!serial || !device || !resource || !size || size==UINT64_MAX || !alignment ||
           (alignment&(alignment-1)) || offset%alignment || size%alignment || offset>UINT64_MAX-size) return false;
        if(kind==committed) return !heap && !heap_serial && !offset && heap_size==size;
        return kind==placed && heap && offset<=heap_size && size<=heap_size-offset;
    }
    static bool disjoint(const GpuAllocationEvidence& a,const GpuAllocationEvidence& b) {
        // Shared heaps/resources may have separately opened COM identities.
        if(!a.valid() || !b.valid() || a.device!=b.device || a.resource==b.resource || ((a.heap_flags|b.heap_flags)&0x21)) return false;
        if(a.kind==placed && b.kind==placed && a.heap==b.heap)
            return a.offset+a.size<=b.offset || b.offset+b.size<=a.offset;
        // Distinct heap identities alone do not exclude independently opened
        // heaps backed by the same external memory.
        return (a.kind==committed || a.heap_serial) && (b.kind==committed || b.heap_serial);
    }
};
