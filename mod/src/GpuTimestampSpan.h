#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <optional>
#include <cmath>
#include <cstring>

// Reusable timestamp storage owned with its command-list slot. Caller must
// prove that slot's fence has completed before reading or recording again.
class GpuTimestampSpan {
public:
    struct Result {double start_ms{},duration_ms{};};
    bool configure(ID3D12Device* device,ID3D12CommandQueue* queue){
        if(m_queries)return true;
        UINT64 frequency{};if(!device || !queue || FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency)return false;
        D3D12_QUERY_HEAP_DESC desc{};desc.Count=2;desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;if(FAILED(device->CreateQueryHeap(&desc,IID_PPV_ARGS(&queries))))return false;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=16;buffer.Height=1;
        buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))))return false;
        m_queries=std::move(queries);m_readback=std::move(readback);m_frequency=frequency;return true;
    }
    bool ready()const{return m_queries && m_readback && m_frequency;}
    void begin(ID3D12GraphicsCommandList* command){command->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);}
    void end(ID3D12GraphicsCommandList* command){command->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);command->ResolveQueryData(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,m_readback.Get(),0);}
    std::optional<Result> read(ID3D12CommandQueue* queue){
        if(!ready() || !queue)return {};
        std::array<UINT64,2> ticks{};void* mapped{};const D3D12_RANGE range{0,16};
        if(FAILED(m_readback->Map(0,&range,&mapped)))return {};std::memcpy(ticks.data(),mapped,16);
        const D3D12_RANGE no_write{};m_readback->Unmap(0,&no_write);
        UINT64 gpu{},cpu{};LARGE_INTEGER qpc{};if(FAILED(queue->GetClockCalibration(&gpu,&cpu)) || !QueryPerformanceFrequency(&qpc))return {};
        return convert(ticks,m_frequency,gpu,cpu,uint64_t(qpc.QuadPart));
    }
    static std::optional<Result> convert(std::array<UINT64,2> ticks,UINT64 frequency,UINT64 gpu,UINT64 cpu,UINT64 qpc_frequency){
        if(!frequency || !qpc_frequency || !ticks[0] || ticks[1]<ticks[0])return {};
        const auto start=double((static_cast<long double>(ticks[0])-gpu)*1000/frequency+static_cast<long double>(cpu)*1000/qpc_frequency);
        const auto duration=double((static_cast<long double>(ticks[1])-ticks[0])*1000/frequency);
        if(!std::isfinite(start) || start<0 || !std::isfinite(duration))return {};return Result{start,duration};
    }
private:
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queries;Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;UINT64 m_frequency{};
};
