#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <climits>
#pragma comment(lib,"d3dcompiler.lib")

// Draws the guard guide's flat quads (VrGuardGuide::clip corners) straight into an eye image
// on the producing queue: no texture, no depth, alpha blended, a few hundred pixels each.
// round: the quad's inscribed disc instead (the aim dot, VrAimDot), its colour inside ring (a
// fraction of the radius) and dark outside it, with a one-pixel soft edge.
class GuardGuideRenderer {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12RootSignature> m_root;Ptr<ID3D12PipelineState> m_pipeline;DXGI_FORMAT m_format{};
public:
    struct Bindings {Ptr<ID3D12DescriptorHeap> rtv;};
    HRESULT prepare(ID3D12Device* device,DXGI_FORMAT format,Bindings& bindings) {
        if(!bindings.rtv) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;desc.NumDescriptors=1;
            const auto hr=device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&bindings.rtv));if(FAILED(hr)) return hr;
        }
        if(m_pipeline && m_format==format) return S_OK;
        static constexpr char shader[]=R"(
cbuffer Guide : register(b0) {float4 corner[4];float4 colour;float4 style;}
struct V {float4 position:SV_Position;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID) {
    static const uint order[6]={0,1,2,0,2,3};static const float2 uvs[4]={float2(0,0),float2(1,0),float2(1,1),float2(0,1)};
    V v;v.position=corner[order[id]];v.uv=uvs[order[id]];return v;
}
float4 ps(V v):SV_Target {
    if(style.x<.5) return colour;
    const float r=length(v.uv*2-1),edge=max(fwidth(r),1e-3);
    const float inside=1-smoothstep(1-edge,1,r);if(inside<=0) discard;
    const float centre=1-smoothstep(style.y-edge,style.y,r);
    return float4(colour.rgb*centre,colour.a*inside);
})";
        Ptr<ID3DBlob> vs,ps,error;
        auto hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error);if(FAILED(hr)) return hr;
        hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error);if(FAILED(hr)) return hr;
        D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameter.Constants.Num32BitValues=24;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=1;signature.pParameters=&parameter;
        Ptr<ID3DBlob> blob;hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);if(FAILED(hr)) return hr;
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_root));if(FAILED(hr)) return hr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=m_root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
        p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        auto& blend=p.BlendState.RenderTarget[0];blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_RED|D3D12_COLOR_WRITE_ENABLE_GREEN|D3D12_COLOR_WRITE_ENABLE_BLUE;blend.BlendEnable=TRUE;
        blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha=D3D12_BLEND_ZERO;blend.DestBlendAlpha=D3D12_BLEND_ONE;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=format;p.SampleDesc.Count=1;
        hr=device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_pipeline));if(SUCCEEDED(hr)) m_format=format;return hr;
    }
    // target is in RENDER_TARGET state. corners: 4 x (x,y,z,w) clip space, in order around the quad.
    // ring>0 draws the round dot (see above) with that inner fraction of the radius in colour.
    void draw(ID3D12Device* device,ID3D12GraphicsCommandList* commands,Bindings& bindings,ID3D12Resource* target,const std::array<float,16>& corners,const std::array<float,4>& colour,float ring=0) {
        const auto d=target->GetDesc();const auto rtv=bindings.rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target,nullptr,rtv);
        std::array<float,24> constants{};std::copy(corners.begin(),corners.end(),constants.begin());std::copy(colour.begin(),colour.end(),constants.begin()+16);
        constants[20]=ring>0?1.f:0.f;constants[21]=ring;
        commands->SetPipelineState(m_pipeline.Get());commands->SetGraphicsRootSignature(m_root.Get());commands->SetGraphicsRoot32BitConstants(0,24,constants.data(),0);
        const D3D12_VIEWPORT viewport{0,0,float(d.Width),float(d.Height),0,1};const D3D12_RECT scissor{0,0,LONG(d.Width),LONG(d.Height)};
        commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->DrawInstanced(6,1,0,0);
    }
    void reset() {m_pipeline.Reset();m_root.Reset();m_format={};}
};
