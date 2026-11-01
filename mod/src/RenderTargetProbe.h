#pragma once
#include <d3d12.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <array>
#include <functional>
#include <span>
#include <optional>
#include <wrl/client.h>
#include "GpuObjectIdentity.h"

// Bounded CPU descriptor observations. These do not prove execution or GPU frame ownership.
// Never retain/dereference resource pointers after the native callback returns.
class RenderTargetProbe {
public:
    static constexpr size_t CAPACITY=2048;
    static RenderTargetProbe& get() {static auto* value=new RenderTargetProbe;return *value;}
    bool observe_creation(ID3D12Device* device);
    bool install(ID3D12Device* device); // Explicit desktop experiment only.
    struct Status {size_t cached{};uint64_t evictions{},missing_bindings{},missing_copy_sources{},errors{};bool early_creation{};};
    Status status() const {std::lock_guard lock{m_mutex};return {m_views.size(),m_evictions,m_missing_bindings,m_missing_copy_sources,m_errors.load(),m_creation_installed};}
    std::function<void(uintptr_t,std::span<const uintptr_t>,bool)> observe_binding; // Set before installing hooks; bool means all descriptors observed.
    bool observes_binding(uintptr_t command) const {return command && m_bind_address && (*reinterpret_cast<void***>(command))[46]==m_bind_address;}
    uintptr_t resource(uintptr_t handle) const {std::lock_guard lock{m_mutex};const auto* view=find(handle);return view?view->resource:0;}
    uintptr_t canonical_resource(uintptr_t handle) const {std::lock_guard lock{m_mutex};const auto* view=find(handle);return view?view->identity:0;}
    struct View {
        uintptr_t resource{},handle{},command_list{};
        DXGI_FORMAT storage{},format{};
        D3D12_RTV_DIMENSION dimension{};
        uint64_t width{},created{},bound{};uint32_t height{};
        bool known{};
        uintptr_t identity{};
        D3D12_RESOURCE_DIMENSION resource_dimension{};
        D3D12_RESOURCE_FLAGS flags{};
        uint16_t depth_or_array{},mips{};
        uint32_t samples{},planes{},subresources{};
    };
    struct Binding {UINT count{};std::array<D3D12_CPU_DESCRIPTOR_HANDLE,8> handles{};std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> depth;};
    using BindingTransform=std::function<std::optional<Binding>(ID3D12GraphicsCommandList*,const Binding&,std::span<const View>)>;
    BindingTransform transform_binding; // Startup-only opt-in; empty leaves exact native ABI untouched.
    // Restore a binding captured from an actual callback; bypass the transform,
    // but keep the normal observation and original native call exactly once.
    void restore_binding(ID3D12GraphicsCommandList*,const Binding&);
    void created(ID3D12Resource* resource,const D3D12_RENDER_TARGET_VIEW_DESC* view,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        if(!handle.ptr) return;
        View entry;entry.handle=handle.ptr;entry.known=resource || view;
        if(resource) {
            const auto desc=resource->GetDesc();entry.resource=reinterpret_cast<uintptr_t>(resource);
            entry.identity=GpuObjectIdentity::get(resource);entry.known=entry.identity!=0;
            entry.width=desc.Width;entry.height=desc.Height;entry.storage=desc.Format;
            entry.resource_dimension=desc.Dimension;entry.flags=desc.Flags;
            entry.depth_or_array=desc.DepthOrArraySize;entry.mips=desc.MipLevels;entry.samples=desc.SampleDesc.Count;
            // Query only while the actual CreateRTV resource argument is live.
            // Cached pointers are never queried or AddRef'd by readers.
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            D3D12_FEATURE_DATA_FORMAT_INFO info{desc.Format,0};
            if(SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&device))) &&
               SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&info,sizeof(info)))) entry.planes=info.PlaneCount;
            if(desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) entry.subresources=1;
            else entry.subresources=uint32_t(entry.mips)*entry.planes*(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1u:entry.depth_or_array);
            entry.format=view?view->Format:desc.Format;
            entry.dimension=view?view->ViewDimension:D3D12_RTV_DIMENSION_UNKNOWN;
        }
        std::lock_guard lock{m_mutex};entry.created=++m_serial;
        store(entry); // Descriptor reuse invalidates old attribution.
    }
    void bound(uintptr_t command,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* handles,bool contiguous,UINT stride) {
        if(!command) return;
        if(count>8 || (count && (!handles || (contiguous && !stride))) ||
           (count && contiguous && handles[0].ptr>UINTPTR_MAX-size_t(count-1)*stride)) {
            if(observe_binding) observe_binding(command,{},false);return;
        }
        bool known=true;
        std::array<uintptr_t,8> resources{};
        {std::lock_guard lock{m_mutex};const auto serial=++m_serial;
        for(UINT i=0;i<count;++i) {
            const auto handle=contiguous?handles[0].ptr+size_t(i)*stride:handles[i].ptr;
            auto* found=find(handle);
            if(found) {found->bound=serial;found->command_list=command;resources[i]=found->identity;known=known && found->known;if(!found->known) ++m_missing_bindings;}
            else {known=false;++m_missing_bindings;}
        }
        }
        if(observe_binding) observe_binding(command,std::span{resources}.first(count),known);
    }
    void copied(UINT destination_count,const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,const UINT* destination_sizes,
                UINT source_count,const D3D12_CPU_DESCRIPTOR_HANDLE* sources,const UINT* source_sizes,UINT stride) {
        // Flatten bounded ranges before mutating the cache. Unobserved sources
        // invalidate destinations, rather than retaining a previous resource.
        if(!stride || destination_count>CAPACITY || source_count>CAPACITY ||
           (destination_count && !destinations) || (source_count && !sources)) {clear();return;}
        const auto flatten=[&](UINT count,const auto* starts,const UINT* sizes,std::vector<uintptr_t>& output) {
            for(UINT range=0;range<count;++range) {
                const auto size=sizes?sizes[range]:1u;
                if(size>CAPACITY-output.size() || (size && (!starts[range].ptr ||
                   starts[range].ptr>UINTPTR_MAX-size_t(size-1)*stride))) return false;
                for(UINT item=0;item<size;++item) output.push_back(starts[range].ptr+size_t(item)*stride);
            }
            return true;
        };
        std::vector<uintptr_t> to,from;
        if(!flatten(destination_count,destinations,destination_sizes,to) || !flatten(source_count,sources,source_sizes,from) || to.size()!=from.size()) {clear();return;}
        std::lock_guard lock{m_mutex};std::vector<View> copies;copies.reserve(from.size());
        for(size_t i=0;i<from.size();++i) {
            const auto* found=find(from[i]);
            if(!found || !found->known) ++m_missing_copy_sources;
            View value=found?*found:View{};value.handle=to[i];value.bound=value.command_list=0;value.created=++m_serial;copies.push_back(value);
        }
        for(const auto& value:copies) {
            store(value);
        }
    }
    std::vector<View> views(uintptr_t resource) const {
        std::lock_guard lock{m_mutex};std::vector<View> result;
        if(resource) for(const auto& view:m_views) if(view.resource==resource) result.push_back(view);
        return result;
    }
    struct Observation {uint64_t epoch{},serial{};std::vector<View> views;};
    Observation observe(uintptr_t resource) const {
        std::lock_guard lock{m_mutex};Observation result{m_epoch,m_serial,{}};
        if(resource) for(const auto& view:m_views) if(view.resource==resource) result.views.push_back(view);
        return result;
    }
    void clear() {std::lock_guard lock{m_mutex};m_index.clear();m_views.clear();m_serial=0;++m_epoch;}
    // A swap-chain reset. RTV descriptors outlive it: the game makes its back-buffer views again (each
    // new one replaces its handle's entry), and the other views it keeps and the pair retention's
    // private output views stay live. Forgetting them made each later binding of one unknown, and an
    // unknown binding refuses the eye (31 Oct 19:00: docs/FREEZE_REHOOK_2026-10-27.md).
    void device_reset() {std::lock_guard lock{m_mutex};++m_epoch;}
private:
    // Indices survive vector growth and in-place eviction. No COM objects are
    // retained here; all access and mutations share the descriptor-cache lock.
    View* find(uintptr_t handle) {const auto it=m_index.find(handle);return it==m_index.end()?nullptr:&m_views[it->second];}
    const View* find(uintptr_t handle) const {const auto it=m_index.find(handle);return it==m_index.end()?nullptr:&m_views[it->second];}
    void store(const View& value) {
        if(auto* existing=find(value.handle)) {*existing=value;return;}
        if(m_views.size()==CAPACITY) {
            const auto oldest=std::ranges::min_element(m_views,{},&View::created);
            const auto slot=size_t(oldest-m_views.begin());
            // Allocate the new index first so failure cannot expose a stale
            // handle pointing at a replacement resource. Hooks clear on failure.
            m_index.emplace(value.handle,slot);
            m_index.erase(oldest->handle);*oldest=value;++m_evictions;
        } else {
            if(m_views.empty()) {m_views.reserve(CAPACITY);m_index.reserve(CAPACITY+1);}
            m_index.emplace(value.handle,m_views.size());m_views.push_back(value);
        }
    }
    using Create=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    using Bind=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
    using Copy=void(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,D3D12_DESCRIPTOR_HEAP_TYPE);
    using Simple=void(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_DESCRIPTOR_HEAP_TYPE);
    static void STDMETHODCALLTYPE on_create(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    static void STDMETHODCALLTYPE on_bind(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
    std::optional<Binding> transformed(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
    static void STDMETHODCALLTYPE on_copy(ID3D12Device*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,D3D12_DESCRIPTOR_HEAP_TYPE);
    static void STDMETHODCALLTYPE on_simple(ID3D12Device*,UINT,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_DESCRIPTOR_HEAP_TYPE);
    inline static Copy s_copy{};inline static Simple s_simple{};
    inline static std::atomic<RenderTargetProbe*> s_probe{};
    inline static std::atomic<Create> s_create{};inline static std::atomic<Bind> s_bind{};
    std::atomic<UINT> m_stride{};
    std::mutex m_install;std::array<void*,3> m_creation_addresses{};bool m_creation_installed{};
    uint64_t m_evictions{},m_missing_bindings{},m_missing_copy_sources{};std::atomic<uint64_t> m_errors{};
    void* m_bind_address{}; // Immutable installed implementation; read only while actual command argument is live.
    mutable std::mutex m_mutex;std::vector<View> m_views;std::unordered_map<uintptr_t,size_t> m_index;uint64_t m_serial{},m_epoch{1};
};
