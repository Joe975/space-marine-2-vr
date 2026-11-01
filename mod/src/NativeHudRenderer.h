#pragma once
#include "MenuPanelRenderer.h"
#include "VrNativeHud.h"

// Two small native HUD regions composed into a head-relative plane, entirely on
// the producing GPU queue. No CPU readback, synchronous waits or fabricated values.
class NativeHudRenderer {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12RootSignature> m_root;Ptr<ID3D12PipelineState> m_pipeline;DXGI_FORMAT m_format{};
    static constexpr UINT CONSTANTS=48; // transform, two regions, six excluded rectangles, homography rows.
public:
    using Bindings=MenuPanelRenderer::Bindings;
    HRESULT prepare(ID3D12Device* device,DXGI_FORMAT format,Bindings& bindings){
        if(!bindings.srv){D3D12_DESCRIPTOR_HEAP_DESC d{};d.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;d.NumDescriptors=1;d.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            auto hr=device->CreateDescriptorHeap(&d,IID_PPV_ARGS(&bindings.srv));if(FAILED(hr))return hr;
            d.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;d.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;hr=device->CreateDescriptorHeap(&d,IID_PPV_ARGS(&bindings.rtv));if(FAILED(hr)){bindings.srv.Reset();return hr;}}
        if(m_pipeline && m_format==format)return S_OK;
        static constexpr char shader[]=R"(
cbuffer Hud : register(b0){float4 transform;float4 health;float4 inventory;float4 excluded[6];float4 homography[3];}
Texture2D native_hud : register(t0);SamplerState linear_clamp : register(s0);
struct Vertex{float4 position:SV_Position;float2 uv:TEXCOORD0;};
Vertex vs(uint id:SV_VertexID){Vertex v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv.x*2-1,1-v.uv.y*2,0,1);return v;}
float4 ps(Vertex v):SV_Target{
 // A plane that does not face the eye (the canvas locked to the game camera, VrHudHomography): a
 // projective map, flagged by homography[0].w. Otherwise the head-facing plane's scale and offset.
 float2 compact;
 if(homography[0].w>.5){const float3 e=float3(v.uv,1);const float3 c=float3(dot(homography[0].xyz,e),dot(homography[1].xyz,e),dot(homography[2].xyz,e));
  if(c.z<=0)discard;compact=c.xy/c.z;}
 else compact=v.uv*transform.xy+transform.zw;
 if(any(compact<0)||any(compact>1))discard;
 float4 region=compact.x<.5?health:inventory;float2 local=float2(frac(compact.x*2),compact.y);
 float2 canvas=region.xy+local*region.zw;
 // VrNativeHudExcluded: canvas areas another plane already draws.
 [unroll]for(int i=0;i<6;++i)if(all(canvas>=excluded[i].xy)&&all(canvas<excluded[i].xy+excluded[i].zw))discard;
 float3 rgb=native_hud.Sample(linear_clamp,canvas).rgb;
 // Native gameplay canvas is black behind these graphics. Premultiplied black
 // key preserves the original RGB contribution, including antialiased glyphs.
 return float4(rgb,max(rgb.r,max(rgb.g,rgb.b)));
})";
        Ptr<ID3DBlob> vs,ps,error;auto hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error);if(FAILED(hr))return hr;
        hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error);if(FAILED(hr))return hr;
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
        D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=CONSTANTS;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=FLT_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;signature.NumStaticSamplers=1;signature.pStaticSamplers=&sampler;
        Ptr<ID3DBlob> blob;hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);if(FAILED(hr))return hr;
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_root));if(FAILED(hr))return hr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=m_root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};p.SampleMask=UINT_MAX;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        auto& blend=p.BlendState.RenderTarget[0];blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;blend.BlendEnable=TRUE;
        blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=format;p.SampleDesc.Count=1;
        hr=device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_pipeline));if(SUCCEEDED(hr))m_format=format;return hr;
    }
    void draw(ID3D12Device* device,ID3D12GraphicsCommandList* commands,Bindings& bindings,ID3D12Resource* source,ID3D12Resource* target,const std::array<float,4>& transform,const VrNativeHud& crop,
              const VrNativeHudExcluded::Rects& excluded={},const VrHudHomography* homography=nullptr){
        const auto d=target->GetDesc();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=source->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(source,&srv,bindings.srv->GetCPUDescriptorHandleForHeapStart());const auto rtv=bindings.rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target,nullptr,rtv);
        std::array<float,CONSTANTS> constants{};std::copy(transform.begin(),transform.end(),constants.begin());std::copy(crop.values.values.begin(),crop.values.values.end(),constants.begin()+4);
        for(size_t i=0;i<excluded.size();++i)std::copy(excluded[i].begin(),excluded[i].end(),constants.begin()+12+4*i);
        if(homography)for(size_t row=0;row<3;++row){std::copy_n(homography->h.begin()+3*row,3,constants.begin()+36+4*row);if(row==0)constants[39]=1;}
        commands->SetPipelineState(m_pipeline.Get());commands->SetGraphicsRootSignature(m_root.Get());ID3D12DescriptorHeap* heaps[]{bindings.srv.Get()};commands->SetDescriptorHeaps(1,heaps);
        commands->SetGraphicsRoot32BitConstants(0,CONSTANTS,constants.data(),0);commands->SetGraphicsRootDescriptorTable(1,bindings.srv->GetGPUDescriptorHandleForHeapStart());
        // Restrict rasterization to the small status rectangle instead of running
        // the pixel shader across the entire 4K eye image every frame.
        const auto edge=[](float pixel,float extent){return LONG(std::clamp(pixel,0.f,extent));};
        const auto& b=homography?homography->bounds:std::array<float,4>{};
        const D3D12_RECT scissor=homography?D3D12_RECT{edge(std::floor(b[0]*float(d.Width)),float(d.Width)),edge(std::floor(b[1]*float(d.Height)),float(d.Height)),
            edge(std::ceil(b[2]*float(d.Width)),float(d.Width)),edge(std::ceil(b[3]*float(d.Height)),float(d.Height))}:
            D3D12_RECT{edge(std::floor(-transform[2]/transform[0]*float(d.Width)),float(d.Width)),edge(std::floor(-transform[3]/transform[1]*float(d.Height)),float(d.Height)),
            edge(std::ceil((1-transform[2])/transform[0]*float(d.Width)),float(d.Width)),edge(std::ceil((1-transform[3])/transform[1]*float(d.Height)),float(d.Height))};
        if(scissor.right<=scissor.left || scissor.bottom<=scissor.top)return;
        const D3D12_VIEWPORT viewport{0,0,float(d.Width),float(d.Height),0,1};commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->DrawInstanced(3,1,0,0);
    }
    void reset(){m_pipeline.Reset();m_root.Reset();m_format={};}
};
