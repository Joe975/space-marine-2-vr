#include "GpuAllocationProbe.h"
#include <MinHook.h>
#include <intrin.h>
#include <vector>
#include "GpuObjectIdentity.h"
#include "GpuClosedListProbe.h"

uintptr_t GpuAllocationProbe::identity(IUnknown* object) {
    return GpuObjectIdentity::get(object);
}
bool GpuAllocationProbe::install(ID3D12Device* device) {
    if(s_probe || !device || !(m_identity=identity(device))) return false;
    if(!GpuSignatureProbe::get().observe_device(device)) return false;
    m_device=device;const auto initialized=MH_Initialize();
    if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return false;
    auto** table=*reinterpret_cast<void***>(device);
    std::vector<void*> addresses{table[27],table[29],table[28]},hooks{reinterpret_cast<void*>(&on_committed),reinterpret_cast<void*>(&on_placed),reinterpret_cast<void*>(&on_heap)};
    Microsoft::WRL::ComPtr<ID3D12Device4> extended;const bool has_extended=SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&extended)));
    if(has_extended) {auto** ext=*reinterpret_cast<void***>(extended.Get());addresses.insert(addresses.end(),{ext[53],ext[54]});hooks.insert(hooks.end(),{reinterpret_cast<void*>(&on_committed1),reinterpret_cast<void*>(&on_heap1)});}
    std::vector<void*> originals(addresses.size());size_t made{};
    for(;made<addresses.size();++made) if(MH_CreateHook(addresses[made],hooks[made],&originals[made])!=MH_OK) break;
    if(made!=addresses.size()) {for(size_t i=0;i<made;++i) MH_RemoveHook(addresses[i]);return false;}
    s_committed=reinterpret_cast<Committed>(originals[0]);s_placed=reinterpret_cast<Placed>(originals[1]);
    s_heap=reinterpret_cast<Heap>(originals[2]);
    if(has_extended) {s_committed1=reinterpret_cast<Committed1>(originals[3]);s_heap1=reinterpret_cast<Heap1>(originals[4]);}s_probe=this;
    for(auto address:addresses) if(MH_EnableHook(address)!=MH_OK) {
        for(auto a:addresses) MH_DisableHook(a);s_probe=nullptr;for(auto a:addresses) MH_RemoveHook(a);return false;
    }
    m_mask=(has_extended?31:11)|32;return true;
}
std::optional<GpuAllocationProbe::SignatureRecord> GpuAllocationProbe::read_signature(ID3D12CommandSignature* signature) {
    return GpuSignatureProbe::read(signature);
}
void GpuAllocationProbe::annotate_heap(ID3D12Device* device,void** output,uint32_t api) {
    if(identity(device)!=m_identity || !output || !*output) return;
    ++m_heap_calls[api];Microsoft::WRL::ComPtr<ID3D12Heap> heap;
    if(FAILED(reinterpret_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&heap)))) {++m_failed;return;}
    const auto desc=heap->GetDesc();HeapStamp value;value.serial=++m_serial;value.device=m_identity;value.heap=identity(heap.Get());value.size=desc.SizeInBytes;value.flags=desc.Flags;
    if(!value.heap || !value.size || FAILED(heap->SetPrivateData(KEY,sizeof(value),&value))) ++m_failed;
}
void GpuAllocationProbe::annotate(ID3D12Device* device,ID3D12Heap* heap,uint64_t offset,D3D12_HEAP_FLAGS flags,D3D12_RESOURCE_STATES initial,void** output,uint32_t api,uintptr_t callsite,uint64_t began) {
    if(identity(device)!=m_identity || !output || !*output) return;
    ++m_calls[api];Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if(FAILED(reinterpret_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&resource)))) {++m_failed;return;}
    Record value;value.api=api;value.initial_state=initial;value.callsite=callsite;value.creation_begin_ns=began;value.thread=GetCurrentThreadId();value.desc=resource->GetDesc();
    value.properties_known=SUCCEEDED(resource->GetHeapProperties(&value.properties,&value.actual_heap_flags));
    auto& range=value.range;range.serial=++m_serial;range.device=m_identity;range.resource=identity(resource.Get());
    const auto info=device->GetResourceAllocationInfo(0,1,&value.desc);range.size=info.SizeInBytes;range.alignment=info.Alignment;
    if(heap) {
        const auto desc=heap->GetDesc();Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if(FAILED(heap->GetDevice(IID_PPV_ARGS(&owner))) || identity(owner.Get())!=m_identity) {++m_failed;return;}
        range.kind=GpuAllocationEvidence::placed;range.heap=identity(heap);range.offset=offset;range.heap_size=desc.SizeInBytes;range.heap_flags=desc.Flags;
        HeapStamp stamp{};UINT bytes=sizeof(stamp);
        if(SUCCEEDED(heap->GetPrivateData(KEY,&bytes,&stamp)) && bytes==sizeof(stamp) && stamp.magic==0x534d3248 && stamp.version==1 &&
           stamp.serial && stamp.device==m_identity && stamp.heap==range.heap && stamp.size==range.heap_size && stamp.flags==range.heap_flags) range.heap_serial=stamp.serial;
    } else {range.kind=GpuAllocationEvidence::committed;range.heap_size=range.size;range.heap_flags=flags;}
    if(!range.valid() || FAILED(resource->SetPrivateData(KEY,sizeof(value),&value))) {++m_failed;return;}
    ++m_annotated;
}
std::optional<GpuAllocationProbe::Record> GpuAllocationProbe::read(ID3D12Resource* resource) {
    if(!resource) return std::nullopt;
    Record value{};UINT bytes=sizeof(value);
    if(FAILED(resource->GetPrivateData(KEY,&bytes,&value)) || bytes!=sizeof(value) || value.magic!=0x534d3241 || value.version!=2 ||
       value.api>2 || (value.api==1)!=(value.range.kind==GpuAllocationEvidence::placed) || !value.range.valid() || value.range.resource!=identity(resource)) return std::nullopt;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    if(FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || identity(owner.Get())!=value.range.device) return std::nullopt;
    const auto actual=resource->GetDesc();const auto& desc=value.desc;
    if(actual.Dimension!=desc.Dimension || actual.Alignment!=desc.Alignment || actual.Width!=desc.Width || actual.Height!=desc.Height ||
       actual.DepthOrArraySize!=desc.DepthOrArraySize || actual.MipLevels!=desc.MipLevels || actual.Format!=desc.Format ||
       actual.SampleDesc.Count!=desc.SampleDesc.Count || actual.SampleDesc.Quality!=desc.SampleDesc.Quality || actual.Layout!=desc.Layout || actual.Flags!=desc.Flags) return std::nullopt;
    D3D12_HEAP_PROPERTIES properties{};D3D12_HEAP_FLAGS flags{};
    const bool known=SUCCEEDED(resource->GetHeapProperties(&properties,&flags));
    if(known!=value.properties_known || (known && (flags!=value.actual_heap_flags ||
        properties.Type!=value.properties.Type || properties.CPUPageProperty!=value.properties.CPUPageProperty ||
        properties.MemoryPoolPreference!=value.properties.MemoryPoolPreference || properties.CreationNodeMask!=value.properties.CreationNodeMask ||
        properties.VisibleNodeMask!=value.properties.VisibleNodeMask))) return std::nullopt;
    return value;
}
HRESULT STDMETHODCALLTYPE GpuAllocationProbe::on_committed(ID3D12Device* d,const D3D12_HEAP_PROPERTIES* h,D3D12_HEAP_FLAGS f,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    const auto began=GpuClosedListProbe::now();
    const auto hr=s_committed(d,h,f,desc,state,clear,iid,out);
    if(SUCCEEDED(hr) && s_probe) s_probe->annotate(d,nullptr,0,f,state,out,0,reinterpret_cast<uintptr_t>(_ReturnAddress()),began);return hr;
}
HRESULT STDMETHODCALLTYPE GpuAllocationProbe::on_placed(ID3D12Device* d,ID3D12Heap* heap,UINT64 offset,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    const auto began=GpuClosedListProbe::now();
    const auto hr=s_placed(d,heap,offset,desc,state,clear,iid,out);
    if(SUCCEEDED(hr) && s_probe) s_probe->annotate(d,heap,offset,D3D12_HEAP_FLAG_NONE,state,out,1,reinterpret_cast<uintptr_t>(_ReturnAddress()),began);return hr;
}
HRESULT STDMETHODCALLTYPE GpuAllocationProbe::on_committed1(ID3D12Device4* d,const D3D12_HEAP_PROPERTIES* h,D3D12_HEAP_FLAGS f,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,ID3D12ProtectedResourceSession* session,REFIID iid,void** out) {
    const auto began=GpuClosedListProbe::now();
    const auto hr=s_committed1(d,h,f,desc,state,clear,session,iid,out);
    if(SUCCEEDED(hr) && s_probe) s_probe->annotate(d,nullptr,0,f,state,out,2,reinterpret_cast<uintptr_t>(_ReturnAddress()),began);return hr;
}
HRESULT STDMETHODCALLTYPE GpuAllocationProbe::on_heap(ID3D12Device* d,const D3D12_HEAP_DESC* desc,REFIID iid,void** out) {
    const auto hr=s_heap(d,desc,iid,out);if(SUCCEEDED(hr) && s_probe) s_probe->annotate_heap(d,out,0);return hr;
}
HRESULT STDMETHODCALLTYPE GpuAllocationProbe::on_heap1(ID3D12Device4* d,const D3D12_HEAP_DESC* desc,ID3D12ProtectedResourceSession* session,REFIID iid,void** out) {
    const auto hr=s_heap1(d,desc,session,iid,out);if(SUCCEEDED(hr) && s_probe) s_probe->annotate_heap(d,out,1);return hr;
}
