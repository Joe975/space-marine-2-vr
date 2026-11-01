#include "GpuQueueProbe.h"
#include "GpuObjectIdentity.h"
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <type_traits>

GpuQueueProbe& GpuQueueProbe::get() {static auto* value=new GpuQueueProbe;return *value;}
std::optional<GpuQueueProbe::Device> GpuQueueProbe::read_device(ID3D12Device* device) {
    if(!device) return std::nullopt;Device value;UINT bytes=sizeof(value);
    if(FAILED(device->GetPrivateData(DEVICE_KEY,&bytes,&value)) || bytes!=sizeof(value) || value.magic!=0x534d325144455649 ||
       !value.serial || value.identity!=GpuObjectIdentity::get(device) || value.method_count>16) return std::nullopt;
    return value;
}
bool GpuQueueProbe::observe_device(ID3D12Device* device,bool early_return) {
    if(!device) return false;std::lock_guard lock{m_mutex};
    Device value=read_device(device).value_or(Device{});
    if(!value.serial) {value.serial=++m_serial;value.identity=GpuObjectIdentity::get(device);}
    if(!value.identity) {++m_errors;return false;}value.early_return|=early_return;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) {++m_errors;return false;}
    const std::array<Create,16> hooks{&on_create<0>,&on_create<1>,&on_create<2>,&on_create<3>,&on_create<4>,&on_create<5>,&on_create<6>,&on_create<7>,&on_create<8>,&on_create<9>,&on_create<10>,&on_create<11>,&on_create<12>,&on_create<13>,&on_create<14>,&on_create<15>};
    const std::array<Create1,16> hooks1{&on_create1<0>,&on_create1<1>,&on_create1<2>,&on_create1<3>,&on_create1<4>,&on_create1<5>,&on_create1<6>,&on_create1<7>,&on_create1<8>,&on_create1<9>,&on_create1<10>,&on_create1<11>,&on_create1<12>,&on_create1<13>,&on_create1<14>,&on_create1<15>};
    auto hook=[&](auto& implementations,auto& originals,auto& thunks,void* address) {
        if(implementations.contains(address)) return true;
        if(implementations.size()>=16) return false;
        const auto index=implementations.size();void* original{};
        if(MH_CreateHook(address,reinterpret_cast<void*>(thunks[index]),&original)!=MH_OK) return false;
        originals[index]=reinterpret_cast<std::remove_reference_t<decltype(originals[index])>::value_type>(original);
        implementations.emplace(address,index);
        if(MH_EnableHook(address)==MH_OK) return true;
        implementations.erase(address);MH_RemoveHook(address);return false;
    };
    if(hook(m_base,m_originals,hooks,(*reinterpret_cast<void***>(device))[8])) value.hooks|=1;else ++value.failed;
    Microsoft::WRL::ComPtr<ID3D12Device9> extended;const auto hr=device->QueryInterface(IID_PPV_ARGS(&extended));
    value.device9=SUCCEEDED(hr);
    if(value.device9) {if(hook(m_extended,m_originals1,hooks1,(*reinterpret_cast<void***>(extended.Get()))[75])) value.hooks|=2;else ++value.failed;}
    else if(hr!=E_NOINTERFACE) ++value.failed;
    const bool ok=SUCCEEDED(device->SetPrivateData(DEVICE_KEY,sizeof(value),&value)) && value.hooks==(value.device9?3:1) && !value.failed;
    if(!ok) ++m_errors;return ok;
}
template<size_t I> HRESULT STDMETHODCALLTYPE GpuQueueProbe::on_create(ID3D12Device* device,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID iid,void** output) {
    auto& self=get();const auto original=self.m_originals[I].load();if(!original) return E_UNEXPECTED;
    const auto hr=original(device,desc,iid,output);if(SUCCEEDED(hr)) try {self.annotate(device,desc,output,1,GUID_NULL,reinterpret_cast<uintptr_t>(_ReturnAddress()));} catch(...) {++self.m_errors;} return hr;
}
template<size_t I> HRESULT STDMETHODCALLTYPE GpuQueueProbe::on_create1(ID3D12Device9* device,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID creator,REFIID iid,void** output) {
    auto& self=get();const auto original=self.m_originals1[I].load();if(!original) return E_UNEXPECTED;
    const auto hr=original(device,desc,creator,iid,output);if(SUCCEEDED(hr)) try {self.annotate(device,desc,output,2,creator,reinterpret_cast<uintptr_t>(_ReturnAddress()));} catch(...) {++self.m_errors;} return hr;
}
void GpuQueueProbe::annotate(ID3D12Device* device,const D3D12_COMMAND_QUEUE_DESC* desc,void** output,uint32_t api,const GUID& creator,uintptr_t callsite) {
    if(!output || !*output || !desc) return;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if(FAILED(reinterpret_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&queue)))) {++m_errors;return;}
    std::lock_guard lock{m_mutex};auto owner=read_device(device);if(!owner) {++m_errors;return;}
    ++owner->calls[api==1?0:1];Queue value;value.serial=++m_serial;value.device_serial=owner->serial;value.device=owner->identity;
    value.identity=GpuObjectIdentity::get(queue.Get());value.execute=reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(queue.Get()))[10]);
    value.callsite=callsite;value.api=api;value.thread=GetCurrentThreadId();value.desc=queue->GetDesc();value.creator=creator;value.requested_node=desc->NodeMask;
    // Actual WARP normalizes the default node mask 0 to the first node bit 1.
    // Preserve both values; do not permit any other descriptor difference.
    const bool node_matches=value.desc.NodeMask==desc->NodeMask || (!desc->NodeMask && value.desc.NodeMask==1);
    if(!value.identity || value.desc.Type!=desc->Type || value.desc.Priority!=desc->Priority || value.desc.Flags!=desc->Flags || !node_matches ||
       FAILED(queue->SetPrivateData(QUEUE_KEY,sizeof(value),&value))) ++owner->failed;
    else {
        ++owner->annotated;
        if(std::find(owner->execute_methods.begin(),owner->execute_methods.begin()+owner->method_count,value.execute)==owner->execute_methods.begin()+owner->method_count) {
            if(owner->method_count==16) ++owner->failed;else owner->execute_methods[owner->method_count++]=value.execute;
        }
    }
    if(owner->failed || FAILED(device->SetPrivateData(DEVICE_KEY,sizeof(*owner),&*owner))) ++m_errors;
}
std::optional<GpuQueueProbe::Queue> GpuQueueProbe::read(ID3D12CommandQueue* queue) {
    if(!queue) return std::nullopt;Queue value;UINT bytes=sizeof(value);
    if(FAILED(queue->GetPrivateData(QUEUE_KEY,&bytes,&value)) || bytes!=sizeof(value) || value.magic!=0x534d325155455545 || !value.serial ||
       value.identity!=GpuObjectIdentity::get(queue) || value.execute!=reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(queue))[10])) return std::nullopt;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;if(FAILED(queue->GetDevice(IID_PPV_ARGS(&owner)))) return std::nullopt;
    const auto device=read_device(owner.Get());if(!device || device->serial!=value.device_serial || device->identity!=value.device) return std::nullopt;
    const auto actual=queue->GetDesc();
    if(actual.Type!=value.desc.Type || actual.Priority!=value.desc.Priority || actual.Flags!=value.desc.Flags || actual.NodeMask!=value.desc.NodeMask) return std::nullopt;
    return value;
}
bool GpuQueueProbe::all_observed_methods_match(const Device& device,uintptr_t implementation) {
    return !get().errors() && device.early_return && device.serial && !device.failed && device.hooks==(device.device9?3:1) && device.method_count>0 && device.method_count<=16 && implementation &&
        std::all_of(device.execute_methods.begin(),device.execute_methods.begin()+device.method_count,[&](auto method){return method==implementation;});
}
