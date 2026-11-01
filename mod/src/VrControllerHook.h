#pragma once
#include "VrControllerInput.h"
#include <atomic>
#include <array>

class VrControllerHook {
public:
    static VrControllerHook& get() { static VrControllerHook value; return value; }
    bool install(VrControllerInput&,HMODULE fixture = nullptr);
    void uninstall();
    bool installed() const { return m_installed; }
    uint64_t polls() const { return m_polls; }
    uint64_t capabilities() const { return m_capabilities; }
    uint64_t refreshes() const { return m_refreshes; }
    bool healthy() const { return m_healthy; }
private:
    bool refresh();
    bool refresh_target(void*,const std::array<uint8_t,5>&,const std::array<uint8_t,5>&);
    using GetState = DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
    using GetCapabilities = DWORD(WINAPI*)(DWORD,DWORD,XINPUT_CAPABILITIES*);
    static DWORD WINAPI state_hook(DWORD,XINPUT_STATE*);
    static DWORD WINAPI capabilities_hook(DWORD,DWORD,XINPUT_CAPABILITIES*);
    std::atomic<VrControllerInput*> m_input{};
    std::atomic<bool> m_installed{};
    std::atomic<uint64_t> m_polls{},m_capabilities{};
    std::atomic<uint64_t> m_refreshes{};
    std::atomic<bool> m_healthy{};
    void* m_state_target{}; void* m_capabilities_target{};
    std::array<uint8_t,5> m_state_before{},m_state_patch{},m_caps_before{},m_caps_patch{};
    GetState m_original_state{}; GetCapabilities m_original_capabilities{};
};
