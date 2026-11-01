#pragma once
#include <d3d12.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>

// Object-owned Close receipts, observed before framework initialization. No leases.
class GpuClosedListProbe {
public:
    using Close=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
    struct Receipt {uint64_t magic{0x534d32434c4f5345},serial{},closed_ns{};uintptr_t identity{},device{},reset{},close{};uint32_t type{};};
    static GpuClosedListProbe& get() {static auto* value=new GpuClosedListProbe;return *value;}
    static uint64_t now() {return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    bool install(ID3D12Device*);
    Close original() const {return m_original.load();}
    void* address() const {return m_address.load();}
    uint64_t errors() const {return m_errors.load();}
    void observer(std::function<void(uintptr_t,bool)> callback) {std::lock_guard lock{m_mutex};m_observer=std::move(callback);}
    static std::optional<Receipt> read(ID3D12CommandList*);
    struct Birth {uint64_t magic{0x534d324249525448},serial{},created_ns{};uintptr_t identity{},device{};uint32_t type{},api{};bool initial_closed{};};
    static std::optional<Birth> birth(ID3D12CommandList*);
    void creation_observer(std::function<void(uintptr_t,bool)> callback) {std::lock_guard lock{m_mutex};m_creation_observer=std::move(callback);}
    static bool precedes_copy(const Receipt& value,uint64_t creation,uintptr_t device,D3D12_COMMAND_LIST_TYPE queue_type) {
        return value.serial && value.identity && value.closed_ns && value.closed_ns<creation && value.device && value.device==device &&
            value.type==D3D12_COMMAND_LIST_TYPE_COPY && queue_type==D3D12_COMMAND_LIST_TYPE_COPY;
    }
    void invalidate(ID3D12GraphicsCommandList* command) {if(command && FAILED(command->SetPrivateData(KEY,0,nullptr))) ++m_errors;}
    inline static constexpr GUID KEY{0xbde0e779,0x8d0f,0x411c,{0x98,0xf1,0x4b,0x30,0xb7,0xda,0x21,0xe7}};
    inline static constexpr GUID BIRTH_KEY{0xba78c5e1,0x1c44,0x4f31,{0x94,0x21,0x14,0x87,0xbc,0x36,0x12,0x31}};
private:
    using Create=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_COMMAND_LIST_TYPE,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**);
    using Create1=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*,UINT,D3D12_COMMAND_LIST_TYPE,D3D12_COMMAND_LIST_FLAGS,REFIID,void**);
    static HRESULT STDMETHODCALLTYPE on_create(ID3D12Device*,UINT,D3D12_COMMAND_LIST_TYPE,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**);
    static HRESULT STDMETHODCALLTYPE on_create1(ID3D12Device4*,UINT,D3D12_COMMAND_LIST_TYPE,D3D12_COMMAND_LIST_FLAGS,REFIID,void**);
    bool install_creation(ID3D12Device*);
    void created(void**,uint32_t,bool);
    static HRESULT STDMETHODCALLTYPE on_close(ID3D12GraphicsCommandList*);
    std::mutex m_install,m_mutex;std::function<void(uintptr_t,bool)> m_observer,m_creation_observer;
    std::atomic<Close> m_original{};std::atomic<void*> m_address{};std::atomic<uint64_t> m_serial{},m_errors{};
    std::atomic<Create> m_create{};std::atomic<Create1> m_create1{};void* m_create_address{};void* m_create1_address{};
    inline static thread_local bool s_creating{};
};
