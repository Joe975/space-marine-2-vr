#include "GpuTestHost.h"
#include "EyePhoto.h"
#include <wincodec.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

// Through the real pieces on a software GPU: EyeTextureCapture records the copy, EyePhoto reads it
// back and writes a PNG, and WIC reads that PNG back.
namespace {
namespace fs=std::filesystem;
using Clock=EyePhoto::Clock;
using namespace std::chrono_literals;
const vr_photo::Stamp STAMP{2026,11,2,14,3,7};

// An eye as stereo sends it: the projection the overlays need.
EyeTextureCapture::Metadata tracked(uint64_t id) {
    auto m=meta(id,0,uint32_t(id));m.tracked_stereo=true;
    check(vr_projection::build({-1,1,-1,1},.1f,100,m.projection)?S_OK:E_FAIL);
    return m;
}
struct Png {uint32_t width{},height{};std::vector<uint8_t> bgr;};
Png read_png(const fs::path& path) {
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
    check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    check(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder));
    check(decoder->GetFrame(0,&frame));
    GUID container{};check(decoder->GetContainerFormat(&container));require(container==GUID_ContainerFormatPng,"the file is a PNG");
    WICPixelFormatGUID format{};check(frame->GetPixelFormat(&format));require(format==GUID_WICPixelFormat24bppBGR,"opaque 8-bit colour, no alpha channel");
    Png out;check(frame->GetSize(&out.width,&out.height));out.bgr.resize(size_t(out.width)*out.height*3);
    check(frame->CopyPixels(nullptr,out.width*3,static_cast<UINT>(out.bgr.size()),out.bgr.data()));
    return out;
}
// A Present loop: frames arrive, one at a time, until the picture ends.
std::optional<EyePhoto::Result> settle(EyePhoto& photo,Clock::duration limit=10s) {
    const auto end=Clock::now()+limit;
    while(Clock::now()<end) {if(auto r=photo.poll(Clock::now())) return r;std::this_thread::sleep_for(2ms);}
    return std::nullopt;
}
EyeTextureCapture::Tap armed_tap(EyePhoto& photo,ID3D12Device* device,ID3D12Resource* eye,float aspect=0) {
    const auto end=Clock::now()+5s;
    while(Clock::now()<end) {if(auto tap=photo.frame(device,eye,aspect,Clock::now())) return *tap;std::this_thread::sleep_for(1ms);}
    throw std::runtime_error{"the save thread never asked for the frame"};
}

// The stretch the live run showed: a 16:9 texture holding a near-square view saves at the view's shape.
void the_picture_has_the_views_shape(Host& host,const fs::path& folder) {
    const auto eye=host.color(0,0,1,160,DXGI_FORMAT_B8G8R8A8_UNORM,90);
    const float aspect=vr_photo::view_aspect(.770f,.708f); // Index, live: 0.92
    EyeTextureCapture capture;EyePhoto photo{folder/"shape"};
    photo.request(STAMP,Clock::now());
    require(!photo.frame(host.device.Get(),eye.Get(),aspect,Clock::now()),"starts");
    capture.tap(armed_tap(photo,host.device.Get(),eye.Get(),aspect));
    require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),meta(1,0,1))==S_OK,"captures");
    photo.tapped(*capture.take_tapped(),Clock::now());
    const auto result=settle(photo);
    require(result && result->outcome==vr_photo::Outcome::saved,"saves");
    require(result->eye_width==160 && result->eye_height==90,"the eye's own size is reported");
    const auto png=read_png(result->path);
    require(png.width==result->width && png.height==result->height,"the file has the size reported");
    require(std::abs(float(png.width)/png.height-aspect)<.02f,"the file has the view's shape, not the texture's");
    require(png.bgr[0]==255 && png.bgr[1]==0 && png.bgr[2]==0,"resampling keeps the colours");
    // A gradient survives the resample in order: the left is the left.
    std::vector<uint8_t> ramp(size_t(64)*16*3);for(uint32_t y=0;y<16;++y) for(uint32_t x=0;x<64;++x) for(uint32_t c=0;c<3;++c) ramp[(size_t(y)*64+x)*3+c]=uint8_t(x*4);
    const auto path=folder/"ramp.png";check(EyePhoto::save_png(path,ramp.data(),64,16,vr_photo::Size{32,32}));
    const auto scaled=read_png(path);require(scaled.width==32 && scaled.height==32 && scaled.bgr[0]<16 && scaled.bgr[31*3]>236 && scaled.bgr[(size_t(31)*32)*3]<16,"resampled the right way round");
}

void plain_eye_becomes_a_png(Host& host,const fs::path& folder) {
    for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) {
        const fs::path dir=folder/(format==DXGI_FORMAT_R8G8B8A8_UNORM?"rgba":"bgra");
        const auto eye=host.color(1,.5f,0,64,format,32);
        EyeTextureCapture capture;EyePhoto photo{dir};
        require(!photo.wanted() && !photo.poll(Clock::now()),"idle until a click");
        require(photo.request(STAMP,Clock::now()) && photo.wanted(),"a click wants a frame");
        require(!photo.frame(host.device.Get(),eye.Get(),0,Clock::now()),"the first frame starts the save and costs no copy");
        capture.tap(armed_tap(photo,host.device.Get(),eye.Get()));
        require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),meta(1,0,1))==S_OK,"the capture records the copy");
        const auto tapped=capture.take_tapped();
        require(tapped.has_value() && tapped->fence && tapped->value==1 && tapped->render_id==1,"the capture reports the copy's fence");
        photo.tapped(*tapped,Clock::now());
        require(!photo.wanted(),"nothing more to ask of the frames");
        const auto result=settle(photo);
        require(result && result->outcome==vr_photo::Outcome::saved,"the picture saves");
        require(result->path==dir/vr_photo::file_name(STAMP) && fs::exists(result->path) && !fs::exists(fs::path{result->path}+=L".part"),"the timestamped file, and no partial file");
        require(result->width==64 && result->height==32 && result->eye_width==64 && result->bytes==fs::file_size(result->path),"no shape given: the eye's full size");
        const auto png=read_png(result->path);
        require(png.width==64 && png.height==32,"full size");
        // host.color fills red 1, green .5, blue 0 in the eye's own format; 0.5 rounds either way.
        require(png.bgr[0]==0 && png.bgr[1]>=127 && png.bgr[1]<=128 && png.bgr[2]==255,"the colours stay themselves in either byte order");
        require(photo.request(STAMP,Clock::now()),"the next click starts again");
        require(!photo.frame(host.device.Get(),eye.Get(),0,Clock::now()),"second picture starts");
        capture.tap(armed_tap(photo,host.device.Get(),eye.Get()));
        require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),meta(2,0,2))==S_OK,"second capture");
        photo.tapped(*capture.take_tapped(),Clock::now());
        const auto second=settle(photo);
        require(second && second->outcome==vr_photo::Outcome::saved && second->path==dir/vr_photo::file_name(STAMP,2) && fs::exists(result->path),"the same second never overwrites a picture");
    }
}

// The frame never waits: the GPU is held back, and everything on the frame path still returns.
void the_frame_never_waits(Host& host,const fs::path& folder) {
    const auto eye=host.color(0,1,0,32,DXGI_FORMAT_R8G8B8A8_UNORM,32);
    EyeTextureCapture capture;EyePhoto photo{folder/"held"};
    ComPtr<ID3D12Fence> gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    photo.request(STAMP,Clock::now());photo.frame(host.device.Get(),eye.Get(),0,Clock::now());
    capture.tap(armed_tap(photo,host.device.Get(),eye.Get()));
    check(host.queue->Wait(gate.Get(),1));
    require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),meta(1,0,1))==S_OK,"the capture records and returns with the GPU stopped");
    photo.tapped(*capture.take_tapped(),Clock::now());
    // Rapid clicks while it's stuck: refused at once, and they queue nothing.
    for(int i=0;i<50;++i) require(!photo.request(STAMP,Clock::now()),"a click during a save takes nothing");
    std::this_thread::sleep_for(50ms);
    require(!photo.poll(Clock::now()) && !photo.poll(Clock::now()+1h),"a stopped GPU is not a timeout yet, and poll doesn't wait for it");
    require(!fs::exists(folder/"held"/vr_photo::file_name(STAMP)),"nothing is written before the copy ends");
    check(gate->Signal(1));
    const auto result=settle(photo);
    require(result && result->outcome==vr_photo::Outcome::saved,"the save ends when the GPU does");
    require(fs::directory_iterator(folder/"held")->path().filename()==vr_photo::file_name(STAMP) && std::distance(fs::directory_iterator(folder/"held"),fs::directory_iterator{})==1,"51 clicks, one picture");
}

void no_eye_image_is_refused(Host& host,const fs::path& folder) {
    const fs::path dir=folder/"refused";
    const auto t=Clock::now();
    {EyePhoto photo{dir};photo.request(STAMP,t);
     require(!photo.poll(t+vr_photo::EYE_WAIT-1ms),"still waiting");
     const auto result=photo.poll(t+vr_photo::EYE_WAIT);
     require(result && result->outcome==vr_photo::Outcome::no_eye_image && !photo.wanted(),"no frame ever came: refused");
     require(!photo.poll(t+1h) && photo.request(STAMP,t+1h),"reported once, and the next click is free");}
    {// The save started, then the eyes stopped (the menu came up).
     const auto eye=host.color(0,1,0,32,DXGI_FORMAT_R8G8B8A8_UNORM,32);EyePhoto photo{dir};
     const auto now=Clock::now();photo.request(STAMP,now);photo.frame(host.device.Get(),eye.Get(),0,now);armed_tap(photo,host.device.Get(),eye.Get());
     require(!photo.poll(now+vr_photo::EYE_WAIT-1ms),"armed and waiting");
     require(!photo.poll(now+vr_photo::EYE_WAIT),"the save thread is told to stop");
     require(!photo.wanted(),"no more frames wanted");
     const auto result=settle(photo);
     require(result && result->outcome==vr_photo::Outcome::no_eye_image,"refused, from the save thread");}
    require(!fs::exists(dir),"nothing was written");
}

void bad_cases_stay_quiet(Host& host,const fs::path& folder) {
    {// A folder that can't exist: a file is in the way.
     const auto blocked=folder/"blocked";{std::ofstream file{blocked};file<<"x";}
     const auto eye=host.color(0,1,0,32,DXGI_FORMAT_R8G8B8A8_UNORM,32);EyeTextureCapture capture;EyePhoto photo{blocked/"photos"};
     photo.request(STAMP,Clock::now());photo.frame(host.device.Get(),eye.Get(),0,Clock::now());
     capture.tap(armed_tap(photo,host.device.Get(),eye.Get()));
     require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),meta(1,0,1))==S_OK,"the frame carries on");
     photo.tapped(*capture.take_tapped(),Clock::now());
     const auto result=settle(photo);
     require(result && result->outcome==vr_photo::Outcome::no_folder,"a clear reason");
     require(photo.request(STAMP,Clock::now()),"the next click still works");}
    {// An eye format the mod doesn't take.
     const auto eye=host.color(0,1,0,32,DXGI_FORMAT_R16G16B16A16_FLOAT,32);EyePhoto photo{folder/"format"};
     photo.request(STAMP,Clock::now());photo.frame(host.device.Get(),eye.Get(),0,Clock::now());
     const auto result=photo.poll(Clock::now());
     require(result && result->outcome==vr_photo::Outcome::bad_format && !fs::exists(folder/"format"),"refused before any work");}
    {// A taken name that stays taken isn't overwritten, even when the encoder is called directly.
     const auto path=folder/"direct.png";const std::array<uint8_t,12> pixels{1,2,3,4,5,6,7,8,9,10,11,12};
     check(EyePhoto::save_png(path,pixels.data(),2,2));
     require(EyePhoto::save_png(path,pixels.data(),2,2)==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),"an existing picture stays");
     require(!fs::exists(fs::path{path}+=L".part"),"the partial file goes");
     const auto png=read_png(path);require(png.bgr.size()==12 && png.bgr[0]==1 && png.bgr[11]==12,"the first file is intact");
     require(FAILED(EyePhoto::save_png(folder/"nowhere"/"x.png",pixels.data(),2,2)),"a missing folder fails");
     require(EyePhoto::save_png(path,nullptr,2,2)==E_INVALIDARG,"no pixels");}
    require(!EyePhoto::default_folder().empty() && EyePhoto::default_folder().filename()==L"Space Marine 2 VR" && EyePhoto::default_folder().is_absolute(),"the default folder is under Pictures");
}

// The copy is one-shot and comes before anything the mod draws over the eye.
void the_tap_itself(Host& host) {
    const auto eye=host.color(1,1,1,16,DXGI_FORMAT_R8G8B8A8_UNORM,16);
    D3D12_RESOURCE_DESC desc=eye->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 size{};
    host.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&size);
    const auto readback=[&]{
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=size;buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;ComPtr<ID3D12Resource> out;
        check(host.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)));return out;};
    const auto bytes=[&](ID3D12Resource* buffer){void* mapped{};check(buffer->Map(0,nullptr,&mapped));std::vector<uint8_t> out(static_cast<const uint8_t*>(mapped),static_cast<const uint8_t*>(mapped)+size);D3D12_RANGE none{};buffer->Unmap(0,&none);return out;};
    EyeTextureCapture capture;
    {// Under a comfort vignette, the darkest overlay: the eye image darkens, the picture doesn't.
     const auto buffer=readback();capture.tap({buffer,footprint});
     auto m=tracked(1);m.comfort_vignette=1;
     require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),m)==S_OK,"capture with an overlay");
     const auto tapped=capture.take_tapped();require(tapped.has_value(),"tapped");wait(tapped->fence.Get(),tapped->value);capture.poll();
     const auto image=capture.latest(0);require(image && image->metadata.comfort_vignette==1,"the overlay was drawn on the eye");
     const auto pixels=bytes(buffer.Get());
     require(pixels[footprint.Offset]==255 && pixels[footprint.Offset+1]==255 && pixels[footprint.Offset+2]==255,"the picture holds the scene, not the overlay: corner stays white");}
    {// One tap, one copy.
     const auto buffer=readback();capture.tap({buffer,footprint});
     require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),meta(2,0,2))==S_OK && capture.take_tapped().has_value(),"first capture takes the tap");
     require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),meta(3,0,3))==S_OK && !capture.take_tapped(),"the next one doesn't");}
    {// An unused tap ends with the frame it was set for.
     const auto buffer=readback();capture.tap({buffer,footprint});
     require(!capture.take_tapped(),"not taken");
     require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),meta(4,0,4))==S_OK && !capture.take_tapped(),"so no later frame copies into it");}
    {// A size that isn't this eye's is left alone (a resolution change between click and copy).
     auto wrong=footprint;wrong.Footprint.Width/=2;const auto buffer=readback();capture.tap({buffer,wrong});
     require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),meta(5,0,5))==S_OK && !capture.take_tapped(),"refused quietly, the eye goes on");}
    {// A menu frame never goes into a picture.
     VrMenuPanel panel;panel.width_degrees=60;const auto buffer=readback();capture.tap({buffer,footprint});
     auto m=tracked(6);m.screen_menu=true;m.menu_panel=panel;
     require(capture.capture(host.device.Get(),host.queue.Get(),eye.Get(),m)==S_OK && !capture.take_tapped(),"menu frames are not tapped");}
}
}

int main() {
    const fs::path folder=fs::temp_directory_path()/("sm2vr-photo-test-"+std::to_string(GetCurrentProcessId()));
    int code=0;
    try {
        check(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
        fs::remove_all(folder);fs::create_directories(folder);
        Host host;
        plain_eye_becomes_a_png(host,folder);the_picture_has_the_views_shape(host,folder);the_frame_never_waits(host,folder);no_eye_image_is_refused(host,folder);bad_cases_stay_quiet(host,folder);the_tap_itself(host);
        std::cout<<"eye_photo tests passed\n";
    }catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';code=1;}
    std::error_code ignored;fs::remove_all(folder,ignored);
    return code;
}
