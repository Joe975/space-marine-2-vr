// Unattended headset-mirror check: what the SteamVR compositor shows in each eye. A background
// OpenVR client (never starts SteamVR, never submits, never reads input) copies the compositor's
// per-eye mirror textures (IVRCompositor::GetMirrorTextureD3D11) to a side-by-side PPM, downscaled,
// and prints per-eye statistics so a script can check the result without a person:
//   mean luminance per eye (0-255), the fraction of near-black pixels, and the mean absolute
//   difference between the eyes (0 means the same image in both eyes; stereo shows a small one).
// Works for OpenXR games too: the compositor mirrors whatever was submitted.
// Usage: openvr_mirror_capture <absolute output prefix> [count=1] [interval_ms=2000] [downscale=4]
//   writes <prefix>_<n>.ppm and prints one JSON line per capture.
// Stream: openvr_mirror_capture <absolute output prefix> stream <seconds> [downscale=8]
//   records every new mirror frame of both eyes for up to <seconds> (or until <prefix>.stop exists):
//   <prefix>.rgb holds the frames back to back (both eyes side by side, RGB, size in <prefix>.json),
//   <prefix>.times one line per frame: milliseconds since the start, local wall clock. A frame is new
//   when a sparse pixel sample differs from the last one written (the compositor repeats game frames).
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <openvr.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
struct Eye { unsigned width{},height{}; std::vector<uint8_t> rgb; }; // Downscaled RGB.
bool read_eye(ID3D11Device* device,ID3D11DeviceContext* context,vr::IVRCompositor* compositor,vr::EVREye which,unsigned downscale,Eye& out,std::string& error) {
    void* view{};
    if(compositor->GetMirrorTextureD3D11(which,device,&view)!=vr::VRCompositorError_None || !view) { error="GetMirrorTextureD3D11 failed"; return false; }
    auto* srv=static_cast<ID3D11ShaderResourceView*>(view);
    ComPtr<ID3D11Resource> resource;srv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;bool ok=SUCCEEDED(resource.As(&texture));
    D3D11_TEXTURE2D_DESC desc{};if(ok) texture->GetDesc(&desc);
    const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS;
    const bool rgba=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if(ok && !bgra && !rgba) { error="unsupported mirror format "+std::to_string(desc.Format); ok=false; }
    ComPtr<ID3D11Texture2D> staging;
    if(ok) {
        D3D11_TEXTURE2D_DESC copy=desc;copy.Usage=D3D11_USAGE_STAGING;copy.BindFlags=0;copy.CPUAccessFlags=D3D11_CPU_ACCESS_READ;copy.MiscFlags=0;copy.MipLevels=1;copy.ArraySize=1;
        ok=SUCCEEDED(device->CreateTexture2D(&copy,nullptr,&staging));
        if(ok) context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),0,nullptr);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(ok && SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) {
        out.width=desc.Width/downscale;out.height=desc.Height/downscale;out.rgb.assign(size_t(out.width)*out.height*3,0);
        for(unsigned y=0;y<out.height;++y) for(unsigned x=0;x<out.width;++x) {
            const auto* p=static_cast<const uint8_t*>(mapped.pData)+size_t(y*downscale)*mapped.RowPitch+size_t(x*downscale)*4;
            auto* q=&out.rgb[(size_t(y)*out.width+x)*3];q[0]=bgra?p[2]:p[0];q[1]=p[1];q[2]=bgra?p[0]:p[2];
        }
        context->Unmap(staging.Get(),0);
    } else if(ok) { error="staging map failed"; ok=false; }
    compositor->ReleaseMirrorTextureD3D11(view);
    return ok;
}
double luminance(const uint8_t* p) { return .2126*p[0]+.7152*p[1]+.0722*p[2]; }
// One eye's mirror view, opened once. Each frame is copied into a mip chain, reduced on the GPU and
// only the small level read back, so the capture doesn't pull full eye images over the bus.
struct StreamEye {
    void* view{};ComPtr<ID3D11Texture2D> texture,mips,staging;ComPtr<ID3D11ShaderResourceView> mip_view;
    D3D11_TEXTURE2D_DESC desc{};bool bgra{};unsigned level{},width{},height{};
    bool open(ID3D11Device* device,vr::IVRCompositor* compositor,vr::EVREye which,unsigned downscale) {
        if(compositor->GetMirrorTextureD3D11(which,device,&view)!=vr::VRCompositorError_None || !view) return false;
        ComPtr<ID3D11Resource> resource;static_cast<ID3D11ShaderResourceView*>(view)->GetResource(&resource);
        if(FAILED(resource.As(&texture))) return false;
        texture->GetDesc(&desc);
        bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const bool rgba=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS;
        if(!bgra && !rgba) return false;
        while((1u<<level)<downscale) ++level;
        width=(std::max)(1u,desc.Width>>level);height=(std::max)(1u,desc.Height>>level);
        D3D11_TEXTURE2D_DESC chain=desc;chain.Format=bgra?DXGI_FORMAT_B8G8R8A8_TYPELESS:DXGI_FORMAT_R8G8B8A8_TYPELESS;chain.MipLevels=level+1;chain.ArraySize=1;
        chain.SampleDesc={1,0};chain.Usage=D3D11_USAGE_DEFAULT;chain.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;chain.CPUAccessFlags=0;chain.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if(FAILED(device->CreateTexture2D(&chain,nullptr,&mips))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=level+1;
        if(FAILED(device->CreateShaderResourceView(mips.Get(),&srv,&mip_view))) return false;
        D3D11_TEXTURE2D_DESC copy=chain;copy.Width=width;copy.Height=height;copy.MipLevels=1;copy.Usage=D3D11_USAGE_STAGING;copy.BindFlags=0;copy.CPUAccessFlags=D3D11_CPU_ACCESS_READ;copy.MiscFlags=0;
        return SUCCEEDED(device->CreateTexture2D(&copy,nullptr,&staging));
    }
    bool read(ID3D11DeviceContext* context,uint8_t* out,unsigned stride) {
        context->CopySubresourceRegion(mips.Get(),0,0,0,0,texture.Get(),0,nullptr);
        if(level) context->GenerateMips(mip_view.Get());
        context->CopySubresourceRegion(staging.Get(),0,0,0,0,mips.Get(),level,nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            const auto* p=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch+size_t(x)*4;
            auto* q=out+size_t(y)*stride+size_t(x)*3;q[0]=bgra?p[2]:p[0];q[1]=p[1];q[2]=bgra?p[0]:p[2];
        }
        context->Unmap(staging.Get(),0);return true;
    }
};
int stream(const std::string& prefix,double seconds,unsigned downscale,ID3D11Device* device,ID3D11DeviceContext* context) {
    auto* compositor=vr::VRCompositor();StreamEye eyes[2];
    // The first copy of a freshly opened mirror reads black: open both, copy once, discard.
    if(!eyes[0].open(device,compositor,vr::Eye_Left,downscale) || !eyes[1].open(device,compositor,vr::Eye_Right,downscale)) { std::fprintf(stderr,"mirror textures unavailable\n"); return 1; }
    if(eyes[0].width!=eyes[1].width || eyes[0].height!=eyes[1].height) { std::fprintf(stderr,"eye size mismatch\n"); return 1; }
    const unsigned w=eyes[0].width,h=eyes[0].height,stride=w*2*3;downscale=1u<<eyes[0].level;
    std::vector<uint8_t> frame(size_t(stride)*h),last;
    for(auto& e:eyes) e.read(context,frame.data(),stride);
    std::ofstream rgb{prefix+".rgb",std::ios::binary},times{prefix+".times"};
    { std::ofstream info{prefix+".json"};info<<"{\"width\":"<<w*2<<",\"height\":"<<h<<",\"eye_width\":"<<w<<",\"downscale\":"<<downscale<<",\"format\":\"rgb24\"}\n"; }
    const auto stop=std::filesystem::path{prefix+".stop"};
    const auto start=std::chrono::steady_clock::now();size_t written{},polled{};
    auto next_check=start;
    while(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<seconds) {
        const auto now=std::chrono::steady_clock::now();
        if(now>=next_check) { if(std::filesystem::exists(stop)) break; next_check=now+std::chrono::milliseconds(500); }
        if(!eyes[0].read(context,frame.data(),stride) || !eyes[1].read(context,frame.data()+size_t(w)*3,stride)) { std::fprintf(stderr,"read failed\n"); return 1; }
        ++polled;
        bool changed=last.empty();
        for(size_t i=0;!changed && i<frame.size();i+=97) changed=frame[i]!=last[i];
        if(changed) {
            rgb.write(reinterpret_cast<const char*>(frame.data()),std::streamsize(frame.size()));last=frame;++written;
            SYSTEMTIME local{};GetLocalTime(&local);
            char clock[32];std::snprintf(clock,sizeof clock,"%02u:%02u:%02u.%03u",local.wHour,local.wMinute,local.wSecond,local.wMilliseconds);
            times<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<' '<<clock<<'\n';
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    for(auto& e:eyes) compositor->ReleaseMirrorTextureD3D11(e.view);
    std::printf("{\"frames\":%zu,\"polled\":%zu,\"width\":%u,\"height\":%u}\n",written,polled,w*2,h);
    return 0;
}
}
int main(int argc,char** argv) {
    const bool streaming=argc>=4 && std::string{argv[2]}=="stream";
    if(argc<2 || argc>5 || !std::filesystem::path{argv[1]}.is_absolute()) { std::fprintf(stderr,"usage: openvr_mirror_capture <absolute output prefix> [count] [interval_ms] [downscale]\n       openvr_mirror_capture <absolute output prefix> stream <seconds> [downscale]\n"); return 2; }
    if(streaming) {
        const double seconds=std::atof(argv[3]);const int downscale=argc>4?std::atoi(argv[4]):8;
        if(seconds<=0 || seconds>3600 || downscale<1 || downscale>16) { std::fprintf(stderr,"bounded arguments required\n"); return 2; }
        auto error=vr::VRInitError_None;
        if(!vr::VR_Init(&error,vr::VRApplication_Background) || error!=vr::VRInitError_None) { std::fprintf(stderr,"VR_Init(Background) failed: %d (is SteamVR running?)\n",int(error)); return 1; }
        int32_t adapter_index=-1;vr::VRSystem()->GetDXGIOutputInfo(&adapter_index);
        ComPtr<IDXGIFactory1> factory;ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        if(adapter_index>=0 && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) factory->EnumAdapters1(UINT(adapter_index),&adapter);
        int status=1;
        if(SUCCEEDED(D3D11CreateDevice(adapter.Get(),adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)))
            status=stream(argv[1],seconds,unsigned(downscale),device.Get(),context.Get());
        else std::fprintf(stderr,"D3D11 device failed\n");
        vr::VR_Shutdown();return status;
    }
    const int count=argc>2?std::atoi(argv[2]):1,interval=argc>3?std::atoi(argv[3]):2000,downscale=argc>4?std::atoi(argv[4]):4;
    if(count<1 || count>100 || interval<0 || interval>60000 || downscale<1 || downscale>16) { std::fprintf(stderr,"bounded arguments required\n"); return 2; }
    auto error=vr::VRInitError_None;
    if(!vr::VR_Init(&error,vr::VRApplication_Background) || error!=vr::VRInitError_None) { std::fprintf(stderr,"VR_Init(Background) failed: %d (is SteamVR running?)\n",int(error)); return 1; }
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    int status=0;
    // The mirror is shared from the HMD's adapter: create the device there, not on adapter 0.
    int32_t adapter_index=-1;vr::VRSystem()->GetDXGIOutputInfo(&adapter_index);
    ComPtr<IDXGIFactory1> factory;ComPtr<IDXGIAdapter1> adapter;
    if(adapter_index>=0 && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) factory->EnumAdapters1(UINT(adapter_index),&adapter);
    if(FAILED(D3D11CreateDevice(adapter.Get(),adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) { std::fprintf(stderr,"D3D11 device failed\n"); status=1; }
    std::fprintf(stderr,"adapter index %d\n",adapter_index);
    // The first copy of a freshly opened mirror texture reads black (22 Oct, whichever eye came first):
    // open and copy both eyes once and discard the result.
    if(!status){Eye warm;std::string ignored;read_eye(device.Get(),context.Get(),vr::VRCompositor(),vr::Eye_Left,downscale,warm,ignored);read_eye(device.Get(),context.Get(),vr::VRCompositor(),vr::Eye_Right,downscale,warm,ignored);}
    for(int n=0;n<count && !status;++n) {
        if(n) std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        Eye eyes[2];std::string why;
        // Odd captures read the right eye first, so a first-read artefact cannot pass for one eye being black.
        const int first=n%2,second=1-first;
        if(!read_eye(device.Get(),context.Get(),vr::VRCompositor(),first?vr::Eye_Right:vr::Eye_Left,downscale,eyes[first],why) ||
           !read_eye(device.Get(),context.Get(),vr::VRCompositor(),second?vr::Eye_Right:vr::Eye_Left,downscale,eyes[second],why) ||
           eyes[0].width!=eyes[1].width || eyes[0].height!=eyes[1].height) { std::printf("{\"capture\":%d,\"error\":\"%s\"}\n",n,why.empty()?"eye size mismatch":why.c_str()); status=1; break; }
        const auto w=eyes[0].width,h=eyes[0].height;double mean[2]{},dark[2]{},difference{};
        for(size_t i=0;i<size_t(w)*h;++i) {
            for(int e=0;e<2;++e){const auto l=luminance(&eyes[e].rgb[i*3]);mean[e]+=l;dark[e]+=l<8;}
            for(int c=0;c<3;++c) difference+=std::abs(int(eyes[0].rgb[i*3+c])-int(eyes[1].rgb[i*3+c]));
        }
        const double pixels=double(w)*h;
        const auto path=std::string{argv[1]}+"_"+std::to_string(n)+".ppm";
        std::ofstream file{path,std::ios::binary};file<<"P6\n"<<w*2<<' '<<h<<"\n255\n";
        for(unsigned y=0;y<h;++y) for(int e=0;e<2;++e) file.write(reinterpret_cast<const char*>(&eyes[e].rgb[size_t(y)*w*3]),std::streamsize(w*3));
        std::printf("{\"capture\":%d,\"file\":\"%s\",\"eye_width\":%u,\"eye_height\":%u,\"mean_luminance\":[%.2f,%.2f],\"dark_fraction\":[%.4f,%.4f],\"mean_abs_eye_difference\":%.3f}\n",
            n,std::filesystem::path{path}.filename().string().c_str(),w,h,mean[0]/pixels,mean[1]/pixels,dark[0]/pixels,dark[1]/pixels,difference/(pixels*3));
        std::fflush(stdout);
    }
    vr::VR_Shutdown();return status;
}
