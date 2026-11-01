#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cfloat>
#pragma comment(lib,"d3dcompiler.lib")

// Optional native menu image composition; called on the game's direct queue.
// Every reusable eye slot owns its descriptors until that slot's fence completes.
class MenuPanelRenderer {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12RootSignature> m_root;Ptr<ID3D12PipelineState> m_pipeline;DXGI_FORMAT m_format{};
public:
    struct Bindings {Ptr<ID3D12DescriptorHeap> srv,rtv;};
    HRESULT prepare(ID3D12Device* device,DXGI_FORMAT format,Bindings& bindings) {
        if(!bindings.srv) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;desc.NumDescriptors=1;desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            auto hr=device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&bindings.srv));if(FAILED(hr)) return hr;
            desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr=device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&bindings.rtv));if(FAILED(hr)) {bindings.srv.Reset();return hr;}
        }
        if(m_pipeline && m_format==format) return S_OK;
        static constexpr char shader[]=R"(
cbuffer Transform : register(b0) {float4 row_u,row_v,row_w;}
Texture2D source_image : register(t0);SamplerState linear_clamp : register(s0);
struct Vertex {float4 position:SV_Position;float2 uv:TEXCOORD0;};
Vertex vs(uint id:SV_VertexID) {Vertex v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv.x*2-1,1-v.uv.y*2,0,1);return v;}
float4 ps(Vertex v):SV_Target {float4 p=float4(v.uv,1,0);float w=dot(row_w,p);
 if(w<=0) return float4(0,0,0,1);float2 uv=float2(dot(row_u,p),dot(row_v,p))/w;
 if(any(uv<0) || any(uv>1)) return float4(0,0,0,1);return float4(source_image.Sample(linear_clamp,uv).rgb,1);}
)";
        Ptr<ID3DBlob> vs,ps,error;
        auto hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error);if(FAILED(hr)) return hr;
        hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error);if(FAILED(hr)) return hr;
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
        D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=12;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=FLT_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;signature.NumStaticSamplers=1;signature.pStaticSamplers=&sampler;
        Ptr<ID3DBlob> root_blob;hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error);if(FAILED(hr)) return hr;
        hr=device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&m_root));if(FAILED(hr)) return hr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=m_root.Get();
        pipeline.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pipeline.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pipeline.SampleMask=UINT_MAX;pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=format;pipeline.SampleDesc.Count=1;
        hr=device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&m_pipeline));if(SUCCEEDED(hr)) m_format=format;return hr;
    }
    void draw(ID3D12Device* device,ID3D12GraphicsCommandList* commands,Bindings& bindings,ID3D12Resource* source,ID3D12Resource* target,const std::array<float,12>& transform) {
        // The view reads the menu in its own format: the back buffer can be BGRA or sRGB while the eye is not (EyeTextureCapture accepts all four).
        const auto desc=target->GetDesc();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=source->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(source,&srv,bindings.srv->GetCPUDescriptorHandleForHeapStart());
        const auto rtv=bindings.rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target,nullptr,rtv);
        commands->SetPipelineState(m_pipeline.Get());commands->SetGraphicsRootSignature(m_root.Get());
        ID3D12DescriptorHeap* heaps[]{bindings.srv.Get()};commands->SetDescriptorHeaps(1,heaps);
        commands->SetGraphicsRoot32BitConstants(0,12,transform.data(),0);commands->SetGraphicsRootDescriptorTable(1,bindings.srv->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1};const D3D12_RECT scissor{0,0,LONG(desc.Width),LONG(desc.Height)};
        commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->DrawInstanced(3,1,0,0);
    }
    void reset() {m_pipeline.Reset();m_root.Reset();m_format={};}
};
