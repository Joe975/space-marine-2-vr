#pragma once
#include <d3d12.h>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>

// Creation evidence lives on the real device/queue. No queue/device COM leases.
class GpuQueueProbe {
public:
    struct Device {
        uint64_t magic{0x534d325144455649},serial{};uintptr_t identity{};bool early_return{},device9{};uint32_t hooks{};
        std::array<uint64_t,2> calls{};uint64_t annotated{},failed{},method_count{};std::array<uintptr_t,16> execute_methods{};
    };
    struct Queue {
        uint64_t magic{0x534d325155455545},serial{},device_serial{};uintptr_t device{},identity{},execute{},callsite{};
        uint32_t api{},thread{},requested_node{};D3D12_COMMAND_QUEUE_DESC desc{};GUID creator{};
    };
    static GpuQueueProbe& get();
    bool observe_device(ID3D12Device*,bool early_return=false);
    static std::optional<Device> read_device(ID3D12Device*);
    static std::optional<Queue> read(ID3D12CommandQueue*);
    static bool all_observed_methods_match(const Device&,uintptr_t);
    uint64_t errors() const {return m_errors.load();}
    inline static constexpr GUID DEVICE_KEY{0x2a48abb8,0x2237,0x476b,{0x92,0xa9,0x14,0x55,0x3f,0x58,0xe1,0x4e}};
    inline static constexpr GUID QUEUE_KEY{0xf25e16d1,0x51b9,0x4e22,{0x8c,0x3f,0x31,0x01,0xd7,0x8e,0xef,0xa0}};
private:
    using Create=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**);
    using Create1=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device9*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,REFIID,void**);
    template<size_t I> static HRESULT STDMETHODCALLTYPE on_create(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**);
    template<size_t I> static HRESULT STDMETHODCALLTYPE on_create1(ID3D12Device9*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,REFIID,void**);
    void annotate(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC*,void**,uint32_t,const GUID&,uintptr_t);
    std::mutex m_mutex;std::map<void*,size_t> m_base,m_extended;
    std::array<std::atomic<Create>,16> m_originals{};std::array<std::atomic<Create1>,16> m_originals1{};
    std::atomic<uint64_t> m_serial{},m_errors{};
};
