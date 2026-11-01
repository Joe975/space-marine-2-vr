#pragma once
#include "GpuTestHost.h"
#include "NativeOrdinaryTextureCreation.h"
#include "GpuObjectIdentity.h"
#include <sstream>
#include <memory>
using Creation=NativeOrdinaryTextureCreation;
template<class T>void put(uintptr_t pointer,size_t offset,const T& value){std::memcpy(reinterpret_cast<void*>(pointer+offset),&value,sizeof(value));}
struct Fixture {
    Host& host;native_texture_contract::Snapshot contract;std::array<NativeRenderProbe::Output,9> outputs{};
    Creation::Origin origin{57,23,19,31,7,1,0x1000,0x2000,0x3000};
    struct Native {std::array<uint8_t,0x480> bytes{};ComPtr<ID3D12Resource> resource;ComPtr<ID3D12Heap> heap;};
    std::array<uintptr_t,26> vtable{};std::array<uint8_t,0x480> source{};ComPtr<ID3D12Resource> source_resource;
    std::array<uint8_t,0x22e0> pool{};std::array<uintptr_t,8> pool_buckets{};std::array<uintptr_t,3> pool_node{};
    std::vector<std::unique_ptr<Native>> created;std::vector<std::string> names;unsigned factory_calls{},retain_calls{},pre_calls{},init_calls{},fault{};bool current{true};Creation* reenter{};
    explicit Fixture(Host& h):host(h){
        contract.module=0x140000000;contract.factory=0x123000;contract.factory_create=contract.module+0x2119040;contract.complete=true;contract.selected_id=0xc5;contract.bank_variant=0;
        const auto native=reinterpret_cast<uintptr_t>(source.data());contract.wrappers[1].address=native;contract.wrappers[1].vtable=reinterpret_cast<uintptr_t>(vtable.data());contract.wrappers[1].methods[2]=contract.module+0x2113cc0;contract.wrappers[1].methods[3]=contract.module+0x264b7b0;
        contract.pool=reinterpret_cast<uintptr_t>(pool.data());contract.record_count=2;contract.pool_mode=2;contract.bank_count=1;
        contract.banks=0x7f00000;contract.records=0x4f80000; // POD addresses only: creation never reads them.
        pool_node={0,native,1};pool_buckets[native_pool_membership::hash(native)%pool_buckets.size()]=reinterpret_cast<uintptr_t>(pool_node.data());
        put(contract.pool,0x22d0,native_pool_membership::Header{1,int32_t(pool_buckets.size()),reinterpret_cast<uintptr_t>(pool_buckets.data())});
        for(size_t i=0;i<2;++i){auto& target=contract.targets[i];target.id=uint32_t(0xc4+i);target.readable=true;target.definition[0]=0x20000b0;target.initialization_words={{8,4,1,1,38,1}};target.wrappers={i?native:uintptr_t(0x567000)};}
        contract.targets[0].record=0;contract.targets[1].record=1;
        auto desc=description(8,4);D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;check(host.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&source_resource)));
        outputs[1].resource=reinterpret_cast<uintptr_t>(source_resource.Get());outputs[1].status=NativeRenderProbe::OutputStatus::observed;
        RenderTargetProbe::View view;view.known=true;view.resource=outputs[1].resource;view.identity=GpuObjectIdentity::get(source_resource.Get());view.width=8;view.height=4;view.storage=DXGI_FORMAT_R16G16B16A16_TYPELESS;view.format=DXGI_FORMAT_R16G16B16A16_FLOAT;view.flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;view.mips=view.depth_or_array=view.samples=view.planes=view.subresources=1;outputs[1].descriptors.views={view};
    }
    static D3D12_RESOURCE_DESC description(uint32_t width,uint32_t height){D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;desc.Format=DXGI_FORMAT_R16G16B16A16_TYPELESS;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;return desc;}
    Creation::Api api(){Creation::Api api;api.vtable=contract.wrappers[1].vtable;api.factory_method=contract.factory_create;api.preinitialize_method=contract.wrappers[1].methods[2];api.initialize_method=contract.wrappers[1].methods[3];
        api.create=[this](uintptr_t factory,const Creation::Name* name,uint64_t flags,uint32_t one,uintptr_t zero){++factory_calls;require(factory==contract.factory && name && name->padding==0 && name->length==std::strlen(name->text) && flags==0x20000b0 && one==1 && !zero,"actual factory ABI receives exact native string/flags and trailing native arguments");names.emplace_back(name->text);
            if(reenter)reenter->consider(origin,contract,outputs,[this]{return current;});
            if(fault==1)return uintptr_t(0);if(fault==2)return reinterpret_cast<uintptr_t>(source.data());if(fault==8)throw std::runtime_error("native fixture factory");if(fault==9 && !created.empty())return reinterpret_cast<uintptr_t>(created[0]->bytes.data());
            auto native=std::make_unique<Native>();const auto pointer=reinterpret_cast<uintptr_t>(native->bytes.data());put(pointer,0,fault==4?uintptr_t(1):contract.wrappers[1].vtable);put(pointer,0x98,flags);created.push_back(std::move(native));if(fault==11)current=false;return pointer;};
        api.retain=[this](uintptr_t reference){++retain_calls;const auto result=InterlockedIncrement(reinterpret_cast<LONG*>(reference));return int32_t(result)+(fault==3?1:0);};
        api.preinitialize=[this](uintptr_t native,uintptr_t heap,uintptr_t offset){++pre_calls;require(native && !heap && !offset,"native preinitializer receives owned wrapper with no borrowed heap/offset");};
        api.initialize=[this](uintptr_t native,uint32_t width,uint32_t height,uint32_t depth,uint32_t format,uint32_t one,uint32_t mips){++init_calls;require(width==8 && height==4 && depth==1 && format==38 && one==1 && mips==1,"all six native initializer arguments retain their actual order");
            auto& object=*created.back();auto desc=description(width+(fault==6),height);D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            if(fault==7){D3D12_HEAP_DESC allocation{};allocation.Properties=heap;allocation.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;allocation.SizeInBytes=host.device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;check(host.device->CreateHeap(&allocation,IID_PPV_ARGS(&object.heap)));check(host.device->CreatePlacedResource(object.heap.Get(),0,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&object.resource)));}
            else if(fault==12)object.resource=source_resource;
            else check(host.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&object.resource)));
            put(native,0x278,reinterpret_cast<uintptr_t>(object.resource.Get()));if(fault!=5)put(native,0x98,uint64_t(0x20000b0)|(uint64_t(1)<<50));if(fault==10)current=false;
            if(fault==13){pool_buckets.fill(0);pool_node={0,native,0};pool_buckets[native_pool_membership::hash(native)%pool_buckets.size()]=reinterpret_cast<uintptr_t>(pool_node.data());}};
        return api;
    }
    void run(Creation& owner){owner.consider(origin,contract,outputs,[this]{return current;});}
};
