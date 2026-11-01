#include "GpuTestHost.h"
#include "RenderTargetProbe.h"

int main() {
    try {
        Host host;auto& probe=*new RenderTargetProbe; // Process-wide hook fixture lifetime.
        bool binding_known=false;uint64_t binding_events{};std::vector<uintptr_t> bound_resources;
        probe.observe_binding=[&](uintptr_t,std::span<const uintptr_t> values,bool known) {++binding_events;binding_known=known;bound_resources.assign(values.begin(),values.end());};
        const auto texture=host.color(.2f,.4f,.6f,8,DXGI_FORMAT_R8G8B8A8_UNORM,4);
        auto desc=texture->GetDesc();desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> raw;check(host.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&raw)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
        ComPtr<ID3D12DescriptorHeap> descriptors;check(host.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&descriptors)));
        const auto first=descriptors->GetCPUDescriptorHandleForHeapStart();const auto stride=host.device->GetDescriptorHandleIncrementSize(hd.Type);
        const D3D12_CPU_DESCRIPTOR_HANDLE second{first.ptr+stride};
        D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        host.device->CreateRenderTargetView(raw.Get(),&view,first);probe.created(raw.Get(),&view,first);
        view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        host.device->CreateRenderTargetView(raw.Get(),&view,second);probe.created(raw.Get(),&view,second);
        auto observations=probe.views(reinterpret_cast<uintptr_t>(raw.Get()));
        require(observations[0].resource_dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && observations[0].flags==desc.Flags &&
                observations[0].mips==1 && observations[0].planes==1 && observations[0].samples==1 && observations[0].subresources==1,
                "actual resource flags, dimensions and queried plane count are copied during native resource lifetime");
        auto observed=probe.observe(reinterpret_cast<uintptr_t>(raw.Get()));
        require(observed.epoch && observed.serial==observations.back().created && observed.views.size()==2,"resource observation atomically includes cache clock and descriptors");
        require(observations.size()==2 && observations[0].storage==DXGI_FORMAT_R8G8B8A8_TYPELESS && observations[0].format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && observations[1].format==DXGI_FORMAT_R8G8B8A8_UNORM,"typeless storage preserves each actual typed RTV observation");
        probe.bound(123,2,&first,true,stride);observations=probe.views(reinterpret_cast<uintptr_t>(raw.Get()));
        require(observations[0].bound && observations[0].bound==observations[1].bound && observations[1].command_list==123,"contiguous native RTV binding resolves both descriptors");
        require(binding_known && bound_resources.size()==2,"every actually observed descriptor establishes complete binding evidence");
        require(bound_resources[0]==GpuObjectIdentity::get(raw.Get()) && observations[0].identity==GpuObjectIdentity::get(raw.Get()) &&
            probe.canonical_resource(first.ptr)==GpuObjectIdentity::get(raw.Get()),"native RTV binding and resource lookup preserve canonical COM identity separately from raw interface address");
        const D3D12_CPU_DESCRIPTOR_HANDLE handles[]{second,first};probe.bound(456,2,handles,false,0);
        require(probe.views(reinterpret_cast<uintptr_t>(raw.Get()))[0].command_list==456,"noncontiguous RTV binding resolves descriptor handles");
        probe.created(texture.Get(),nullptr,first);observations=probe.views(reinterpret_cast<uintptr_t>(raw.Get()));
        require(observations.size()==1 && observations[0].handle==second.ptr,"descriptor reuse cannot attribute old resource to new owner");
        require(probe.views(reinterpret_cast<uintptr_t>(texture.Get()))[0].format==DXGI_FORMAT_R8G8B8A8_UNORM,"null description uses typed resource format");
        probe.created(nullptr,nullptr,second);require(probe.views(reinterpret_cast<uintptr_t>(raw.Get())).empty(),"null resource descriptor clears previous owner");
        probe.bound(123,1,&second,true,stride);require(!binding_known,"a null resource without a valid view description is not proven unbound");
        probe.created(nullptr,&view,second);probe.bound(123,1,&second,true,stride);
        require(binding_known && bound_resources==std::vector<uintptr_t>{0},"an explicitly observed null RTV is known to have no target");
        const D3D12_CPU_DESCRIPTOR_HANDLE unknown{123456789};probe.bound(123,1,&unknown,true,stride);
        require(!binding_known,"unobserved descriptor cannot masquerade as known unbound");
        probe.bound(123,0,nullptr,true,0);require(binding_known && bound_resources.empty(),"explicit zero-target binding is known unbound");
        const auto prior_events=binding_events;probe.bound(123,9,&first,true,stride);
        require(binding_events==prior_events+1 && !binding_known,"malformed binding invalidates prior evidence instead of silently retaining it");
        const D3D12_CPU_DESCRIPTOR_HANDLE wrapped{UINTPTR_MAX};probe.bound(123,2,&wrapped,true,stride);
        require(!binding_known,"wrapped descriptor binding cannot leave trusted prior state");
        const auto prior_epoch=observed.epoch;
        // 31 Oct 19:00: a swap-chain reset keeps the descriptors of everything but the back buffers, so
        // a view made before it binds as known after it. A view made again replaces its old entry.
        probe.device_reset();
        require(probe.observe(0).epoch>prior_epoch && !probe.views(reinterpret_cast<uintptr_t>(texture.Get())).empty(),"a swap-chain reset changes the cache epoch and keeps the views");
        probe.bound(123,1,&first,true,stride);require(binding_known && bound_resources[0]==GpuObjectIdentity::get(texture.Get()),"a view made before a swap-chain reset still binds as known after it");
        probe.created(raw.Get(),&view,first);require(probe.canonical_resource(first.ptr)==GpuObjectIdentity::get(raw.Get()),"a view made again after the reset replaces the old attribution");
        const auto kept_epoch=probe.observe(0).epoch;
        probe.clear();require(probe.views(reinterpret_cast<uintptr_t>(texture.Get())).empty() && probe.observe(0).epoch>kept_epoch,"a probe fault clears evidence and changes cache epoch");
        auto array_desc=desc;array_desc.MipLevels=3;array_desc.DepthOrArraySize=2;
        ComPtr<ID3D12Resource> array;check(host.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&array_desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&array)));
        probe.created(array.Get(),nullptr,first);const auto array_address=reinterpret_cast<uintptr_t>(array.Get());
        auto array_receipt=probe.observe(array_address);array.Reset();
        require(array_receipt.views[0].subresources==6 && probe.observe(array_address).views[0].subresources==6,
                "mip/array subresource count survives resource release as historical POD without COM access");
        probe.clear();
        for(size_t i=1;i<=RenderTargetProbe::CAPACITY+3;++i) probe.created(texture.Get(),nullptr,{i});
        observations=probe.views(reinterpret_cast<uintptr_t>(texture.Get()));
        require(observations.size()==RenderTargetProbe::CAPACITY && std::ranges::none_of(observations,[](const auto& v){return v.handle<=3;}),"bounded descriptor cache evicts oldest creation");
        for(uintptr_t handle=1;handle<=3;++handle) {
            require(!probe.resource(handle) && !probe.canonical_resource(handle),"evicted handle cannot resolve the replacement slot through a stale index");
            const D3D12_CPU_DESCRIPTOR_HANDLE missing{handle};probe.bound(123,1,&missing,false,0);
            require(!binding_known,"binding an evicted handle invalidates ownership evidence");
        }
        for(uintptr_t handle=4;handle<=RenderTargetProbe::CAPACITY+3;++handle)
            require(probe.resource(handle)==reinterpret_cast<uintptr_t>(texture.Get()),"every surviving handle resolves after vector growth and repeated eviction");
        probe.created(raw.Get(),&view,{1});
        require(!probe.resource(4) && probe.resource(1)==reinterpret_cast<uintptr_t>(raw.Get()),"reinserted evicted handle gets its own current owner and evicts the oldest survivor");
        probe.created(raw.Get(),&view,{5});
        require(probe.resource(5)==reinterpret_cast<uintptr_t>(raw.Get()) && probe.status().cached==RenderTargetProbe::CAPACITY,"reuse of a full-cache handle updates in place without eviction");
        // Snapshot all sources before overwriting any destination, including
        // when the descriptor index already contains every destination.
        const D3D12_CPU_DESCRIPTOR_HANDLE overlap_from[]{D3D12_CPU_DESCRIPTOR_HANDLE{1},D3D12_CPU_DESCRIPTOR_HANDLE{6}};
        const D3D12_CPU_DESCRIPTOR_HANDLE overlap_to[]{D3D12_CPU_DESCRIPTOR_HANDLE{6},D3D12_CPU_DESCRIPTOR_HANDLE{1}};
        probe.copied(2,overlap_to,nullptr,2,overlap_from,nullptr,1);
        require(probe.resource(6)==reinterpret_cast<uintptr_t>(raw.Get()) && probe.resource(1)==reinterpret_cast<uintptr_t>(texture.Get()),"overlapping indexed copies preserve the source snapshot rather than feeding back overwritten owners");
        probe.clear();
        require(!probe.resource(1) && !probe.resource(6) && !probe.canonical_resource(RenderTargetProbe::CAPACITY+3),"clear invalidates the entire descriptor index");
        probe.created(raw.Get(),&view,{6});
        require(probe.resource(6)==reinterpret_cast<uintptr_t>(raw.Get()) && !probe.resource(1),"repopulation after clear cannot expose old slot indices");
        probe.clear();require(probe.observe_creation(host.device.Get()),"observe actual descriptor creation before late bind-hook setup");
        host.device->CreateRenderTargetView(raw.Get(),&view,first);
        require(probe.views(reinterpret_cast<uintptr_t>(raw.Get())).size()==1,"native descriptor creation is preserved before the late render observer exists");
        require(probe.install(host.device.Get()),"late D3D12 binding adopts the same early creation hooks without clearing evidence");
        require(probe.views(reinterpret_cast<uintptr_t>(raw.Get())).size()==1,"late observer retains the original early descriptor receipt");
        host.device->CreateRenderTargetView(raw.Get(),&view,first);
        observations=probe.views(reinterpret_cast<uintptr_t>(raw.Get()));
        require(observations.size()==1 && observations[0].format==DXGI_FORMAT_R8G8B8A8_UNORM && !observations[0].bound,"real native CreateRenderTargetView call reaches recorder");
        host.begin();host.commands->OMSetRenderTargets(1,&first,TRUE,nullptr);
        require(binding_known && probe.observes_binding(reinterpret_cast<uintptr_t>(host.commands.Get())),"actual RTV binding implementation matches the installed hook");
        observations=probe.views(reinterpret_cast<uintptr_t>(raw.Get()));
        require(observations[0].bound && observations[0].command_list==reinterpret_cast<uintptr_t>(host.commands.Get()),"real native OMSetRenderTargets call records the created descriptor");
        // Probe must outlive its process-wide hooks. Exit immediately after validation.
        host.commands->Close();
        D3D12_DESCRIPTOR_HEAP_DESC copy_desc{};copy_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;copy_desc.NumDescriptors=4;
        ComPtr<ID3D12DescriptorHeap> copied;check(host.device->CreateDescriptorHeap(&copy_desc,IID_PPV_ARGS(&copied)));
        const auto destination=copied->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE d1{destination.ptr+stride},d2{destination.ptr+2*stride},d3{destination.ptr+3*stride};
        host.device->CopyDescriptorsSimple(1,destination,first,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        require(probe.resource(destination.ptr)==reinterpret_cast<uintptr_t>(raw.Get()),"actual simple descriptor copy preserves RTV identity");
        require(probe.canonical_resource(destination.ptr)==GpuObjectIdentity::get(raw.Get()),"actual descriptor copy preserves canonical identity for GPU attribution");
        host.begin();host.commands->OMSetRenderTargets(1,&destination,TRUE,nullptr);host.commands->Close();
        observations=probe.views(reinterpret_cast<uintptr_t>(raw.Get()));
        require(std::ranges::any_of(observations,[&](const auto& v){return v.handle==destination.ptr && v.bound && v.format==DXGI_FORMAT_R8G8B8A8_UNORM;}),"copied RTV binding retains typed view attribution");
        host.device->CreateRenderTargetView(texture.Get(),nullptr,first);
        require(probe.resource(destination.ptr)==reinterpret_cast<uintptr_t>(raw.Get()),"copied descriptor keeps its own resource when its source slot is reused");
        host.device->CreateRenderTargetView(raw.Get(),&view,first);
        host.device->CreateRenderTargetView(texture.Get(),nullptr,second);
        const D3D12_CPU_DESCRIPTOR_HANDLE source_ranges[]{first,second};const UINT two=2;
        host.device->CopyDescriptors(1,&d1,&two,2,source_ranges,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        require(probe.resource(d1.ptr)==reinterpret_cast<uintptr_t>(raw.Get()) && probe.resource(d2.ptr)==reinterpret_cast<uintptr_t>(texture.Get()),"range copy flattens null-size source ranges into a contiguous destination");
        const D3D12_CPU_DESCRIPTOR_HANDLE destination_ranges[]{d3,destination};
        host.device->CopyDescriptors(2,destination_ranges,nullptr,1,&d1,&two,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        require(probe.resource(d3.ptr)==reinterpret_cast<uintptr_t>(raw.Get()) && probe.resource(destination.ptr)==reinterpret_cast<uintptr_t>(texture.Get()),"range copy maps a contiguous source into disjoint null-size destinations");
        // A native descriptor can predate observation. Its unknown source must
        // invalidate a destination that was previously attributed to our target.
        probe.clear();probe.created(raw.Get(),&view,d3);
        host.device->CopyDescriptorsSimple(1,d3,second,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        require(probe.resource(d3.ptr)==0,"unknown copied source invalidates stale destination attribution");
        probe.bound(123,1,&d3,true,stride);require(!binding_known,"an unknown copied source propagates binding uncertainty");
        probe.created(raw.Get(),&view,d3);const UINT excessive=UINT(RenderTargetProbe::CAPACITY+1);
        probe.copied(1,&d3,&excessive,1,&first,&excessive,stride);
        require(probe.views(reinterpret_cast<uintptr_t>(raw.Get())).empty(),"oversized copy cannot leave trusted stale descriptor evidence");
        probe.created(raw.Get(),&view,d3);const D3D12_CPU_DESCRIPTOR_HANDLE overflow{UINTPTR_MAX};
        probe.copied(1,&overflow,&two,1,&first,&two,stride);
        require(probe.resource(d3.ptr)==0,"wrapped descriptor range invalidates evidence");
        std::cout<<"Render target probe tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
