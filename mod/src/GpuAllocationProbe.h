#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <optional>
#include "GpuAllocationEvidence.h"
#include "GpuTargetAccess.h"
#include "GpuIndirectEvidence.h"
#include "GpuSignatureProbe.h"

// Allocation evidence for the inline copy and pair retention in every headset launch (began as a
// desktop diagnostic). POD metadata belongs to the real resource's private-data
// store, so no resource-address cache or extra resource/heap leases accumulate.
class GpuAllocationProbe {
public:
    struct Record {
        uint32_t magic{0x534d3241},version{2},api{},initial_state{},thread{};
        uint64_t callsite{};GpuAllocationEvidence range;D3D12_RESOURCE_DESC desc{};
        uint64_t creation_begin_ns{};
        D3D12_HEAP_PROPERTIES properties{};D3D12_HEAP_FLAGS actual_heap_flags{};bool properties_known{};
        GpuTargetAccess access() const {return {range,desc,properties,uint32_t(actual_heap_flags),properties_known};}
    };
    using SignatureRecord=GpuSignatureProbe::Record;
    struct Status {uint32_t hook_mask{};std::array<uint64_t,3> calls{};std::array<uint64_t,2> heap_calls{};uint64_t annotated{},failed{},signature_calls{},signatures_annotated{};};
    bool install(ID3D12Device*);
    // Reuse an existing process-lifetime observer on the same live device.
    // This grants no resource lease; callers still need actual owned resources.
    static bool observes(ID3D12Device* device) {return s_probe && device && s_probe->m_identity==identity(device) && (s_probe->m_mask&3)==3;}
    static std::optional<Record> read(ID3D12Resource*);
    static uintptr_t identity(IUnknown*);
    static std::optional<SignatureRecord> read_signature(ID3D12CommandSignature*);
    Status status() const {const auto signatures=GpuSignatureProbe::get().status();return {m_mask,{m_calls[0].load(),m_calls[1].load(),m_calls[2].load()},{m_heap_calls[0].load(),m_heap_calls[1].load()},m_annotated.load(),m_failed.load()+signatures.failed,signatures.signature_calls,signatures.annotated};}
    static Status observed_status(ID3D12Device* device){return observes(device)?s_probe->status():Status{};}
    // Unique application-defined key; contents are plain bytes, never COM pointers.
    inline static constexpr GUID KEY{0x82a4fe72,0x8397,0x4f4a,{0xa6,0x10,0xb9,0x24,0x0c,0x31,0xe8,0x55}};
    inline static constexpr GUID SIGNATURE_KEY=GpuSignatureProbe::KEY;
private:
    struct HeapStamp {uint32_t magic{0x534d3248},version{1};uint64_t serial{},device{},heap{},size{};uint32_t flags{};};
    void annotate_heap(ID3D12Device*,void**,uint32_t);
    void annotate(ID3D12Device*,ID3D12Heap*,uint64_t,D3D12_HEAP_FLAGS,D3D12_RESOURCE_STATES,void**,uint32_t,uintptr_t,uint64_t);
    using Committed=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    using Placed=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Heap*,UINT64,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    using Committed1=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,ID3D12ProtectedResourceSession*,REFIID,void**);
    using Heap=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_DESC*,REFIID,void**);
    using Heap1=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*,const D3D12_HEAP_DESC*,ID3D12ProtectedResourceSession*,REFIID,void**);
    static HRESULT STDMETHODCALLTYPE on_committed(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    static HRESULT STDMETHODCALLTYPE on_placed(ID3D12Device*,ID3D12Heap*,UINT64,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    static HRESULT STDMETHODCALLTYPE on_committed1(ID3D12Device4*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,ID3D12ProtectedResourceSession*,REFIID,void**);
    static HRESULT STDMETHODCALLTYPE on_heap(ID3D12Device*,const D3D12_HEAP_DESC*,REFIID,void**);
    static HRESULT STDMETHODCALLTYPE on_heap1(ID3D12Device4*,const D3D12_HEAP_DESC*,ID3D12ProtectedResourceSession*,REFIID,void**);
    inline static GpuAllocationProbe* s_probe{};
    inline static Committed s_committed{};inline static Placed s_placed{};inline static Committed1 s_committed1{};
    inline static Heap s_heap{};inline static Heap1 s_heap1{};
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;uintptr_t m_identity{};uint32_t m_mask{};
    std::array<std::atomic<uint64_t>,3> m_calls{};std::atomic<uint64_t> m_serial{},m_annotated{},m_failed{};
    std::array<std::atomic<uint64_t>,2> m_heap_calls{};
};
