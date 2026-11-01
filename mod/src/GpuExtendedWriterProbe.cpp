#include "GpuExtendedWriterProbe.h"
#include "GpuObjectIdentity.h"
#include <MinHook.h>
#include <intrin.h>
#include <vector>
#include <utility>
#include <initializer_list>

bool GpuExtendedWriterProbe::install(ID3D12GraphicsCommandList* native,Callback callback) {
    if(!native || s_probe || !callback) return false;
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return false;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> one;Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> four;Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> six;
    const auto h1=native->QueryInterface(IID_PPV_ARGS(&one)),h4=native->QueryInterface(IID_PPV_ARGS(&four)),h6=native->QueryInterface(IID_PPV_ARGS(&six));
    for(auto hr:{h1,h4,h6}) if(FAILED(hr) && hr!=E_NOINTERFACE) return false;
    std::vector<void*> addresses,hooks;std::array<size_t,GpuTargetAccess::extended_count> indices{};
    uint32_t mask{};
    const auto add=[&](Kind kind,void* address,void* hook) {indices[kind]=addresses.size();addresses.push_back(address);hooks.push_back(hook);m_addresses[kind]=address;mask|=1u<<kind;};
    auto** base=*reinterpret_cast<void***>(native);
    add(Kind::discard,base[51],reinterpret_cast<void*>(&on_discard));add(Kind::depth,base[47],reinterpret_cast<void*>(&on_depth));add(Kind::copy_tiles,base[18],reinterpret_cast<void*>(&on_tiles));
    if(one) add(Kind::resolve_region,(*reinterpret_cast<void***>(one.Get()))[64],reinterpret_cast<void*>(&on_resolve));
    if(four) {auto** table=*reinterpret_cast<void***>(four.Get());add(Kind::initialize_meta,table[70],reinterpret_cast<void*>(&on_initialize));add(Kind::execute_meta,table[71],reinterpret_cast<void*>(&on_execute));add(Kind::dispatch_rays,table[76],reinterpret_cast<void*>(&on_rays));}
    if(six) add(Kind::dispatch_mesh,(*reinterpret_cast<void***>(six.Get()))[79],reinterpret_cast<void*>(&on_mesh));
    std::vector<void*> originals(addresses.size());size_t made{};
    for(;made<addresses.size();++made) if(MH_CreateHook(addresses[made],hooks[made],&originals[made])!=MH_OK) break;
    if(made!=addresses.size()) {for(size_t i=0;i<made;++i) MH_RemoveHook(addresses[i]);return false;}
    s_discard=reinterpret_cast<Discard>(originals[indices[Kind::discard]]);s_depth=reinterpret_cast<Depth>(originals[indices[Kind::depth]]);s_tiles=reinterpret_cast<Tiles>(originals[indices[Kind::copy_tiles]]);
    if(one) s_resolve=reinterpret_cast<Resolve>(originals[indices[Kind::resolve_region]]);
    if(four) {s_initialize=reinterpret_cast<Meta>(originals[indices[Kind::initialize_meta]]);s_execute=reinterpret_cast<Meta>(originals[indices[Kind::execute_meta]]);s_rays=reinterpret_cast<Rays>(originals[indices[Kind::dispatch_rays]]);}
    if(six) s_mesh=reinterpret_cast<Mesh>(originals[indices[Kind::dispatch_mesh]]);
    m_callback=std::move(callback);s_probe=this;
    for(auto address:addresses) if(MH_EnableHook(address)!=MH_OK) {for(auto a:addresses) MH_DisableHook(a);s_probe=nullptr;for(auto a:addresses) MH_RemoveHook(a);return false;}
    m_status={true,bool(one),bool(four),bool(six),mask};return true;
}
bool GpuExtendedWriterProbe::covers(ID3D12GraphicsCommandList* native) const {
    if(!native || !m_status.known) return false;auto** table=*reinterpret_cast<void***>(native);
    if(table[51]!=m_addresses[Kind::discard] || table[47]!=m_addresses[Kind::depth] || table[18]!=m_addresses[Kind::copy_tiles]) return false;
    const auto same=[&]<class T>(bool expected,std::initializer_list<std::pair<Kind,size_t>> methods) {
        Microsoft::WRL::ComPtr<T> value;const auto hr=native->QueryInterface(IID_PPV_ARGS(&value));
        if(!expected) return hr==E_NOINTERFACE;if(FAILED(hr)) return false;auto** actual=*reinterpret_cast<void***>(value.Get());
        for(auto [kind,slot]:methods) if(actual[slot]!=m_addresses[kind]) return false;return true;
    };
    return same.operator()<ID3D12GraphicsCommandList1>(m_status.list1,{{Kind::resolve_region,64}}) &&
        same.operator()<ID3D12GraphicsCommandList4>(m_status.list4,{{Kind::initialize_meta,70},{Kind::execute_meta,71},{Kind::dispatch_rays,76}}) &&
        same.operator()<ID3D12GraphicsCommandList6>(m_status.list6,{{Kind::dispatch_mesh,79}});
}
uintptr_t GpuExtendedWriterProbe::base_key(IUnknown* value) {
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> base;
    return value && SUCCEEDED(value->QueryInterface(IID_PPV_ARGS(&base)))?reinterpret_cast<uintptr_t>(base.Get()):0;
}
void GpuExtendedWriterProbe::emit(uintptr_t command,Kind kind,ID3D12Resource* resource,uintptr_t callsite) {
    // Resource metadata is copied while the real native API argument is live.
    if(s_probe) s_probe->m_callback(command,kind,GpuObjectIdentity::get(resource),callsite,GpuAllocationProbe::read(resource));
}
void STDMETHODCALLTYPE GpuExtendedWriterProbe::on_discard(ID3D12GraphicsCommandList* c,ID3D12Resource* r,const D3D12_DISCARD_REGION* region) {
    emit(reinterpret_cast<uintptr_t>(c),Kind::discard,r,reinterpret_cast<uintptr_t>(_ReturnAddress()));s_discard(c,r,region);
}
void STDMETHODCALLTYPE GpuExtendedWriterProbe::on_depth(ID3D12GraphicsCommandList* c,D3D12_CPU_DESCRIPTOR_HANDLE d,D3D12_CLEAR_FLAGS flags,FLOAT depth,UINT8 stencil,UINT count,const D3D12_RECT* rects) {
    emit(reinterpret_cast<uintptr_t>(c),Kind::depth,nullptr,reinterpret_cast<uintptr_t>(_ReturnAddress()));s_depth(c,d,flags,depth,stencil,count,rects);
}
void STDMETHODCALLTYPE GpuExtendedWriterProbe::on_resolve(ID3D12GraphicsCommandList1* c,ID3D12Resource* dst,UINT sub,UINT x,UINT y,ID3D12Resource* src,UINT src_sub,D3D12_RECT* rect,DXGI_FORMAT format,D3D12_RESOLVE_MODE mode) {
    emit(base_key(c),Kind::resolve_region,dst,reinterpret_cast<uintptr_t>(_ReturnAddress()));s_resolve(c,dst,sub,x,y,src,src_sub,rect,format,mode);
}
void STDMETHODCALLTYPE GpuExtendedWriterProbe::on_initialize(ID3D12GraphicsCommandList4* c,ID3D12MetaCommand* command,const void* data,SIZE_T bytes) {
    emit(base_key(c),Kind::initialize_meta,nullptr,reinterpret_cast<uintptr_t>(_ReturnAddress()));s_initialize(c,command,data,bytes);
}
void STDMETHODCALLTYPE GpuExtendedWriterProbe::on_execute(ID3D12GraphicsCommandList4* c,ID3D12MetaCommand* command,const void* data,SIZE_T bytes) {
    emit(base_key(c),Kind::execute_meta,nullptr,reinterpret_cast<uintptr_t>(_ReturnAddress()));s_execute(c,command,data,bytes);
}
void STDMETHODCALLTYPE GpuExtendedWriterProbe::on_rays(ID3D12GraphicsCommandList4* c,const D3D12_DISPATCH_RAYS_DESC* desc) {
    emit(base_key(c),Kind::dispatch_rays,nullptr,reinterpret_cast<uintptr_t>(_ReturnAddress()));s_rays(c,desc);
}
void STDMETHODCALLTYPE GpuExtendedWriterProbe::on_mesh(ID3D12GraphicsCommandList6* c,UINT x,UINT y,UINT z) {
    emit(base_key(c),Kind::dispatch_mesh,nullptr,reinterpret_cast<uintptr_t>(_ReturnAddress()));s_mesh(c,x,y,z);
}
void STDMETHODCALLTYPE GpuExtendedWriterProbe::on_tiles(ID3D12GraphicsCommandList* c,ID3D12Resource* tiled,const D3D12_TILED_RESOURCE_COORDINATE* start,const D3D12_TILE_REGION_SIZE* size,ID3D12Resource* buffer,UINT64 offset,D3D12_TILE_COPY_FLAGS flags) {
    // Either direction may write. A named private committed target is not
    // considered safe merely because this API expects tiled resources.
    emit(reinterpret_cast<uintptr_t>(c),Kind::copy_tiles,tiled,reinterpret_cast<uintptr_t>(_ReturnAddress()));s_tiles(c,tiled,start,size,buffer,offset,flags);
}
