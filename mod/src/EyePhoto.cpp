#include "EyePhoto.h"
#include <Windows.h>
#include <wincodec.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <condition_variable>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
bool bgra_order(DXGI_FORMAT format) {return format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;}
bool rgba_order(DXGI_FORMAT format) {return format==DXGI_FORMAT_R8G8B8A8_UNORM || format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;}
double ms(EyePhoto::Clock::duration d) {return std::chrono::duration<double,std::milli>(d).count();}
// Worker thread only; each save is one short-lived thread, so nothing else's COM state is touched.
struct Com {
    HRESULT hr;
    Com():hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED)) {}
    ~Com() {if(SUCCEEDED(hr)) CoUninitialize();}
};
HRESULT encode_png(const std::filesystem::path& path,const uint8_t* bgr,uint32_t width,uint32_t height,vr_photo::Size picture) {
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    auto hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));if(FAILED(hr)) return hr;
    hr=factory->CreateStream(&stream);if(FAILED(hr)) return hr;
    hr=stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE);if(FAILED(hr)) return hr;
    hr=factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);if(FAILED(hr)) return hr;
    hr=encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache);if(FAILED(hr)) return hr;
    hr=encoder->CreateNewFrame(&frame,nullptr);if(FAILED(hr)) return hr;
    hr=frame->Initialize(nullptr);if(FAILED(hr)) return hr;
    hr=frame->SetSize(picture.width,picture.height);if(FAILED(hr)) return hr;
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;
    hr=frame->SetPixelFormat(&format);if(FAILED(hr)) return hr;
    if(format!=GUID_WICPixelFormat24bppBGR) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    const UINT stride=width*3;
    if(picture==vr_photo::Size{width,height}) hr=frame->WritePixels(height,stride,stride*height,const_cast<BYTE*>(bgr));
    else {
        ComPtr<IWICBitmap> source;ComPtr<IWICBitmapScaler> scaler;
        hr=factory->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat24bppBGR,stride,stride*height,const_cast<BYTE*>(bgr),&source);if(FAILED(hr)) return hr;
        hr=factory->CreateBitmapScaler(&scaler);if(FAILED(hr)) return hr;
        hr=scaler->Initialize(source.Get(),picture.width,picture.height,WICBitmapInterpolationModeHighQualityCubic);if(FAILED(hr)) return hr;
        hr=frame->WriteSource(scaler.Get(),nullptr);
    }
    if(FAILED(hr)) return hr;
    hr=frame->Commit();if(FAILED(hr)) return hr;
    return encoder->Commit();
}
}

// Shared with the save thread, so a game that exits mid-save never waits for it.
struct EyePhoto::Job {
    std::mutex mutex;std::condition_variable wake;
    ComPtr<ID3D12Device> device;
    D3D12_RESOURCE_DESC desc{};
    std::filesystem::path folder;
    vr_photo::Stamp stamp;
    Clock::time_point clicked{};
    float view_aspect{};
    EyeTextureCapture::Tap tap;
    std::optional<EyeTextureCapture::Tapped> tapped;
    Clock::time_point tapped_at{};
    bool armed{},cancelled{},done{};
    Result result;
};

EyePhoto::~EyePhoto() {
    std::lock_guard lock{m_mutex};
    if(m_job) {std::lock_guard job{m_job->mutex};m_job->cancelled=true;m_job->wake.notify_all();}
}
bool EyePhoto::request(const vr_photo::Stamp& stamp,Clock::time_point now) {
    std::lock_guard lock{m_mutex};
    if(!m_flight.request(now)) return false;
    m_stamp=stamp;return true;
}
bool EyePhoto::wanted() const {
    std::lock_guard lock{m_mutex};
    return m_flight.stage()==vr_photo::Flight::Stage::wanted || m_flight.stage()==vr_photo::Flight::Stage::preparing || m_flight.stage()==vr_photo::Flight::Stage::armed;
}
std::optional<EyeTextureCapture::Tap> EyePhoto::frame(ID3D12Device* device,ID3D12Resource* eye,float view_aspect,Clock::time_point now) {
    std::lock_guard lock{m_mutex};
    using Stage=vr_photo::Flight::Stage;
    if(m_flight.stage()==Stage::wanted) {
        if(!device || !eye) return std::nullopt;
        const auto desc=eye->GetDesc();
        auto job=std::make_shared<Job>();
        job->device=device;job->desc=desc;job->folder=m_folder;job->stamp=m_stamp;job->clicked=m_flight.clicked();
        job->view_aspect=view_aspect;job->result.eye_width=static_cast<uint32_t>(desc.Width);job->result.eye_height=desc.Height;
        m_job=job;m_flight.preparing();
        if(!bgra_order(desc.Format) && !rgba_order(desc.Format)) {job->result.outcome=vr_photo::Outcome::bad_format;job->done=true;return std::nullopt;}
        try {std::thread{[job]{run(job);}}.detach();}
        catch(...) {job->result.outcome=vr_photo::Outcome::no_thread;job->done=true;}
        return std::nullopt;
    }
    if(m_flight.stage()==Stage::preparing && m_job) {
        std::lock_guard job{m_job->mutex};
        if(m_job->armed) m_flight.armed();
    }
    if(m_flight.stage()==Stage::armed && m_job) {
        std::lock_guard job{m_job->mutex};
        if(!m_job->cancelled) return m_job->tap;
    }
    return std::nullopt;
}
void EyePhoto::tapped(const EyeTextureCapture::Tapped& value,Clock::time_point now) {
    std::lock_guard lock{m_mutex};
    if(!m_job) return;
    std::lock_guard job{m_job->mutex};
    m_flight.copying();
    m_job->tapped=value;m_job->tapped_at=now;m_job->wake.notify_all();
}
std::optional<EyePhoto::Result> EyePhoto::poll(Clock::time_point now) {
    std::lock_guard lock{m_mutex};
    if(m_flight.idle()) return std::nullopt;
    if(m_job) {
        std::lock_guard job{m_job->mutex};
        if(m_job->done) {
            auto result=m_job->result;
            m_job.reset();m_flight.finish();return result;
        }
    }
    if(!m_flight.expired(now)) return std::nullopt;
    if(!m_job) {
        Result result;result.outcome=vr_photo::Outcome::no_eye_image;result.wait_ms=ms(now-m_flight.clicked());
        m_flight.finish();return result;
    }
    // The save thread ends itself and reports it at the next poll.
    std::lock_guard job{m_job->mutex};
    m_flight.finishing();m_job->cancelled=true;m_job->wake.notify_all();
    return std::nullopt;
}
std::filesystem::path EyePhoto::default_folder() {
    PWSTR pictures{};
    const auto hr=SHGetKnownFolderPath(FOLDERID_Pictures,KF_FLAG_CREATE,nullptr,&pictures);
    std::filesystem::path out;
    if(SUCCEEDED(hr) && pictures) out=std::filesystem::path{pictures}/L"Space Marine 2 VR";
    CoTaskMemFree(pictures);
    return out;
}
HRESULT EyePhoto::save_png(const std::filesystem::path& path,const uint8_t* bgr,uint32_t width,uint32_t height,vr_photo::Size picture) {
    if(!bgr || !width || !height) return E_INVALIDARG;
    if(!picture.width || !picture.height) picture={width,height};
    Com com;
    if(FAILED(com.hr) && com.hr!=RPC_E_CHANGED_MODE) return com.hr;
    auto partial=path;partial+=L".part";
    auto hr=encode_png(partial,bgr,width,height,picture);
    // Only a finished file gets the .png name; MoveFile refuses to replace one already there.
    if(SUCCEEDED(hr) && !MoveFileExW(partial.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)) hr=HRESULT_FROM_WIN32(GetLastError());
    if(FAILED(hr)) DeleteFileW(partial.c_str());
    return hr;
}
void EyePhoto::run(const std::shared_ptr<Job>& job) {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL); // Second to the game's threads on a busy CPU.
    auto& result=job->result;
    const auto finish=[&](vr_photo::Outcome outcome,HRESULT hr=S_OK) {
        std::lock_guard lock{job->mutex};
        result.outcome=outcome;result.hr=hr;job->done=true;
    };
    try {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 size{};
        job->device->GetCopyableFootprints(&job->desc,0,1,0,&footprint,nullptr,nullptr,&size);
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=size;buffer.Height=1;
        buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> readback;
        auto hr=job->device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback));
        if(FAILED(hr)) {finish(vr_photo::Outcome::gpu,hr);return;}
        std::optional<EyeTextureCapture::Tapped> tapped;
        {
            std::unique_lock lock{job->mutex};
            job->tap={readback,footprint};job->armed=true;
            job->wake.wait_for(lock,vr_photo::TAP_WAIT,[&]{return job->cancelled || job->tapped.has_value();});
            tapped=job->tapped;
            if(tapped) {result.wait_ms=ms(job->tapped_at-job->clicked);}
            else {result.wait_ms=ms(Clock::now()-job->clicked);}
        }
        if(!tapped) {finish(vr_photo::Outcome::no_eye_image);return;}
        const auto copy_begin=Clock::now();
        const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event) {readback.Detach();finish(vr_photo::Outcome::gpu,HRESULT_FROM_WIN32(GetLastError()));return;}
        hr=tapped->fence->SetEventOnCompletion(tapped->value,event);
        const bool copied=SUCCEEDED(hr) && WaitForSingleObject(event,static_cast<DWORD>(vr_photo::GPU_WAIT.count()))==WAIT_OBJECT_0 && tapped->fence->GetCompletedValue()!=UINT64_MAX;
        CloseHandle(event);
        if(!copied) {readback.Detach();finish(vr_photo::Outcome::gpu,FAILED(hr)?hr:HRESULT_FROM_WIN32(WAIT_TIMEOUT));return;} // The GPU may still write it: never freed.
        result.copy_ms=ms(Clock::now()-copy_begin);
        const auto encode_begin=Clock::now();
        void* mapped{};const D3D12_RANGE range{0,static_cast<SIZE_T>(size)};
        hr=readback->Map(0,&range,&mapped);
        if(FAILED(hr)) {finish(vr_photo::Outcome::gpu,hr);return;}
        std::vector<uint8_t> rows;
        const bool converted=vr_photo::bgr_rows(static_cast<const uint8_t*>(mapped)+footprint.Offset,footprint.Footprint.RowPitch,result.eye_width,result.eye_height,bgra_order(job->desc.Format),rows);
        const D3D12_RANGE none{};readback->Unmap(0,&none);readback.Reset();
        if(!converted) {finish(vr_photo::Outcome::bad_format);return;}
        const auto picture=vr_photo::picture_size(result.eye_width,result.eye_height,job->view_aspect);
        result.width=picture.width;result.height=picture.height;
        auto folder=job->folder.empty()?default_folder():job->folder;
        result.path=folder;
        std::error_code error;
        if(folder.empty() || (std::filesystem::create_directories(folder,error),error) || !std::filesystem::is_directory(folder,error)) {finish(vr_photo::Outcome::no_folder,HRESULT_FROM_WIN32(error.value()));return;}
        for(unsigned attempt=1;;++attempt) {
            auto path=folder/vr_photo::file_name(job->stamp,attempt);
            result.path=path;
            if(attempt<100 && std::filesystem::exists(path,error)) continue;
            hr=save_png(path,rows.data(),result.eye_width,result.eye_height,picture);
            if(hr!=HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) || attempt>=100) break;
        }
        result.encode_ms=ms(Clock::now()-encode_begin);
        if(FAILED(hr)) {finish(vr_photo::Outcome::write_failed,hr);return;}
        result.bytes=std::filesystem::file_size(result.path,error);
        finish(vr_photo::Outcome::saved);
    }
    catch(const std::bad_alloc&) {finish(vr_photo::Outcome::no_memory,E_OUTOFMEMORY);}
    catch(...) {finish(vr_photo::Outcome::write_failed,E_FAIL);}
}
