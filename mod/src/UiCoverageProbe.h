#pragma once
#include "VrScreenMenu.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cfloat>
#include <optional>
#pragma comment(lib,"d3dcompiler.lib")

// How much of the swap chain's back buffer the game's UI covers (VrScreenMenu.h), measured on
// the GPU: one draw shrinks the back buffer to a 64x36 grid, each cell the brightest channel of
// 8x8 samples across its block, and a copy reads the grid back. At most one sample is in
// flight; begin() refuses while it is, take() returns it once its fence has completed. Runs on
// the game's direct queue with the back buffer in PRESENT state (as the menu panel's copy does),
// and keeps the back buffer referenced until the GPU is done with it.
class UiCoverageProbe {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
public:
    static constexpr UINT WIDTH=64,HEIGHT=36;
    struct Sample {float coverage{};bool counts{};uint64_t id{};};
    ~UiCoverageProbe(){release();}
    bool busy() const {return m_in_flight.has_value();}
    // counts: the sample is of a stereo frame, so a high coverage means a menu (stored with it).
    HRESULT begin(ID3D12CommandQueue* queue,ID3D12Resource* source,bool counts) {
        if(!queue || !source || busy())return E_INVALIDARG;
        if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
        Ptr<ID3D12Device> device;auto hr=queue->GetDevice(IID_PPV_ARGS(&device));if(FAILED(hr))return hr;
        Ptr<ID3D12Device> source_device;hr=source->GetDevice(IID_PPV_ARGS(&source_device));if(FAILED(hr) || source_device.Get()!=device.Get())return E_INVALIDARG;
        const auto desc=source->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels!=1 || desc.DepthOrArraySize!=1 || desc.SampleDesc.Count!=1 || desc.Width<WIDTH || desc.Height<HEIGHT ||
           (desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
            desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM && desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT) || (desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return E_INVALIDARG;
        if(m_device && m_device.Get()!=device.Get())release();
        if(!m_device){hr=create(device.Get());if(FAILED(hr)){release();return hr;}}
        hr=m_allocator->Reset();if(FAILED(hr))return hr;
        hr=m_commands->Reset(m_allocator.Get(),m_pipeline.Get());if(FAILED(hr))return hr;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=desc.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        m_device->CreateShaderResourceView(source,&srv,m_srv->GetCPUDescriptorHandleForHeapStart());
        const auto barrier=[](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;};
        const auto to_read=barrier(source,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_commands->ResourceBarrier(1,&to_read);
        const std::array<UINT,2> size{UINT(desc.Width),desc.Height};
        m_commands->SetGraphicsRootSignature(m_root.Get());ID3D12DescriptorHeap* heaps[]{m_srv.Get()};m_commands->SetDescriptorHeaps(1,heaps);
        m_commands->SetGraphicsRoot32BitConstants(0,2,size.data(),0);m_commands->SetGraphicsRootDescriptorTable(1,m_srv->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT viewport{0,0,float(WIDTH),float(HEIGHT),0,1};const D3D12_RECT scissor{0,0,LONG(WIDTH),LONG(HEIGHT)};
        const auto rtv=m_rtv->GetCPUDescriptorHandleForHeapStart();
        m_commands->RSSetViewports(1,&viewport);m_commands->RSSetScissorRects(1,&scissor);m_commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        m_commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);m_commands->DrawInstanced(3,1,0,0);
        const D3D12_RESOURCE_BARRIER after[]{barrier(source,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PRESENT),
            barrier(m_target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE)};
        m_commands->ResourceBarrier(2,after);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=m_target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=m_readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=m_footprint;
        m_commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        const auto back=barrier(m_target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);m_commands->ResourceBarrier(1,&back);
        hr=m_commands->Close();if(FAILED(hr))return hr;
        ID3D12CommandList* lists[]{m_commands.Get()};queue->ExecuteCommandLists(1,lists);
        m_source=source;m_in_flight=Sample{0,counts,++m_id};
        hr=queue->Signal(m_fence.Get(),++m_fence_value);if(FAILED(hr)){abandon();forget();return hr;} // The list may run: never free what it uses.
        return S_OK;
    }
    // The sample begin() started, once the GPU has finished it.
    std::optional<Sample> take() {
        if(!m_in_flight || !m_fence)return {};
        const auto completed=m_fence->GetCompletedValue();
        if(completed==UINT64_MAX){abandon();forget();return {};} // Device removed.
        if(completed<m_fence_value)return {};
        auto sample=*m_in_flight;m_in_flight.reset();m_source.Reset();
        void* mapped{};const D3D12_RANGE range{0,SIZE_T(m_footprint.Footprint.RowPitch)*HEIGHT};
        if(FAILED(m_readback->Map(0,&range,&mapped)))return {};
        std::array<uint8_t,WIDTH*HEIGHT> cells{};
        for(UINT y=0;y<HEIGHT;++y)for(UINT x=0;x<WIDTH;++x)
            cells[y*WIDTH+x]=static_cast<const uint8_t*>(mapped)[m_footprint.Offset+size_t(y)*m_footprint.Footprint.RowPitch+x*4];
        const D3D12_RANGE none{};m_readback->Unmap(0,&none);
        m_cells=cells;sample.coverage=ui_coverage::covered(cells);return sample;
    }
    const std::array<uint8_t,WIDTH*HEIGHT>& cells() const {return m_cells;}
    // Drops everything; GPU work still in flight keeps its resources (they are leaked, not freed).
    void release() {
        if(m_in_flight && m_fence && (m_fence->GetCompletedValue()<m_fence_value || m_fence->GetCompletedValue()==UINT64_MAX))abandon();
        forget();
    }
private:
    void forget() {
        m_in_flight.reset();m_source.Reset();m_readback.Reset();m_target.Reset();m_rtv.Reset();m_srv.Reset();m_fence.Reset();
        m_commands.Reset();m_allocator.Reset();m_pipeline.Reset();m_root.Reset();m_device.Reset();m_fence_value=0;
    }
    void abandon() {
        // Keep every object the queued list touches alive for good rather than free it under the GPU.
        for(IUnknown* p:std::initializer_list<IUnknown*>{m_source.Get(),m_readback.Get(),m_target.Get(),m_srv.Get(),m_rtv.Get(),m_commands.Get(),m_allocator.Get(),m_pipeline.Get(),m_root.Get(),m_fence.Get()})if(p)p->AddRef();
        m_in_flight.reset();
    }
    HRESULT create(ID3D12Device* device) {
        static constexpr char shader[]=R"(
cbuffer Size : register(b0) {uint2 size;}
Texture2D source_image : register(t0);
float4 vs(uint id:SV_VertexID):SV_Position {float2 uv=float2((id<<1)&2,id&2);return float4(uv.x*2-1,1-uv.y*2,0,1);}
float4 ps(float4 position:SV_Position):SV_Target {
 float2 cell=floor(position.xy);float2 scale=float2(size)/float2(64,36);float brightest=0;
 [loop] for(uint y=0;y<8;++y) [loop] for(uint x=0;x<8;++x){
  uint2 p=min(uint2((cell+(float2(x,y)+.5)/8)*scale),size-1);float3 c=source_image.Load(int3(p,0)).rgb;
  brightest=max(brightest,max(c.r,max(c.g,c.b)));}
 return float4(brightest,brightest,brightest,1);}
)";
        Ptr<ID3DBlob> vs,ps,error;
        auto hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error);if(FAILED(hr))return hr;
        hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error);if(FAILED(hr))return hr;
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
        D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=2;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;
        Ptr<ID3DBlob> blob;hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);if(FAILED(hr))return hr;
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_root));if(FAILED(hr))return hr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=m_root.Get();
        pipeline.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pipeline.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pipeline.SampleMask=UINT_MAX;pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pipeline.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pipeline.SampleDesc.Count=1;
        hr=device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&m_pipeline));if(FAILED(hr))return hr;
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap_desc.NumDescriptors=1;heap_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr=device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&m_srv));if(FAILED(hr))return hr;
        heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr=device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&m_rtv));if(FAILED(hr))return hr;
        D3D12_RESOURCE_DESC target{};target.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;target.Width=WIDTH;target.Height=HEIGHT;target.DepthOrArraySize=target.MipLevels=1;
        target.Format=DXGI_FORMAT_R8G8B8A8_UNORM;target.SampleDesc.Count=1;target.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&target,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_target));if(FAILED(hr))return hr;
        m_target->SetName(L"SM2VR UI coverage grid");
        device->CreateRenderTargetView(m_target.Get(),nullptr,m_rtv->GetCPUDescriptorHandleForHeapStart());
        UINT64 bytes{};device->GetCopyableFootprints(&target,0,1,0,&m_footprint,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;
        buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;heap.Type=D3D12_HEAP_TYPE_READBACK;
        hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_readback));if(FAILED(hr))return hr;
        hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&m_allocator));if(FAILED(hr))return hr;
        hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,m_allocator.Get(),nullptr,IID_PPV_ARGS(&m_commands));if(FAILED(hr))return hr;
        hr=m_commands->Close();if(FAILED(hr))return hr;
        hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence));if(FAILED(hr))return hr;
        m_device=device;return S_OK;
    }
    Ptr<ID3D12Device> m_device;Ptr<ID3D12RootSignature> m_root;Ptr<ID3D12PipelineState> m_pipeline;
    Ptr<ID3D12CommandAllocator> m_allocator;Ptr<ID3D12GraphicsCommandList> m_commands;Ptr<ID3D12Fence> m_fence;
    Ptr<ID3D12DescriptorHeap> m_srv,m_rtv;Ptr<ID3D12Resource> m_target,m_readback,m_source;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_footprint{};uint64_t m_fence_value{},m_id{};
    std::optional<Sample> m_in_flight;std::array<uint8_t,WIDTH*HEIGHT> m_cells{};
};
