#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <functional>
#include <vector>
#include "GpuTextureReadback.h"
#include "GpuStateEvidence.h"
#include "GpuAllocationProbe.h"
#include "GpuExtendedWriterProbe.h"
#include "GpuQueueProbe.h"
#include "GpuClosedListProbe.h"
#include "GpuInlineCopy.h"
#include "GpuSdkCapture.h"

// Desktop diagnostic: completion of an observed graphics-write batch, not frame identity.
class GpuCompletionProbe {
public:
    inline static constexpr GUID RECORDING_KEY{0x7bd1535e,0x02f4,0x4b11,{0xa2,0xbc,0x55,0xd3,0x90,0x8a,0xed,0xb9}};
    enum class Kind:size_t {draw,indexed,clear,copy,copy_texture,resolve,indirect_possible,count};
    static constexpr size_t KIND_COUNT=static_cast<size_t>(Kind::count);
    struct Origin {
        uint64_t render_id{};uint32_t token{};int32_t record{};
        std::array<float,16> source_world{},render_world{};
        uint64_t camera_revision{};uintptr_t camera_render_list{},camera_pip_entry{};
        bool operator==(const Origin&) const = default;
    };
    struct Sample {
        uint64_t serial{},generation{},writes{},lists{},gpu_begin{},gpu_end{},frequency{},submission_sequence{};
        uintptr_t resource{},queue{},commands{};
        std::array<uint64_t,KIND_COUNT> kinds{};
        uintptr_t last_write_callsite{},execute_callsite{};uint32_t last_write_thread{};
        std::optional<Origin> origin;
        double gpu_ms{},completion_upper_ms{};
        std::chrono::steady_clock::time_point submitted{},completed{};
        uint64_t target_lists{};bool all_lists_tracked{};
        bool all_bindings_covered{true};
        uint32_t barrier_hazards{};
    };
    bool install(ID3D12Device*,ID3D12CommandQueue*,std::function<uintptr_t(uintptr_t)> resolve,std::function<std::optional<Origin>()> origin={},std::function<bool(uintptr_t)> binding_coverage={});
    void inline_copies(GpuInlineCopy* observer) {m_inline=observer;} // Set before install, immutable while hooks run.
    void sdk_copies(GpuSdkCapture* observer){m_sdk=observer;}
    GpuInlineCopy::Sample finish_inline(uint64_t tag);
    std::optional<uint64_t> open_recording(ID3D12GraphicsCommandList*);
    static void internal(const std::function<void()>&);
    struct BarrierCapabilities {bool known{},enhanced_supported{},enhanced_hook_installed{};uint32_t base_writer_hook_mask{};bool binding_known{},list4_supported{};uint32_t binding_hook_mask{};};
    // Read after startup installation; capabilities remain immutable thereafter.
    BarrierCapabilities barrier_capabilities() const {return {m_barrier_capabilities_known,m_enhanced_supported,m_enhanced_hook_installed,m_base_writer_hook_mask,m_binding_capabilities_known,m_list4_supported,m_binding_hook_mask};}
    GpuAllocationProbe::Status allocation_status() const {return GpuAllocationProbe::observed_status(m_device.Get());}
    GpuExtendedWriterProbe::Status extended_writer_status() const {return m_extended_writers.status();}
    bool target(ID3D12Resource*); // Resource lease prevents address reuse.
    void bound(uintptr_t,std::span<const uintptr_t>,bool known=false);
    void clear_target();
    std::optional<Sample> latest();
    struct Readback {
        uint64_t writer_serial{},writer_submission{},copy_submission{},executions{};
        uintptr_t queue{},resource{},commands{};
        uint32_t source_state{};
        bool fence_completed{};
        bool raster_state{},common_decay{};
        uint32_t barrier_hazards{};
        bool barrier_capabilities_known{},enhanced_supported{},enhanced_hook_installed{};
        uint32_t base_writer_hook_mask{};
        std::optional<GpuAllocationProbe::Record> allocation;
        uint64_t excluded_aliases{},excluded_uavs{};std::array<uint64_t,5> excluded_writers{};
        bool binding_capabilities_known{},list4_supported{},signature_hook_installed{};uint32_t binding_hook_mask{};
        std::array<uint64_t,2> indirect_exclusions{};
        GpuExtendedWriterProbe::Status extended;
        std::array<uint64_t,GpuTargetAccess::extended_count> extended_exclusions{};
    };
    // Copies only at the latest completed observed submission boundary. Rejects
    // any intervening hooked queue submission; not proof of every writer inside
    // that batch or of prior work on another queue.
    HRESULT readback(uint64_t expected_serial,std::span<const GpuTextureReadback::Source>,
                     std::vector<GpuTextureReadback::Pixels>&,Readback&);
    bool readback_active() const {return m_readback_active.load();}
    struct Snapshot {
        uint64_t tag{};HRESULT result{E_PENDING};Sample writer;Readback boundary;
        std::vector<GpuTextureReadback::Pixels> pixels;
    };
    bool request_snapshot(uint64_t tag);
    std::optional<Snapshot> take_snapshot(uint64_t tag);
    struct SnapshotStatus {
        uint64_t writes{},unknown_lists{},transition_lists{};
        uint32_t state{UINT32_MAX};bool requested{};
        uint64_t barrier_calls{},target_transitions{},untracked_transitions{},foreign_submissions{};
        uint64_t lists{},raster_state_lists{};
        uint64_t aliasing_barriers{},uav_barriers{},malformed_barriers{},enhanced_calls{},untracked_hazards{},foreign_hazard_submissions{};
        uint32_t barrier_hazards{};
        std::array<uint64_t,5> unsupported_writer_calls{};
        std::array<uint64_t,5> nested_writer_calls{};
        std::array<uint64_t,5> excluded_writer_calls{};
        uint64_t excluded_aliases{},excluded_uavs{};
        uintptr_t unsupported_writer_callsite{};uint32_t unsupported_writer_kind{},unsupported_writer_thread{};
        uint64_t alias_target_named{},alias_both_null{},alias_other{};
        struct AliasSample {
            uint64_t ordinal{};uintptr_t command{},before{},after{},callsite{};uint32_t thread{};
            std::optional<GpuAllocationProbe::Record> before_allocation,after_allocation;
        } alias_sample;
        std::optional<GpuAllocationProbe::Record> allocation;
        std::array<uint64_t,6> indirect_actions{};
        std::optional<GpuAllocationProbe::SignatureRecord> signature;
        uintptr_t signature_interface{},indirect_command{};uint32_t indirect_max_count{};
        std::array<uint64_t,3> binding_calls{};
        std::array<uint64_t,2> indirect_exclusions{};
        uint64_t indirect_potential{},indirect_binding_unknown{},indirect_binding_uncovered{};
        std::array<uint64_t,GpuTargetAccess::extended_count> extended_calls{},extended_exclusions{};
        uintptr_t extended_callsite{};uint32_t extended_kind{};
        std::optional<GpuQueueProbe::Device> queue_device;
        std::optional<GpuQueueProbe::Queue> queue_creation;
        uint64_t unknown_queue_submissions{},foreign_unknown_lists{},foreign_target_submissions{},untracked_target_writes{};
        struct ForeignList {
            uint64_t ordinal{};uintptr_t queue{},commands{},graphics{},identity{},reset{},close{},callsite{};
            uint32_t thread{},type{},reason{};bool reset_matches{},close_matches{};
            uint64_t target_creation_begin_ns{};std::optional<GpuClosedListProbe::Receipt> closure;
            bool precreation_copy{};
            bool recording_stamp_known{};uint64_t recording_generation{},recording_epoch{};uintptr_t recording_identity{};
            std::optional<GpuClosedListProbe::Birth> birth;
        } foreign_list;
        uint64_t foreign_precreation_copies{};
        uint64_t recording_cache_clears{};
        uint64_t foreign_fenced_submissions{},foreign_fenced_lists{},foreign_fence_failures{};
        uintptr_t foreign_fenced_queue{};uint64_t foreign_fence_value{},foreign_completed_value{};double foreign_fence_ms{};
        struct ForeignCompletion {uint64_t submission{},lists{},signaled{},completed{};uintptr_t queue{},device{};double wait_ms{};};
        std::vector<ForeignCompletion> foreign_completions;
    };
    SnapshotStatus snapshot_status() {std::lock_guard lock{m_mutex};auto result=m_snapshot_status;result.allocation=GpuAllocationProbe::read(m_target.Get());result.queue_device=GpuQueueProbe::read_device(m_device.Get());result.queue_creation=GpuQueueProbe::read(m_queue.Get());return result;}
private:
    struct Recording {
        uint64_t generation{},epoch{},writes{};bool bound{},closed{};
        bool binding_known{};
        bool binding_covered{};
        std::array<uint64_t,KIND_COUNT> kinds{};uintptr_t last_write_callsite{};uint32_t last_write_thread{};
        std::optional<Origin> origin;
        GpuStateEvidence state;
    };
    struct Work {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>,2> markers;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer,resource;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Sample sample;uint64_t epoch{};bool busy{};
    };
    bool create_work(Work&);
    void poll();
    HRESULT capture_locked(const Sample&,std::span<const GpuTextureReadback::Source>,std::vector<GpuTextureReadback::Pixels>&,Readback&);
    void barriers(uintptr_t,UINT,const D3D12_RESOURCE_BARRIER*,uintptr_t);
    void enhanced_barriers(uintptr_t,UINT,const D3D12_BARRIER_GROUP*);
    void unmodeled_writer(uintptr_t,GpuStateEvidence::Writer,uintptr_t);
    void indirect(uintptr_t,ID3D12CommandSignature*,UINT,uintptr_t);
    bool binding_covered(uintptr_t) const;
    bool recording_matches(uintptr_t,const Recording&) const;
    void binding_state(uintptr_t,uint32_t);
    void extended_writer(uintptr_t,GpuTargetAccess::Extended,uintptr_t,uintptr_t,std::optional<GpuAllocationProbe::Record>);
    void reset(uintptr_t);
    static void observe(Recording&,Kind,uintptr_t,std::optional<Origin> origin={});
    void draw(uintptr_t,Kind,uintptr_t);
    void write(uintptr_t,uintptr_t,Kind,uintptr_t);
    void execute(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*,uintptr_t);
    bool complete_foreign_copy(ID3D12CommandQueue*,uint64_t);
    using Reset=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
    using Close=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
    using Draw=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
    using Indexed=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,INT,UINT);
    using Execute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*);
    using Clear=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT*,UINT,const D3D12_RECT*);
    using Copy=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*);
    using CopyTexture=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,const D3D12_TEXTURE_COPY_LOCATION*,UINT,UINT,UINT,const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*);
    using Resolve=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT,ID3D12Resource*,UINT,DXGI_FORMAT);
    using Barriers=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
    using Enhanced=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*,UINT,const D3D12_BARRIER_GROUP*);
    using Dispatch=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT);
    using Indirect=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64);
    using Bundle=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList*);
    using UavUint=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const UINT*,UINT,const D3D12_RECT*);
    using UavFloat=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const FLOAT*,UINT,const D3D12_RECT*);
    using ClearState=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
    using BeginPass=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,UINT,const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,D3D12_RENDER_PASS_FLAGS);
    using EndPass=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);
    static HRESULT STDMETHODCALLTYPE on_reset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
    static HRESULT STDMETHODCALLTYPE on_close(ID3D12GraphicsCommandList*);
    static void STDMETHODCALLTYPE on_draw(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
    static void STDMETHODCALLTYPE on_indexed(ID3D12GraphicsCommandList*,UINT,UINT,UINT,INT,UINT);
    static void STDMETHODCALLTYPE on_execute(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*);
    static void STDMETHODCALLTYPE on_clear(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT*,UINT,const D3D12_RECT*);
    static void STDMETHODCALLTYPE on_copy(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*);
    static void STDMETHODCALLTYPE on_copy_texture(ID3D12GraphicsCommandList*,const D3D12_TEXTURE_COPY_LOCATION*,UINT,UINT,UINT,const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*);
    static void STDMETHODCALLTYPE on_resolve(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT,ID3D12Resource*,UINT,DXGI_FORMAT);
    static void STDMETHODCALLTYPE on_barriers(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
    static void STDMETHODCALLTYPE on_enhanced(ID3D12GraphicsCommandList7*,UINT,const D3D12_BARRIER_GROUP*);
    static void STDMETHODCALLTYPE on_dispatch(ID3D12GraphicsCommandList*,UINT,UINT,UINT);
    static void STDMETHODCALLTYPE on_indirect(ID3D12GraphicsCommandList*,ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64);
    static void STDMETHODCALLTYPE on_bundle(ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList*);
    static void STDMETHODCALLTYPE on_uav_uint(ID3D12GraphicsCommandList*,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const UINT*,UINT,const D3D12_RECT*);
    static void STDMETHODCALLTYPE on_uav_float(ID3D12GraphicsCommandList*,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const FLOAT*,UINT,const D3D12_RECT*);
    static void STDMETHODCALLTYPE on_clear_state(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
    static void STDMETHODCALLTYPE on_begin_pass(ID3D12GraphicsCommandList4*,UINT,const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,D3D12_RENDER_PASS_FLAGS);
    static void STDMETHODCALLTYPE on_end_pass(ID3D12GraphicsCommandList4*);
    inline static GpuCompletionProbe* s_probe{};
    inline static Reset s_reset{};inline static Close s_close{};inline static Draw s_draw{};
    inline static Indexed s_indexed{};inline static Execute s_execute{};
    inline static Clear s_clear{};
    inline static Copy s_copy{};inline static CopyTexture s_copy_texture{};inline static Resolve s_resolve{};
    inline static Barriers s_barriers{};
    inline static Enhanced s_enhanced{};
    inline static Dispatch s_dispatch{};inline static Indirect s_indirect{};inline static Bundle s_bundle{};
    inline static UavUint s_uav_uint{};inline static UavFloat s_uav_float{};
    inline static ClearState s_clear_state{};inline static BeginPass s_begin_pass{};inline static EndPass s_end_pass{};
    inline static thread_local bool s_internal{};
    inline static thread_local bool s_writer_forwarding{};
    inline static thread_local Readback* s_readback{};
    std::function<uintptr_t(uintptr_t)> m_resolve;
    std::function<std::optional<Origin>()> m_origin;
    std::function<bool(uintptr_t)> m_binding_coverage;
    GpuInlineCopy* m_inline{};
    GpuSdkCapture* m_sdk{};
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_target;
    // Mirrors m_target for the per-command hooks, which return before the lock without one (every play session).
    std::atomic<bool> m_has_target{};
    uintptr_t m_target_identity{};
    GpuAllocationProbe m_allocation;
    GpuExtendedWriterProbe m_extended_writers;
    GpuTargetAccess m_access;
    std::mutex m_mutex;std::map<uintptr_t,Recording> m_recordings;
    struct PendingForeignFence {Microsoft::WRL::ComPtr<ID3D12Fence> fence;HANDLE event{};};
    std::vector<PendingForeignFence> m_pending_foreign_fences; // Timeout leases live until process exit, including registered event handles.
    std::array<Work,16> m_work;uint64_t m_epoch{},m_serial{},m_generation{},m_submission{};
    std::atomic_bool m_readback_active{};
    std::optional<Sample> m_latest;
    std::optional<D3D12_RESOURCE_STATES> m_target_state;
    std::optional<uint64_t> m_snapshot_request;
    std::optional<Snapshot> m_snapshot;
    SnapshotStatus m_snapshot_status;
    bool m_single_subresource{},m_simultaneous{},m_state_raster{},m_state_decay{};
    bool m_barrier_capabilities_known{},m_enhanced_supported{},m_enhanced_hook_installed{};
    uint32_t m_base_writer_hook_mask{};
    bool m_binding_capabilities_known{},m_list4_supported{};uint32_t m_binding_hook_mask{};
    std::array<void*,15> m_command_addresses{};
    void* m_enhanced_address{};
    std::array<void*,3> m_binding_addresses{};
    uint32_t m_epoch_barrier_hazards{}; // Unattributed/foreign-queue hazards cannot recover within this target epoch.
};
