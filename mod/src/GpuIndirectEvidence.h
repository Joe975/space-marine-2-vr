#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>

// Actual successful CreateCommandSignature inputs, not a guess from its name
// or ExecuteIndirect's current PSO. GPU argument/count values remain unknown.
struct GpuIndirectEvidence {
    enum Action:uint32_t {unknown,draw,indexed,dispatch,rays,mesh};
    uint32_t stride{},count{};std::array<D3D12_INDIRECT_ARGUMENT_DESC,32> arguments{};
    Action action() const {
        if(!stride || stride%4 || !count || count>arguments.size()) return unknown;
        uint64_t bytes{};
        for(uint32_t i=0;i+1<count;++i) {
            switch(arguments[i].Type) {
                case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:bytes+=sizeof(D3D12_VERTEX_BUFFER_VIEW);break;
                case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:bytes+=sizeof(D3D12_INDEX_BUFFER_VIEW);break;
                case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
                    if(!arguments[i].Constant.Num32BitValuesToSet) return unknown;
                    bytes+=uint64_t(arguments[i].Constant.Num32BitValuesToSet)*4;break;
                case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
                case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
                case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:bytes+=sizeof(D3D12_GPU_VIRTUAL_ADDRESS);break;
                default:return unknown;
            }
        }
        Action result{};uint32_t action_bytes{};
        switch(arguments[count-1].Type) {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:result=draw;action_bytes=sizeof(D3D12_DRAW_ARGUMENTS);break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:result=indexed;action_bytes=sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:result=dispatch;action_bytes=sizeof(D3D12_DISPATCH_ARGUMENTS);break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:result=rays;action_bytes=sizeof(D3D12_DISPATCH_RAYS_DESC);break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:result=mesh;action_bytes=sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);break;
            default:return unknown;
        }
        return bytes+action_bytes<=stride?result:unknown;
    }
};
