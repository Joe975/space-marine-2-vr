#pragma once
#include "EyeTextureCapture.h"
#include "VrProjection.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok,const char* message) { if (!ok) { throw std::runtime_error(message); } }
void check(HRESULT hr) { if (FAILED(hr)) { throw std::runtime_error("D3D12 fixture call failed: " + std::to_string(hr)); } }
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER result{}; result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    result.Transition = {resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; return result;
}
void wait(ID3D12Fence* fence,uint64_t value) {
    const auto event = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    require(event != nullptr,"fixture event creation failed");
    check(fence->SetEventOnCompletion(value,event));
    const auto result = WaitForSingleObject(event,2000); CloseHandle(event);
    require(result == WAIT_OBJECT_0,"fixture GPU timeout");
}
struct Host {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    uint64_t tick{};
    Host() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); }
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)));
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));
        check(commands->Close());
    }
    void begin() { check(allocator->Reset()); check(commands->Reset(allocator.Get(),nullptr)); }
    void execute() {
        check(commands->Close()); ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(fence.Get(),++tick)); wait(fence.Get(),tick);
    }
    ComPtr<ID3D12Resource> color(float red,float green,float blue,unsigned width = 2,DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,unsigned height = 2) {
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> texture; check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)));
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{}; rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv_desc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv; check(device->CreateDescriptorHeap(&rtv_desc,IID_PPV_ARGS(&rtv)));
        const auto handle = rtv->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(texture.Get(),nullptr,handle);
        begin(); auto before = barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands->ResourceBarrier(1,&before); const float rgba[]{red,green,blue,1}; commands->ClearRenderTargetView(handle,rgba,0,nullptr);
        auto after = barrier(texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON); commands->ResourceBarrier(1,&after); execute();
        return texture;
    }
    void pixels(const EyeTextureCapture::Image& image,uint8_t red,uint8_t green,uint8_t blue) {
        require(image.ready(),"readback must wait for copy completion");
        const auto desc = image.texture()->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 size{};
        device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&size);
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = size; buffer.Height = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> readback; check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        begin(); auto before = barrier(image.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE); commands->ResourceBarrier(1,&before);
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = image.texture(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION target{}; target.pResource = readback.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; target.PlacedFootprint = layout;
        commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        auto after = barrier(image.texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); commands->ResourceBarrier(1,&after); execute();
        void* mapped{}; D3D12_RANGE range{0,static_cast<size_t>(size)}; check(readback->Map(0,&range,&mapped));
        bool correct = true;
        for (unsigned y = 0; y < desc.Height; ++y) for (unsigned x = 0; x < desc.Width; ++x) {
            const auto* pixel = static_cast<uint8_t*>(mapped)+layout.Offset+y*layout.Footprint.RowPitch+x*4;
            correct &= pixel[0] == red && pixel[1] == green && pixel[2] == blue && pixel[3] == 255;
        }
        D3D12_RANGE no_writes{}; readback->Unmap(0,&no_writes);
        require(correct,"retained image pixels must match its source independently of other eye copies");
    }
};
EyeTextureCapture::Metadata meta(uint64_t id,int eye,uint32_t token) { EyeTextureCapture::Metadata m{}; m.render_id = id; m.eye = eye; m.game_token = token; return m; }
}

