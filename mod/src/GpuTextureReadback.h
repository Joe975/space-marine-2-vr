#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>

// Bounded synchronous diagnostic. Caller proves source state and producing queue.
// Copies all sources in one command list, restores states, then waits on one fence.
class GpuTextureReadback {
public:
    struct Source {ID3D12Resource* texture{};D3D12_RESOURCE_STATES state{};};
    struct Pixels {uint32_t width{},height{};DXGI_FORMAT format{};std::vector<uint8_t> rgb;};
    static float half(uint16_t value) {
        const int sign=(value&0x8000)?-1:1,exponent=(value>>10)&31,mantissa=value&1023;
        if(exponent==31) return mantissa?0.f:float(sign)*65504.f;
        return float(sign)*std::ldexp(float(exponent?1024+mantissa:mantissa),exponent?exponent-25:-24);
    }
    static HRESULT capture(ID3D12CommandQueue* queue,std::span<const Source> sources,std::vector<Pixels>& result) {
        using Microsoft::WRL::ComPtr;
        result.clear();
        if(!queue || sources.empty() || sources.size()>2 || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
        struct Work {
            ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Device> device;
            ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> fence;
            std::array<ComPtr<ID3D12Resource>,2> source,buffers;
            std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> footprints{};
            std::array<UINT64,2> sizes{};
        };
        auto work=std::make_unique<Work>();work->queue=queue;
        auto hr=queue->GetDevice(IID_PPV_ARGS(&work->device));if(FAILED(hr)) return hr;
        if(work->device->GetNodeCount()!=1) return E_INVALIDARG;
        std::vector<Pixels> output;
        for(size_t i=0;i<sources.size();++i) {
            if(!sources[i].texture) return E_INVALIDARG;
            ComPtr<ID3D12Device> device;hr=sources[i].texture->GetDevice(IID_PPV_ARGS(&device));
            if(FAILED(hr) || device.Get()!=work->device.Get()) return E_INVALIDARG;
            const auto desc=sources[i].texture->GetDesc();
            const bool format=desc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                desc.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
            const auto state=sources[i].state;
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width==0 || desc.Width>4096 || desc.Height==0 || desc.Height>2160 ||
               desc.MipLevels!=1 || desc.DepthOrArraySize!=1 || desc.SampleDesc.Count!=1 || !format ||
               (state!=D3D12_RESOURCE_STATE_COMMON && state!=D3D12_RESOURCE_STATE_RENDER_TARGET && state!=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE &&
                state!=(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && state!=D3D12_RESOURCE_STATE_COPY_SOURCE)) return E_INVALIDARG;
            work->source[i]=sources[i].texture;
            work->device->GetCopyableFootprints(&desc,0,1,0,&work->footprints[i],nullptr,nullptr,&work->sizes[i]);
            // Native PIP resources use typeless RGBA storage. Copy compatible bytes
            // through an explicit UNORM footprint; this does not infer an sRGB view.
            if(desc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS) work->footprints[i].Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            if(desc.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS) work->footprints[i].Footprint.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=work->sizes[i];buffer.Height=1;
            buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
            hr=work->device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&work->buffers[i]));
            if(FAILED(hr)) return hr;
            output.push_back({static_cast<uint32_t>(desc.Width),desc.Height,desc.Format,{}});
        }
        hr=work->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&work->allocator));if(FAILED(hr)) return hr;
        hr=work->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,work->allocator.Get(),nullptr,IID_PPV_ARGS(&work->commands));if(FAILED(hr)) return hr;
        hr=work->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&work->fence));if(FAILED(hr)) return hr;
        for(size_t i=0;i<sources.size();++i) {
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={work->source[i].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,sources[i].state,D3D12_RESOURCE_STATE_COPY_SOURCE};
            if(sources[i].state!=D3D12_RESOURCE_STATE_COPY_SOURCE) work->commands->ResourceBarrier(1,&barrier);
            D3D12_TEXTURE_COPY_LOCATION source{},target{};source.pResource=work->source[i].Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.pResource=work->buffers[i].Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=work->footprints[i];
            work->commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
            if(sources[i].state!=D3D12_RESOURCE_STATE_COPY_SOURCE) work->commands->ResourceBarrier(1,&barrier);
        }
        hr=work->commands->Close();if(FAILED(hr)) return hr;
        const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event) return HRESULT_FROM_WIN32(GetLastError());
        hr=work->fence->SetEventOnCompletion(1,event);if(FAILED(hr)) {CloseHandle(event);return hr;}
        ID3D12CommandList* lists[]{work->commands.Get()};queue->ExecuteCommandLists(1,lists);
        hr=queue->Signal(work->fence.Get(),1);
        if(FAILED(hr) || WaitForSingleObject(event,2000)!=WAIT_OBJECT_0 || work->fence->GetCompletedValue()==UINT64_MAX) {
            CloseHandle(event);work.release();return FAILED(hr)?hr:HRESULT_FROM_WIN32(WAIT_TIMEOUT); // GPU may still own work.
        }
        CloseHandle(event);
        for(size_t i=0;i<sources.size();++i) {
            void* mapped{};D3D12_RANGE range{0,static_cast<SIZE_T>(work->sizes[i])};
            hr=work->buffers[i]->Map(0,&range,&mapped);if(FAILED(hr)) return hr;
            auto& pixels=output[i];pixels.rgb.resize(size_t(pixels.width)*pixels.height*3);
            const bool fp16=pixels.format==DXGI_FORMAT_R16G16B16A16_FLOAT;
            const bool bgra=pixels.format==DXGI_FORMAT_B8G8R8A8_TYPELESS || pixels.format==DXGI_FORMAT_B8G8R8A8_UNORM || pixels.format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            for(uint32_t y=0;y<pixels.height;++y) for(uint32_t x=0;x<pixels.width;++x) {
                const auto* source=static_cast<const uint8_t*>(mapped)+work->footprints[i].Offset+size_t(y)*work->footprints[i].Footprint.RowPitch+x*(fp16?8:4);
                auto* target=pixels.rgb.data()+(size_t(y)*pixels.width+x)*3;
                for(size_t c=0;c<3;++c) {
                    if(fp16) {uint16_t value{};std::memcpy(&value,source+c*2,2);target[c]=static_cast<uint8_t>(std::clamp(half(value),0.f,1.f)*255+.5f);}
                    else target[c]=source[bgra?2-c:c];
                }
            }
            D3D12_RANGE empty{};work->buffers[i]->Unmap(0,&empty);
        }
        result=std::move(output);return S_OK;
    }
    static bool save(const std::filesystem::path& path,const Pixels& pixels) {
        if(!path.is_absolute() || pixels.rgb.size()!=size_t(pixels.width)*pixels.height*3 || std::filesystem::exists(path)) return false;
        std::ofstream file{path,std::ios::binary};file<<"P6\n"<<pixels.width<<' '<<pixels.height<<"\n255\n";
        file.write(reinterpret_cast<const char*>(pixels.rgb.data()),pixels.rgb.size());return bool(file);
    }
};
