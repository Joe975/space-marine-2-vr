#pragma once
#include "GpuInlineCopy.h"

// Bounded diagnostic mirror on the producing direct queue. Caller supplies the
// current actual GetBuffer lease at the existing Present protocol boundary,
// where its state must be PRESENT. Uses a new list, never native cached bindings.
class GpuPresentMirror {
public:
    struct Sample {uint64_t tag{},producer_fence{},fence_value{};uintptr_t image{},destination{},queue{};HRESULT result{E_PENDING};bool submitted{},completed{};};
    HRESULT submit(ID3D12CommandQueue*,const GpuInlineCopy::Sample&,uint64_t expected_tag,ID3D12Resource* actual_buffer,uintptr_t expected_buffer);
    std::optional<Sample> sample(uint64_t tag);
    bool reset();
    ~GpuPresentMirror();
    static void write_sample(std::ostream&,const Sample&);
private:
    struct Work {
        Sample sample;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Resource> source,destination;
        Microsoft::WRL::ComPtr<ID3D12Fence> producer,fence;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    };
    void poll();
    std::vector<Work> m_work;
};
