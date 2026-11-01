#pragma once
#include <d3d12.h>
#include <array>
#include <functional>
#include "GpuTargetAccess.h"
#include "GpuAllocationProbe.h"

// Observe additional texture/opaque writer APIs; no command/resource leases.
class GpuExtendedWriterProbe {
public:
    using Kind=GpuTargetAccess::Extended;
    using Callback=std::function<void(uintptr_t,Kind,uintptr_t,uintptr_t,std::optional<GpuAllocationProbe::Record>)>;
    struct Status {bool known{},list1{},list4{},list6{};uint32_t hook_mask{};};
    bool install(ID3D12GraphicsCommandList*,Callback);
    bool covers(ID3D12GraphicsCommandList*) const;
    Status status() const {return m_status;}
private:
    static uintptr_t base_key(IUnknown*);
    static void emit(uintptr_t,Kind,ID3D12Resource*,uintptr_t);
    using Discard=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,const D3D12_DISCARD_REGION*);
    using Depth=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*);
    using Resolve=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList1*,ID3D12Resource*,UINT,UINT,UINT,ID3D12Resource*,UINT,D3D12_RECT*,DXGI_FORMAT,D3D12_RESOLVE_MODE);
    using Meta=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,ID3D12MetaCommand*,const void*,SIZE_T);
    using Rays=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,const D3D12_DISPATCH_RAYS_DESC*);
    using Mesh=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList6*,UINT,UINT,UINT);
    using Tiles=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,const D3D12_TILED_RESOURCE_COORDINATE*,const D3D12_TILE_REGION_SIZE*,ID3D12Resource*,UINT64,D3D12_TILE_COPY_FLAGS);
    static void STDMETHODCALLTYPE on_discard(ID3D12GraphicsCommandList*,ID3D12Resource*,const D3D12_DISCARD_REGION*);
    static void STDMETHODCALLTYPE on_depth(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*);
    static void STDMETHODCALLTYPE on_resolve(ID3D12GraphicsCommandList1*,ID3D12Resource*,UINT,UINT,UINT,ID3D12Resource*,UINT,D3D12_RECT*,DXGI_FORMAT,D3D12_RESOLVE_MODE);
    static void STDMETHODCALLTYPE on_initialize(ID3D12GraphicsCommandList4*,ID3D12MetaCommand*,const void*,SIZE_T);
    static void STDMETHODCALLTYPE on_execute(ID3D12GraphicsCommandList4*,ID3D12MetaCommand*,const void*,SIZE_T);
    static void STDMETHODCALLTYPE on_rays(ID3D12GraphicsCommandList4*,const D3D12_DISPATCH_RAYS_DESC*);
    static void STDMETHODCALLTYPE on_mesh(ID3D12GraphicsCommandList6*,UINT,UINT,UINT);
    static void STDMETHODCALLTYPE on_tiles(ID3D12GraphicsCommandList*,ID3D12Resource*,const D3D12_TILED_RESOURCE_COORDINATE*,const D3D12_TILE_REGION_SIZE*,ID3D12Resource*,UINT64,D3D12_TILE_COPY_FLAGS);
    inline static GpuExtendedWriterProbe* s_probe{};
    inline static Discard s_discard{};inline static Depth s_depth{};inline static Resolve s_resolve{};
    inline static Meta s_initialize{},s_execute{};inline static Rays s_rays{};inline static Mesh s_mesh{};inline static Tiles s_tiles{};
    Status m_status;Callback m_callback;std::array<void*,GpuTargetAccess::extended_count> m_addresses{};
};
