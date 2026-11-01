#include "GpuClosedListProbe.h"
#include "GpuObjectIdentity.h"
#include <wrl/client.h>
#include <MinHook.h>

bool GpuClosedListProbe::install(ID3D12Device* device) {
    if(!device) return false;std::lock_guard lock{m_install};
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)))) {++m_errors;return false;}
    auto* address=(*reinterpret_cast<void***>(commands.Get()))[9];
    if(m_original) {const bool same=m_address==address;if(!same) ++m_errors;commands->Close();return same && install_creation(device);}
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) {++m_errors;return false;}
    void* original{};if(MH_CreateHook(address,reinterpret_cast<void*>(&on_close),&original)!=MH_OK) {++m_errors;return false;}
    m_original=reinterpret_cast<Close>(original);m_address=address;
    if(MH_EnableHook(address)!=MH_OK) {MH_RemoveHook(address);m_original=nullptr;m_address=nullptr;++m_errors;return false;}
    return SUCCEEDED(commands->Close()) && install_creation(device);
}
bool GpuClosedListProbe::install_creation(ID3D12Device* device) {
    auto* address=(*reinterpret_cast<void***>(device))[12];
    if(m_create) {if(address!=m_create_address) {++m_errors;return false;}}
    else {
        void* original{};if(MH_CreateHook(address,reinterpret_cast<void*>(&on_create),&original)!=MH_OK) {++m_errors;return false;}
        m_create=reinterpret_cast<Create>(original);m_create_address=address;
        if(MH_EnableHook(address)!=MH_OK) {MH_RemoveHook(address);m_create=nullptr;m_create_address=nullptr;++m_errors;return false;}
    }
    Microsoft::WRL::ComPtr<ID3D12Device4> extended;const auto hr=device->QueryInterface(IID_PPV_ARGS(&extended));
    if(hr==E_NOINTERFACE) return true;if(FAILED(hr)) {++m_errors;return false;}
    address=(*reinterpret_cast<void***>(extended.Get()))[51];
    if(m_create1) {if(address!=m_create1_address) {++m_errors;return false;}return true;}
    void* original{};if(MH_CreateHook(address,reinterpret_cast<void*>(&on_create1),&original)!=MH_OK) {++m_errors;return false;}
    m_create1=reinterpret_cast<Create1>(original);m_create1_address=address;
    if(MH_EnableHook(address)!=MH_OK) {MH_RemoveHook(address);m_create1=nullptr;m_create1_address=nullptr;++m_errors;return false;}return true;
}
void GpuClosedListProbe::created(void** output,uint32_t api,bool initial_closed) {
    if(!output || !*output) return;
    try {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command;Microsoft::WRL::ComPtr<ID3D12Device> device;
        if(FAILED(reinterpret_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&command))) || FAILED(command->GetDevice(IID_PPV_ARGS(&device)))) {++m_errors;return;}
        Birth value;value.serial=++m_serial;value.created_ns=now();value.identity=GpuObjectIdentity::get(command.Get());value.device=GpuObjectIdentity::get(device.Get());
        value.type=uint32_t(command->GetType());value.api=api;value.initial_closed=initial_closed;
        if(!value.identity || !value.device || FAILED(command->SetPrivateData(BIRTH_KEY,sizeof(value),&value))) {++m_errors;return;}
        std::function<void(uintptr_t,bool)> observer;{std::lock_guard lock{m_mutex};observer=m_creation_observer;}
        if(observer) observer(reinterpret_cast<uintptr_t>(command.Get()),initial_closed);
    } catch(...) {++m_errors;}
}
HRESULT STDMETHODCALLTYPE GpuClosedListProbe::on_create(ID3D12Device* device,UINT node,D3D12_COMMAND_LIST_TYPE type,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline,REFIID iid,void** output) {
    auto& self=get();const bool outer=!s_creating;struct Scope {bool previous{s_creating};Scope(){s_creating=true;}~Scope(){s_creating=previous;}} scope;
    const auto hr=self.m_create.load()(device,node,type,allocator,pipeline,iid,output);if(outer && SUCCEEDED(hr)) self.created(output,1,false);return hr;
}
HRESULT STDMETHODCALLTYPE GpuClosedListProbe::on_create1(ID3D12Device4* device,UINT node,D3D12_COMMAND_LIST_TYPE type,D3D12_COMMAND_LIST_FLAGS flags,REFIID iid,void** output) {
    auto& self=get();const bool outer=!s_creating;struct Scope {bool previous{s_creating};Scope(){s_creating=true;}~Scope(){s_creating=previous;}} scope;
    const auto hr=self.m_create1.load()(device,node,type,flags,iid,output);if(outer && SUCCEEDED(hr)) self.created(output,2,true);return hr;
}
std::optional<GpuClosedListProbe::Birth> GpuClosedListProbe::birth(ID3D12CommandList* command) {
    if(!command) return std::nullopt;Birth value;UINT bytes=sizeof(value);Microsoft::WRL::ComPtr<ID3D12Device> device;
    if(FAILED(command->GetPrivateData(BIRTH_KEY,&bytes,&value)) || bytes!=sizeof(value) || value.magic!=0x534d324249525448 || !value.serial || !value.created_ns ||
       !value.identity || value.identity!=GpuObjectIdentity::get(command) || value.type!=uint32_t(command->GetType()) || value.api<1 || value.api>2 || value.initial_closed!=(value.api==2) ||
       FAILED(command->GetDevice(IID_PPV_ARGS(&device))) || value.device!=GpuObjectIdentity::get(device.Get())) return std::nullopt;return value;
}
HRESULT STDMETHODCALLTYPE GpuClosedListProbe::on_close(ID3D12GraphicsCommandList* command) {
    auto& self=get();const auto hr=self.m_original.load()(command);
    if(SUCCEEDED(hr)) {
        Receipt value;value.serial=++self.m_serial;value.closed_ns=now();value.identity=GpuObjectIdentity::get(command);value.type=uint32_t(command->GetType());
        auto** methods=*reinterpret_cast<void***>(command);value.reset=reinterpret_cast<uintptr_t>(methods[10]);value.close=reinterpret_cast<uintptr_t>(methods[9]);
        Microsoft::WRL::ComPtr<ID3D12Device> owner;if(SUCCEEDED(command->GetDevice(IID_PPV_ARGS(&owner)))) value.device=GpuObjectIdentity::get(owner.Get());
        if(!value.identity || !value.device || FAILED(command->SetPrivateData(KEY,sizeof(value),&value))) ++self.m_errors;
    } else self.invalidate(command);
    try {
        std::function<void(uintptr_t,bool)> observer;{std::lock_guard lock{self.m_mutex};observer=self.m_observer;}
        if(observer) observer(reinterpret_cast<uintptr_t>(command),SUCCEEDED(hr));
    } catch(...) {++self.m_errors;}return hr;
}
std::optional<GpuClosedListProbe::Receipt> GpuClosedListProbe::read(ID3D12CommandList* command) {
    if(!command) return std::nullopt;Receipt value;UINT bytes=sizeof(value);
    if(FAILED(command->GetPrivateData(KEY,&bytes,&value)) || bytes!=sizeof(value) || value.magic!=0x534d32434c4f5345 ||
       !value.serial || !value.closed_ns || !value.identity || value.identity!=GpuObjectIdentity::get(command) || value.type!=uint32_t(command->GetType())) return std::nullopt;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> graphics;
    if(FAILED(command->GetDevice(IID_PPV_ARGS(&owner))) || GpuObjectIdentity::get(owner.Get())!=value.device || FAILED(command->QueryInterface(IID_PPV_ARGS(&graphics)))) return std::nullopt;
    auto** methods=*reinterpret_cast<void***>(graphics.Get());
    return value.reset==reinterpret_cast<uintptr_t>(methods[10]) && value.close==reinterpret_cast<uintptr_t>(methods[9])?std::optional{value}:std::nullopt;
}
