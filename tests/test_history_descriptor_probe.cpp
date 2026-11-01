#include "GpuTestHost.h"
#include "HistoryDescriptorProbe.h"
#include <sstream>

int main(){try{
    Host host;auto& probe=HistoryDescriptorProbe::get();require(!probe.install(nullptr) && probe.install(host.device.Get()) && probe.install(host.device.Get()),"null refuses and actual live device installs idempotent SRV/DSV detours");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=32;d.Height=16;d.DepthOrArraySize=2;d.MipLevels=2;d.Format=DXGI_FORMAT_R24G8_TYPELESS;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    ComPtr<ID3D12Resource> texture;check(host.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> depth;check(host.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&depth)));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=UINT(HistoryDescriptorProbe::CAPACITY+1);
    ComPtr<ID3D12DescriptorHeap> shader;check(host.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&shader)));
    const auto dh=depth->GetCPUDescriptorHandleForHeapStart(),sh=shader->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY;dv.Flags=D3D12_DSV_FLAG_READ_ONLY_STENCIL;dv.Texture2DArray={1,0,2};
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2DArray.MostDetailedMip=1;sv.Texture2DArray.MipLevels=1;sv.Texture2DArray.ArraySize=2;
    const auto address=reinterpret_cast<uintptr_t>(texture.Get());
    host.device->CreateDepthStencilView(texture.Get(),&dv,dh);host.device->CreateShaderResourceView(texture.Get(),&sv,sh);
    auto observed=probe.observe(address);require(observed.installed && observed.errors==0 && observed.views.size()==2 && observed.serial==2,"both real native descriptor calls forwarded and captured once");
    for(const auto& v:observed.views){require(v.known && v.explicit_view && v.identity==GpuObjectIdentity::get(texture.Get()) && v.device==GpuObjectIdentity::get(host.device.Get()) && v.storage==DXGI_FORMAT_R24G8_TYPELESS && v.width==32 && v.height==16 && v.flags==D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL && v.mips==2 && v.arrays==2 && v.samples==1 && v.planes==2 && v.subresources==8 && v.mip_slice==1 && v.array_size==2,"live callback captures actual canonical identity/resource flags/planes/subresources and typed view range");
        require(v.kind==HistoryDescriptorProbe::Kind::srv?v.format==DXGI_FORMAT_R24_UNORM_X8_TYPELESS:v.format==DXGI_FORMAT_D24_UNORM_S8_UINT && v.view_flags==D3D12_DSV_FLAG_READ_ONLY_STENCIL,"SRV and DSV formats/flags remain distinct");}
    host.device->CreateShaderResourceView(nullptr,&sv,sh);require(probe.observe(address).views.size()==1,"null SRV reuse invalidates old resource attribution");
    host.device->CreateShaderResourceView(texture.Get(),&sv,sh);require(probe.observe(address).views.size()==2,"real descriptor slot reuse replaces creation evidence");
    const auto stride=host.device->GetDescriptorHandleIncrementSize(hd.Type);
    for(size_t i=0;i<=HistoryDescriptorProbe::CAPACITY;++i)host.device->CreateShaderResourceView(texture.Get(),&sv,{sh.ptr+i*stride});
    observed=probe.observe(address);require(observed.views.size()==HistoryDescriptorProbe::CAPACITY+1 && observed.evictions>0 && observed.errors==0 && std::ranges::count(observed.views,HistoryDescriptorProbe::Kind::dsv,&HistoryDescriptorProbe::View::kind)==1,"actual SRV creation stream is bounded with visible evictions and cannot evict unrelated DSV evidence");
    texture.Reset();const auto after=probe.observe(address);require(after.views==observed.views,"after actual resource release readers copy POD without querying cached COM pointers or owning a lease");
    std::ostringstream out;HistoryDescriptorProbe::write(out,after);require(out.str().find("\"creation_only\":true")!=std::string::npos && out.str().find("\"resource_leases\":false")!=std::string::npos && out.str().find("\"bound_writer_verified\":false")!=std::string::npos,"creation receipt never implies a copied/bound writer or completed GPU use");
    std::cout<<"history descriptors: actual SRV/DSV callbacks, format/subresources, reuse, eviction and no cached COM queries PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
