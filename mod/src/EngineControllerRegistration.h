#pragma once
#include "VrControllerInput.h"
#include "VrRumble.h"
#include <array>
#include <atomic>
#include <span>
#include <string>

// The engine enumerates HID pads before it ever polls XInput. Create its existing slot-zero
// pad on the normal input-update thread; allocation, initialization and ownership stay native.
class EngineControllerRegistration {
public:
    struct Api {
        using GetState = DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
        using Allocate = void*(*)(size_t,uint32_t);
        using Construct = void*(*)(void*);
        using Register = void(*)(void*,void**);
        Allocate allocate{}; Construct construct{}; Register register_pad{};
        uintptr_t manager_vtable{},pad_vtable{};
        GetState* state_slot{}; // The engine's resolved dispatch, independent of GetProcAddress wrappers.
        // What the engine's own resolution yields right now for its XInput module:
        // {GetProcAddress result, raw export-table address}. Re-enumeration (e.g. after a focus
        // change) re-runs GetProcAddress, which an overlay may answer with its own wrapper
        // (hypothesis: on 13 Oct the mod's own lookup returned an Epic Online Services wrapper;
        // the live adoption log names the actual module). A slot value equal to one of these
        // is the engine's legitimate route.
        using Resolve = std::array<GetState,2>(*)(void* manager);
        Resolve resolve{};
        // Rumble (docs/HAPTICS.md): the engine's resolved XInputSetState, written by the same
        // resolver just before GetState. Optional: null leaves rumble native.
        using SetState = DWORD(WINAPI*)(DWORD,XINPUT_VIBRATION*);
        SetState* set_slot{};
        using ResolveSet = std::array<SetState,2>(*)(void* manager);
        ResolveSet resolve_set{};
    };
    // In-game resolver: HMODULE at manager+0x58 (RVA 0x1d6ee80 resolves from it).
    static std::array<Api::GetState,2> resolve_native(void* manager);
    static std::array<Api::SetState,2> resolve_native_set(void* manager);
    // "module.dll+0xRVA" or the memory region kind, for dispatch diagnostics.
    static std::string describe(const void* address);
    enum class Status { idle, wrong_build, hook_failed, installed };
    static EngineControllerRegistration& get();
    static constexpr uint32_t BUILD_TIMESTAMP = 0x6ac3e716;
    static constexpr size_t UPDATE_RVA=0x1d6eca0, CONSTRUCT_RVA=0x1d6c300,
        ALLOCATE_RVA=0x26e0840, REGISTER_RVA=0x1d63870, INITIALIZE_RVA=0x1d6c380;
    static constexpr size_t PAD_SIZE=0x1248;
    static constexpr size_t STATE_SLOT_RVA=0x4f32c08;
    static constexpr size_t SET_STATE_SLOT_RVA=0x4f32c00; // DAT written by the resolver at 0x1d6ee80
    static bool matches_build(uint32_t timestamp,std::span<const uint8_t> update,
        std::span<const uint8_t> construct,std::span<const uint8_t> allocate,
        std::span<const uint8_t> registration,std::span<const uint8_t> initialize);
    Status install_in_game(HMODULE,VrControllerInput&);
    bool install(void* update,Api,VrControllerInput&);
    void uninstall();
    bool ensure_pad(void* manager); // Also used by the deterministic ABI fixture.
    Status status() const { return m_status; }
    uint64_t updates() const { return m_updates; }
    uint64_t registrations() const { return m_registrations; }
    uint64_t failures() const { return m_failures; }
    uint64_t state_polls() const { return m_state_polls; }
    uint64_t dispatch_repairs() const { return m_dispatch_repairs; }
    bool dispatch_healthy() const { return m_dispatch_healthy; }
    uint64_t dispatch_adoptions() const { return m_dispatch_adoptions; } // Legitimate re-resolved routes adopted.
    uint64_t dispatch_rejections() const { return m_dispatch_rejections; } // Distinct foreign pointers left alone.
    // Rumble: slot-zero motor speeds go to `mailbox` (null stops forwarding; the slot stays
    // bound until uninstall). The native SetState is always called first, unchanged.
    void forward_rumble(vr_rumble::Mailbox* mailbox) { m_rumble = mailbox; }
    bool rumble_available() const { return m_api.set_slot != nullptr; }
    bool rumble_healthy() const { return m_rumble_healthy; }
    uint64_t rumble_calls() const { return m_rumble_calls; }
    uint64_t rumble_binds() const { return m_rumble_binds; }
private:
    using Update = void(*)(void*);
    static void update_hook(void*);
    static DWORD WINAPI state_hook(DWORD,XINPUT_STATE*);
    static DWORD WINAPI set_state_hook(DWORD,XINPUT_VIBRATION*);
    void ensure_dispatch(void* manager);
    void ensure_rumble_dispatch(void* manager);
    bool ensure_pad_impl(void*);
    Api m_api{};
    std::atomic<VrControllerInput*> m_input{};
    std::atomic<Status> m_status{Status::idle};
    std::atomic_flag m_registering = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> m_updates{},m_registrations{},m_failures{};
    std::atomic<Api::GetState> m_original_state{};
    std::atomic<uint64_t> m_state_polls{},m_dispatch_repairs{};
    std::atomic<bool> m_dispatch_healthy{};
    std::atomic<uint64_t> m_dispatch_adoptions{},m_dispatch_rejections{};
    Api::GetState m_last_rejected{}; bool m_reported_cleared{};
    std::atomic<vr_rumble::Mailbox*> m_rumble{};
    std::atomic<Api::SetState> m_original_set{};
    std::atomic<bool> m_rumble_healthy{};
    std::atomic<uint64_t> m_rumble_calls{},m_rumble_binds{};
    Api::SetState m_last_rejected_set{};
    void* m_target{}; Update m_original{};
};
