#include "RenderTargetProbe.h"
#include <MinHook.h>
#include <wrl/client.h>
#include "GpuApiForwardScope.h"

bool RenderTargetProbe::observe_creation(ID3D12Device* device) {
    if(!device) return false;
    std::lock_guard lock{m_install};if(s_probe && s_probe!=this) return false;
    auto** methods=*reinterpret_cast<void***>(device);
    const std::array<void*,3> addresses{methods[20],methods[23],methods[24]};
    if(m_creation_installed) {const bool same=addresses==m_creation_addresses;if(!same) ++m_errors;return same;}
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return false;
    const std::array<void*,3> hooks{reinterpret_cast<void*>(&on_create),reinterpret_cast<void*>(&on_copy),reinterpret_cast<void*>(&on_simple)};
    std::array<void*,3> originals{};size_t made{};
    for(;made<addresses.size();++made) if(MH_CreateHook(addresses[made],hooks[made],&originals[made])!=MH_OK) break;
    if(made!=addresses.size()) {for(size_t i=0;i<made;++i) MH_RemoveHook(addresses[i]);return false;}
    s_create=reinterpret_cast<Create>(originals[0]);s_copy=reinterpret_cast<Copy>(originals[1]);s_simple=reinterpret_cast<Simple>(originals[2]);
    m_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);s_probe=this;
    for(auto address:addresses) if(MH_EnableHook(address)!=MH_OK) {
        for(auto a:addresses) MH_DisableHook(a);s_probe=nullptr;for(auto a:addresses) MH_RemoveHook(a);++m_errors;return false;
    }
    m_creation_addresses=addresses;m_creation_installed=true;return true;
}
bool RenderTargetProbe::install(ID3D12Device* device) {
    if(!observe_creation(device)) return false;
    std::lock_guard lock{m_install};if(m_bind_address) return true;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)))) return false;
    if(FAILED(commands->Close())) return false;
    auto* address=(*reinterpret_cast<void***>(commands.Get()))[46];void* original{};
    if(MH_CreateHook(address,reinterpret_cast<void*>(&on_bind),&original)!=MH_OK) return false;
    s_bind=reinterpret_cast<Bind>(original);
    if(MH_EnableHook(address)!=MH_OK) {MH_RemoveHook(address);++m_errors;return false;}
    m_bind_address=address;
    return true;
}
void STDMETHODCALLTYPE RenderTargetProbe::on_create(ID3D12Device* device,ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    s_create.load()(device,resource,desc,handle);
    try {if(auto probe=s_probe.load()) {probe->m_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);probe->created(resource,desc,handle);}} catch(...) {if(auto probe=s_probe.load()) {++probe->m_errors;probe->clear();}}
}
void STDMETHODCALLTYPE RenderTargetProbe::on_bind(ID3D12GraphicsCommandList* commands,UINT count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* handles,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    if(GpuApiForwardScope::active()){s_bind.load()(commands,count,handles,contiguous,depth);return;}
    std::optional<Binding> replacement;
    try{if(auto probe=s_probe.load())replacement=probe->transformed(commands,count,handles,contiguous,depth);}catch(...){if(auto probe=s_probe.load())++probe->m_errors;}
    if(replacement){if(auto probe=s_probe.load())probe->restore_binding(commands,*replacement);return;}
    {GpuApiForwardScope forwarding;s_bind.load()(commands,count,handles,contiguous,depth);}
    try {if(auto probe=s_probe.load()) probe->bound(reinterpret_cast<uintptr_t>(commands),count,handles,contiguous!=FALSE,probe->m_stride);} catch(...) {if(auto probe=s_probe.load()) {++probe->m_errors;probe->clear();}}
}
std::optional<RenderTargetProbe::Binding> RenderTargetProbe::transformed(ID3D12GraphicsCommandList* command,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* handles,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE* depth){
    if(!transform_binding || count>8 || (count && !handles) || (count && contiguous && (!m_stride || handles[0].ptr>UINTPTR_MAX-size_t(count-1)*m_stride)))return std::nullopt;
    Binding native;native.count=count;if(depth)native.depth=*depth;std::array<View,8> views{};
    {std::lock_guard lock{m_mutex};for(UINT i=0;i<count;++i){native.handles[i]=contiguous?D3D12_CPU_DESCRIPTOR_HANDLE{handles[0].ptr+size_t(i)*m_stride}:handles[i];if(const auto* found=find(native.handles[i].ptr))views[i]=*found;}}
    auto result=transform_binding(command,native,std::span{views}.first(count));if(result && result->count!=count)return std::nullopt;return result;
}
void RenderTargetProbe::restore_binding(ID3D12GraphicsCommandList* command,const Binding& binding){
    {GpuApiForwardScope forwarding;s_bind.load()(command,binding.count,binding.handles.data(),FALSE,binding.depth?&*binding.depth:nullptr);}
    bound(reinterpret_cast<uintptr_t>(command),binding.count,binding.handles.data(),false,m_stride);
}
void STDMETHODCALLTYPE RenderTargetProbe::on_copy(ID3D12Device* device,UINT nd,const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,const UINT* ds,
    UINT ns,const D3D12_CPU_DESCRIPTOR_HANDLE* sources,const UINT* ss,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    s_copy(device,nd,destinations,ds,ns,sources,ss,type);
    try {if(type==D3D12_DESCRIPTOR_HEAP_TYPE_RTV) if(auto probe=s_probe.load()) probe->copied(nd,destinations,ds,ns,sources,ss,device->GetDescriptorHandleIncrementSize(type));} catch(...) {if(auto probe=s_probe.load()) probe->clear();}
}
void STDMETHODCALLTYPE RenderTargetProbe::on_simple(ID3D12Device* device,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE destination,D3D12_CPU_DESCRIPTOR_HANDLE source,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    s_simple(device,count,destination,source,type);
    try {if(type==D3D12_DESCRIPTOR_HEAP_TYPE_RTV) if(auto probe=s_probe.load()) probe->copied(1,&destination,&count,1,&source,&count,device->GetDescriptorHandleIncrementSize(type));} catch(...) {if(auto probe=s_probe.load()) probe->clear();}
}
