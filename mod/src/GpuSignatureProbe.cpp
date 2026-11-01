#include "GpuSignatureProbe.h"
#include "GpuObjectIdentity.h"
#include "GpuQueueProbe.h"
#include "RenderTargetProbe.h"
#include "GpuClosedListProbe.h"
#include "HistoryDescriptorProbe.h"
#include <wrl/client.h>
#include <MinHook.h>
#include <intrin.h>

GpuSignatureProbe& GpuSignatureProbe::get() {static auto* value=new GpuSignatureProbe;return *value;}
bool GpuSignatureProbe::install_device_creation() {
    std::lock_guard lock{m_mutex};if(m_early) return true;
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return false;
    auto module=GetModuleHandleW(L"d3d12.dll");if(!module) module=LoadLibraryW(L"d3d12.dll");if(!module) return false;
    const auto address=reinterpret_cast<void*>(GetProcAddress(module,"D3D12CreateDevice"));if(!address) return false;
    void* original{};
    if(MH_CreateHook(address,reinterpret_cast<void*>(&on_device),&original)!=MH_OK) return false;
    s_create_device=reinterpret_cast<CreateDevice>(original);
    if(MH_EnableHook(address)!=MH_OK) {MH_RemoveHook(address);return false;}
    m_early=true;return true;
}
bool GpuSignatureProbe::observe_device(ID3D12Device* device) {
    if(!device || !GpuObjectIdentity::get(device)) return false;
    auto* address=(*reinterpret_cast<void***>(device))[41];std::lock_guard lock{m_mutex};
    if(m_implementations.contains(address)) return true;
    if(m_implementations.size()>=16) {++m_failed;return false;}
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return false;
    const std::array<Signature,16> hooks{&on_signature<0>,&on_signature<1>,&on_signature<2>,&on_signature<3>,&on_signature<4>,&on_signature<5>,&on_signature<6>,&on_signature<7>,
        &on_signature<8>,&on_signature<9>,&on_signature<10>,&on_signature<11>,&on_signature<12>,&on_signature<13>,&on_signature<14>,&on_signature<15>};
    const auto index=m_implementations.size();void* original{};
    if(MH_CreateHook(address,reinterpret_cast<void*>(hooks[index]),&original)!=MH_OK) {++m_failed;return false;}
    m_originals[index]=reinterpret_cast<Signature>(original);m_implementations.emplace(address,index);
    if(MH_EnableHook(address)!=MH_OK) {m_implementations.erase(address);MH_RemoveHook(address);++m_failed;return false;}
    return true;
}
GpuSignatureProbe::Status GpuSignatureProbe::status() const {
    std::lock_guard lock{m_mutex};return {m_early.load(),m_devices.load(),m_calls.load(),m_annotated.load(),m_failed.load(),m_implementations.size()};
}
HRESULT WINAPI GpuSignatureProbe::on_device(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** output) {
    const auto hr=s_create_device(adapter,level,iid,output);
    auto& probe=get();++probe.m_devices;
    if(SUCCEEDED(hr) && output && *output) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if(SUCCEEDED(reinterpret_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&device)))) {
            try {if(!GpuQueueProbe::get().observe_device(device.Get(),true)) ++probe.m_failed;} catch(...) {++probe.m_failed;}
            try {if(!RenderTargetProbe::get().observe_creation(device.Get())) ++probe.m_failed;} catch(...) {++probe.m_failed;}
            char history[8]{};
            if(GetEnvironmentVariableA("SM2VR_ORDINARY_GLOBAL_HISTORY",history,sizeof(history))==1 && history[0]=='1')
                try {if(!HistoryDescriptorProbe::get().install(device.Get())) ++probe.m_failed;} catch(...) {++probe.m_failed;}
            try {if(!GpuClosedListProbe::get().install(device.Get())) ++probe.m_failed;} catch(...) {++probe.m_failed;}
            probe.observe_device(device.Get());
        }
        else ++probe.m_failed;
    }
    return hr;
}
template<size_t Index> HRESULT STDMETHODCALLTYPE GpuSignatureProbe::on_signature(ID3D12Device* device,const D3D12_COMMAND_SIGNATURE_DESC* desc,ID3D12RootSignature* root,REFIID iid,void** output) {
    auto& probe=get();const auto original=probe.m_originals[Index].load();
    if(!original) {++probe.m_failed;return E_UNEXPECTED;}
    const auto hr=original(device,desc,root,iid,output);
    if(SUCCEEDED(hr)) probe.annotate(device,desc,root,output,reinterpret_cast<uintptr_t>(_ReturnAddress()));return hr;
}
void GpuSignatureProbe::annotate(ID3D12Device* device,const D3D12_COMMAND_SIGNATURE_DESC* desc,ID3D12RootSignature* root,void** output,uintptr_t callsite) {
    if(!output || !*output) return;++m_calls;
    if(!desc || !desc->pArgumentDescs || !desc->NumArgumentDescs || desc->NumArgumentDescs>32) return;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> signature;
    if(FAILED(reinterpret_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&signature)))) {++m_failed;return;}
    Record value;value.serial=++m_serial;value.device=GpuObjectIdentity::get(device);value.signature=GpuObjectIdentity::get(signature.Get());
    value.callsite=callsite;value.thread=GetCurrentThreadId();value.root=GpuObjectIdentity::get(root);value.shape.stride=desc->ByteStride;value.shape.count=desc->NumArgumentDescs;
    for(UINT i=0;i<desc->NumArgumentDescs;++i) value.shape.arguments[i]=desc->pArgumentDescs[i];
    if(!value.device || !value.signature || value.shape.action()==GpuIndirectEvidence::unknown) return;
    if(FAILED(signature->SetPrivateData(KEY,sizeof(value),&value))) {++m_failed;return;}++m_annotated;
}
std::optional<GpuSignatureProbe::Record> GpuSignatureProbe::read(ID3D12CommandSignature* signature) {
    if(!signature) return std::nullopt;Record value;UINT bytes=sizeof(value);
    if(FAILED(signature->GetPrivateData(KEY,&bytes,&value)) || bytes!=sizeof(value) || value.magic!=0x534d3253 || value.version!=1 ||
       !value.serial || !value.device || !value.signature || value.signature!=GpuObjectIdentity::get(signature) || value.shape.action()==GpuIndirectEvidence::unknown) return std::nullopt;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    if(FAILED(signature->GetDevice(IID_PPV_ARGS(&owner))) || GpuObjectIdentity::get(owner.Get())!=value.device) return std::nullopt;
    return value;
}
