#pragma once
#include "GpuObjectIdentity.h"
#include <MinHook.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <ostream>
#include <vector>

// Creation-time POD only. No copied/bound descriptor attribution, resource lease,
// lifetime guarantee or GPU execution claim. Readers never query cached pointers.
class HistoryDescriptorProbe {
public:
    static constexpr size_t CAPACITY=16384; // Per kind; SRV churn cannot erase DSV evidence.
    enum class Kind:uint32_t {srv,dsv};
    struct View {
        Kind kind{};uintptr_t handle{},resource{},identity{},device{};uint64_t serial{};
        uint64_t width{};uint32_t height{},storage{},format{},dimension{},flags{},mips{},arrays{},samples{},planes{},subresources{};
        uint32_t view_dimension{},view_flags{},mip_slice{},first_array{},array_size{},plane_slice{};
        bool known{},explicit_view{};
        bool operator==(const View&)const=default;
    };
    struct Observation {bool installed{};uint64_t serial{},errors{},evictions{};std::vector<View> views;};
    static HistoryDescriptorProbe& get(){static auto* value=new HistoryDescriptorProbe;return *value;}
    bool install(ID3D12Device* device){
        if(!device)return false;std::lock_guard lock{m_install};auto** table=*reinterpret_cast<void***>(device);
        const std::array<void*,2> addresses{table[18],table[21]};
        if(m_installed)return addresses==m_addresses;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        const std::array<void*,2> hooks{reinterpret_cast<void*>(&on_srv),reinterpret_cast<void*>(&on_dsv)};std::array<void*,2> originals{};size_t made{};
        for(;made<2;++made)if(MH_CreateHook(addresses[made],hooks[made],&originals[made])!=MH_OK)break;
        if(made!=2){for(size_t i=0;i<made;++i)MH_RemoveHook(addresses[i]);return false;}
        m_srv=reinterpret_cast<Srv>(originals[0]);m_dsv=reinterpret_cast<Dsv>(originals[1]);
        for(auto address:addresses)if(MH_EnableHook(address)!=MH_OK){for(auto a:addresses)MH_DisableHook(a);for(auto a:addresses)MH_RemoveHook(a);++m_errors;return false;}
        m_addresses=addresses;m_installed=true;return true;
    }
    Observation observe(uintptr_t resource)const{
        std::lock_guard lock{m_mutex};Observation value{m_installed.load(),m_serial,m_errors.load(),m_evictions,{}};
        if(resource)for(const auto& [key,view]:m_views)if(view.resource==resource)value.views.push_back(view);
        return value;
    }
    static void write(std::ostream& out,const Observation& value){
        out<<"{\"model\":\"history_descriptor_creation_v1\",\"creation_only\":true,\"resource_leases\":false,\"bound_writer_verified\":false,\"installed\":"<<(value.installed?"true":"false")<<",\"serial\":"<<value.serial<<",\"errors\":"<<value.errors<<",\"evictions\":"<<value.evictions<<",\"views\":[";
        for(size_t i=0;i<value.views.size();++i){if(i)out<<',';const auto& v=value.views[i];
            out<<"{\"kind\":\""<<(v.kind==Kind::srv?"srv":"dsv")<<"\",\"handle\":"<<v.handle<<",\"resource\":"<<v.resource<<",\"identity\":"<<v.identity<<",\"device\":"<<v.device<<",\"serial\":"<<v.serial<<",\"known\":"<<(v.known?"true":"false")<<",\"explicit_view\":"<<(v.explicit_view?"true":"false");
            out<<",\"width\":"<<v.width<<",\"height\":"<<v.height<<",\"storage\":"<<v.storage<<",\"format\":"<<v.format<<",\"dimension\":"<<v.dimension<<",\"flags\":"<<v.flags<<",\"mips\":"<<v.mips<<",\"arrays\":"<<v.arrays<<",\"samples\":"<<v.samples<<",\"planes\":"<<v.planes<<",\"subresources\":"<<v.subresources<<",\"view_dimension\":"<<v.view_dimension<<",\"view_flags\":"<<v.view_flags<<",\"mip_slice\":"<<v.mip_slice<<",\"first_array\":"<<v.first_array<<",\"array_size\":"<<v.array_size<<",\"plane_slice\":"<<v.plane_slice<<'}';
        }out<<"]}";
    }
private:
    using Srv=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_SHADER_RESOURCE_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    using Dsv=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_DEPTH_STENCIL_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    static void STDMETHODCALLTYPE on_srv(ID3D12Device* device,ID3D12Resource* resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle){
        auto& self=get();self.m_srv.load()(device,resource,desc,handle);
        try{auto v=metadata(device,resource,handle,Kind::srv);v.explicit_view=desc!=nullptr;
            if(desc){v.format=desc->Format;v.view_dimension=desc->ViewDimension;
                if(desc->ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2D){v.mip_slice=desc->Texture2D.MostDetailedMip;v.plane_slice=desc->Texture2D.PlaneSlice;v.array_size=1;}
                else if(desc->ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2DARRAY){v.mip_slice=desc->Texture2DArray.MostDetailedMip;v.plane_slice=desc->Texture2DArray.PlaneSlice;v.first_array=desc->Texture2DArray.FirstArraySlice;v.array_size=desc->Texture2DArray.ArraySize;}}
            self.store(v);
        }catch(...){++self.m_errors;self.invalidate({Kind::srv,handle.ptr});}
    }
    static void STDMETHODCALLTYPE on_dsv(ID3D12Device* device,ID3D12Resource* resource,const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle){
        auto& self=get();self.m_dsv.load()(device,resource,desc,handle);
        try{auto v=metadata(device,resource,handle,Kind::dsv);v.explicit_view=desc!=nullptr;
            if(desc){v.format=desc->Format;v.view_dimension=desc->ViewDimension;v.view_flags=desc->Flags;
                if(desc->ViewDimension==D3D12_DSV_DIMENSION_TEXTURE2D){v.mip_slice=desc->Texture2D.MipSlice;v.array_size=1;}
                else if(desc->ViewDimension==D3D12_DSV_DIMENSION_TEXTURE2DARRAY){v.mip_slice=desc->Texture2DArray.MipSlice;v.first_array=desc->Texture2DArray.FirstArraySlice;v.array_size=desc->Texture2DArray.ArraySize;}}
            self.store(v);
        }catch(...){++self.m_errors;self.invalidate({Kind::dsv,handle.ptr});}
    }
    static View metadata(ID3D12Device* device,ID3D12Resource* resource,D3D12_CPU_DESCRIPTOR_HANDLE handle,Kind kind){
        View v;v.kind=kind;v.handle=handle.ptr;if(!resource)return v;
        // Actual live CreateSRV/CreateDSV arguments exclusively authorize these queries.
        const auto d=resource->GetDesc();v.resource=reinterpret_cast<uintptr_t>(resource);v.identity=GpuObjectIdentity::get(resource);v.device=GpuObjectIdentity::get(device);
        v.width=d.Width;v.height=d.Height;v.storage=v.format=d.Format;v.dimension=d.Dimension;v.flags=d.Flags;v.mips=d.MipLevels;v.arrays=d.DepthOrArraySize;v.samples=d.SampleDesc.Count;
        D3D12_FEATURE_DATA_FORMAT_INFO info{d.Format,0};if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&info,sizeof(info))))v.planes=info.PlaneCount;
        v.subresources=d.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER?1:v.mips*v.planes*(d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1:v.arrays);
        v.known=v.identity && v.device && v.subresources;return v;
    }
    using Key=std::pair<Kind,uintptr_t>;
    void store(View value){if(!value.handle)return;std::lock_guard lock{m_mutex};value.serial=++m_serial;
        const Key key{value.kind,value.handle};m_views[key]=value;auto& fifo=m_fifo[size_t(value.kind)];fifo.emplace_back(key,value.serial);
        if(fifo.size()>CAPACITY){const auto [old,serial]=fifo.front();fifo.pop_front();auto found=m_views.find(old);if(found!=m_views.end() && found->second.serial==serial){m_views.erase(found);++m_evictions;}}
    }
    void invalidate(Key key){std::lock_guard lock{m_mutex};m_views.erase(key);}
    std::atomic<Srv> m_srv{};std::atomic<Dsv> m_dsv{};std::atomic_bool m_installed{};
    std::mutex m_install;std::array<void*,2> m_addresses{};
    mutable std::mutex m_mutex;std::map<Key,View> m_views;std::array<std::deque<std::pair<Key,uint64_t>>,2> m_fifo;uint64_t m_serial{},m_evictions{};std::atomic<uint64_t> m_errors{};
};
