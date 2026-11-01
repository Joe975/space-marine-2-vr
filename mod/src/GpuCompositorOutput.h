#pragma once
#include "RenderTargetProbe.h"
#include "GpuInlineCopy.h"

// Ordinary compositor destination: the private eye output of every headset launch (pair retention;
// began as a desktop-only experiment). Only an actual known matching
// native RTV binding is changed; no scene, shader or native compositor is replayed.
// Exposed private storage is kept for process lifetime until general retirement
// has its own completion contract.
class GpuCompositorOutput {
public:
    using Open=std::function<std::optional<uint64_t>(ID3D12GraphicsCommandList*)>;
    using Clear=std::function<void(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE)>;
    enum class Refusal:uint32_t{none,configuration,scope,recording,description,unsafe_end};
    struct ObservedTarget {uintptr_t identity{};uint64_t width{};uint32_t height{},format{};bool known{};};
    struct Receipt {uint64_t tag{},generation{},bindings{},restores{};uintptr_t source{},target{},commands{};uint32_t format{},width{},height{},source_flags{},source_heap_flags{};Refusal refusal{};bool active{},restored{},restore_required{},private_backing{},source_heap_known{};std::array<ObservedTarget,8> observed{};size_t observed_count{};};
    bool configure(ID3D12Device*,ID3D12Resource* shape,Open,Clear);
    bool begin(uint64_t tag,ID3D12Resource* actual_source);
    std::optional<RenderTargetProbe::Binding> transform(ID3D12GraphicsCommandList*,const RenderTargetProbe::Binding&,std::span<const RenderTargetProbe::View>);
    Receipt end(uint64_t tag,bool copy_appended,RenderTargetProbe&);
    Receipt receipt() const {std::lock_guard lock{m_snapshot_mutex};return m_completed;}
    ID3D12Resource* texture() const {return m_texture.Get();} // Actual owned lease.
    // An unsafe end poisons the output (its texture's GPU state is unknown) and begin() refuses it
    // from then on. renew() gives a poisoned output a fresh texture and RTV, leaving the old ones
    // alive and unused; true when the output is usable (not poisoned) afterwards. 29 Oct: a cold
    // restart that kept a poisoned output refused every later eye (Refusal::redirect) until its
    // budget ran out and stereo stopped for the session.
    bool renew();
    bool poisoned() const {return m_poisoned;}
    // The redirect takes the game's final draw off the back buffer. With DLSS it is black afterwards, UI
    // over black (VR_MENUS). The TAA path leaves it uncleared, so old frames and every UI frame pile up,
    // which reads as a full-screen menu (docs/TAA_STEREO.md). True: end() clears it, as the game would.
    static void clear_native(bool value){s_clear_native.store(value,std::memory_order_relaxed);}
    uint64_t renewals() const {return m_renewals;}
    static void write_receipt(std::ostream&,const Receipt&);
    ~GpuCompositorOutput();
private:
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_texture,m_source;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commands;
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtv{};D3D12_RESOURCE_DESC m_desc{};
    D3D12_RESOURCE_STATES m_state{D3D12_RESOURCE_STATE_COMMON};
    Open m_open;Clear m_clear;RenderTargetProbe::Binding m_last_native;
    Receipt m_receipt,m_completed;mutable std::mutex m_snapshot_mutex;
    std::atomic_flag m_owner=ATOMIC_FLAG_INIT;
    inline static thread_local GpuCompositorOutput* s_active{};
    inline static std::atomic_bool s_clear_native{};
    uint32_t m_thread{};bool m_current_replaced{},m_exposed{},m_poisoned{};uint64_t m_renewals{};
    bool create(ID3D12Device*,const D3D12_RESOURCE_DESC&,const D3D12_HEAP_PROPERTIES&);
};
