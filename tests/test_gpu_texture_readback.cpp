#include "GpuTestHost.h"
#include "GpuTextureReadback.h"

int main() {
    try {
        Host host;
        const auto red=host.color(1,0,0,7,DXGI_FORMAT_R8G8B8A8_UNORM,3);
        const auto blue=host.color(0,0,1,9,DXGI_FORMAT_B8G8R8A8_UNORM,5);
        const std::array<GpuTextureReadback::Source,2> sources{{{red.Get(),D3D12_RESOURCE_STATE_COMMON},{blue.Get(),D3D12_RESOURCE_STATE_COMMON}}};
        std::vector<GpuTextureReadback::Pixels> pixels;
        check(GpuTextureReadback::capture(host.queue.Get(),sources,pixels));
        require(pixels.size()==2 && pixels[0].width==7 && pixels[0].height==3 && pixels[1].width==9 && pixels[1].height==5,"read back distinct GPU textures and dimensions in one batch");
        for(size_t i=0;i<pixels.size();++i) for(size_t p=0;p<pixels[i].rgb.size();p+=3) {
            require(pixels[i].rgb[p]==(i?0:255) && pixels[i].rgb[p+1]==0 && pixels[i].rgb[p+2]==(i?255:0),"real GPU readback strips padded rows and decodes RGBA/BGRA independently");
        }
        const auto typeless=[&](ID3D12Resource* source,DXGI_FORMAT format) {
            auto desc=source->GetDesc();desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
            ComPtr<ID3D12Resource> texture;check(host.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)));
            host.begin();host.commands->CopyResource(texture.Get(),source);host.execute();return texture;
        };
        const auto raw_red=typeless(red.Get(),DXGI_FORMAT_R8G8B8A8_TYPELESS),raw_blue=typeless(blue.Get(),DXGI_FORMAT_B8G8R8A8_TYPELESS);
        const std::array<GpuTextureReadback::Source,2> raw{{{raw_red.Get(),D3D12_RESOURCE_STATE_COMMON},{raw_blue.Get(),D3D12_RESOURCE_STATE_COMMON}}};
        check(GpuTextureReadback::capture(host.queue.Get(),raw,pixels));
        for(size_t i=0;i<pixels.size();++i) for(size_t p=0;p<pixels[i].rgb.size();p+=3)
            require(pixels[i].rgb[p]==(i?0:255) && pixels[i].rgb[p+1]==0 && pixels[i].rgb[p+2]==(i?255:0),"native typeless RGBA/BGRA storage copies through compatible UNORM footprints");
        host.begin();auto before=barrier(red.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&before);host.execute();
        const std::array<GpuTextureReadback::Source,1> target{{{red.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET}}};
        check(GpuTextureReadback::capture(host.queue.Get(),target,pixels));
        host.begin();auto after=barrier(red.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&after);host.execute();
        const auto half=host.color(.25f,.5f,1,3,DXGI_FORMAT_R16G16B16A16_FLOAT,2);
        const std::array<GpuTextureReadback::Source,1> hdr{{{half.Get(),D3D12_RESOURCE_STATE_COMMON}}};
        check(GpuTextureReadback::capture(host.queue.Get(),hdr,pixels));
        require(pixels[0].rgb[0]==64 && pixels[0].rgb[1]==128 && pixels[0].rgb[2]==255,"half-float target gets explicit clamped-linear diagnostic conversion");
        require(GpuTextureReadback::half(0x3c00)==1 && GpuTextureReadback::half(0xbc00)==-1 && GpuTextureReadback::half(1)>0,"half decoder handles normal, signed and subnormal values");
        auto invalid=target;invalid[0].state=D3D12_RESOURCE_STATE_COPY_DEST;
        require(GpuTextureReadback::capture(host.queue.Get(),invalid,pixels)==E_INVALIDARG && pixels.empty(),"unverified source state cannot enqueue a copy");
        require(GpuTextureReadback::capture(nullptr,target,pixels)==E_INVALIDARG,"null queue rejected");
        invalid[0].texture=nullptr;require(GpuTextureReadback::capture(host.queue.Get(),invalid,pixels)==E_INVALIDARG,"null source rejected");
        const auto folder=std::filesystem::temp_directory_path()/("sm2vr-pip-readback-"+std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(folder);check(GpuTextureReadback::capture(host.queue.Get(),sources,pixels));
        require(GpuTextureReadback::save(folder/"red.ppm",pixels[0]),"save bounded evidence");
        require(!GpuTextureReadback::save(folder/"red.ppm",pixels[0]) && !GpuTextureReadback::save("relative.ppm",pixels[0]),"cannot overwrite existing evidence or use a relative path");
        std::ifstream file{folder/"red.ppm",std::ios::binary};std::string contents{std::istreambuf_iterator<char>{file},{}};
        require(contents==std::string{"P6\n7 3\n255\n"}+std::string{reinterpret_cast<char*>(pixels[0].rgb.data()),pixels[0].rgb.size()},"PPM contains exact GPU pixels");
        file.close();std::filesystem::remove(folder/"red.ppm");std::filesystem::remove(folder);
        std::cout<<"GPU texture readback tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
