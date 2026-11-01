#include "VrControllerHook.h"
#include <MinHook.h>
#include <spdlog/spdlog.h>
#include <cstring>

bool VrControllerHook::install(VrControllerInput& input,HMODULE fixture) {
    if (m_installed) { return refresh(); }
    const auto module = fixture ? fixture : GetModuleHandleW(L"xinput9_1_0.dll");
    if (!module) { return false; }
    const auto state = reinterpret_cast<void*>(GetProcAddress(module,"XInputGetState"));
    const auto capabilities = reinterpret_cast<void*>(GetProcAddress(module,"XInputGetCapabilities"));
    if (!state || !capabilities) { return false; }
    if (!fixture) {
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(state),&pinned)) { return false; }
    }
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) { return false; }
    if (MH_CreateHook(state,reinterpret_cast<void*>(&state_hook),reinterpret_cast<void**>(&m_original_state)) != MH_OK) { return false; }
    if (MH_CreateHook(capabilities,reinterpret_cast<void*>(&capabilities_hook),reinterpret_cast<void**>(&m_original_capabilities)) != MH_OK) {
        MH_RemoveHook(state); return false;
    }
    m_state_target = state; m_capabilities_target = capabilities; m_input = &input;
    std::memcpy(m_state_before.data(),state,m_state_before.size());
    std::memcpy(m_caps_before.data(),capabilities,m_caps_before.size());
    if (MH_EnableHook(state) != MH_OK) { uninstall(); return false; }
    std::memcpy(m_state_patch.data(),state,m_state_patch.size());
    if (MH_EnableHook(capabilities) != MH_OK) { uninstall(); return false; }
    std::memcpy(m_caps_patch.data(),capabilities,m_caps_patch.size());
    m_installed = true; m_healthy = true;
    spdlog::info("VrControllerHook: XInput9_1_0 state/capability hooks installed");
    return true;
}
bool VrControllerHook::refresh_target(void* target,const std::array<uint8_t,5>& original,const std::array<uint8_t,5>& patch) {
    if (std::memcmp(target,patch.data(),patch.size()) == 0) { return true; }
    // Startup input layers can restore a system export after our initial detour. Only re-arm
    // when the exact original bytes return; never overwrite an unknown replacement detour.
    if (std::memcmp(target,original.data(),original.size()) != 0) { return false; }
    if (MH_DisableHook(target) != MH_OK || MH_EnableHook(target) != MH_OK) { return false; }
    ++m_refreshes;
    spdlog::warn("VrControllerHook: original XInput export restored externally; re-armed owned detour");
    return std::memcmp(target,patch.data(),patch.size()) == 0;
}
bool VrControllerHook::refresh() {
    const bool state = refresh_target(m_state_target,m_state_before,m_state_patch);
    const bool caps = refresh_target(m_capabilities_target,m_caps_before,m_caps_patch);
    const bool healthy = state && caps;
    if (!healthy && m_healthy.exchange(false)) {
        spdlog::error("VrControllerHook: unknown XInput export modification; leaving replacement code unchanged");
    }
    if (healthy) { m_healthy = true; }
    return healthy;
}
void VrControllerHook::uninstall() {
    m_input = nullptr;
    // MinHook removal restores its saved bytes. Avoid removing an unknown external replacement.
    const auto remove = [](void* target,const auto& original,const auto& patch) {
        if (target && (std::memcmp(target,original.data(),original.size()) == 0 ||
            std::memcmp(target,patch.data(),patch.size()) == 0)) { MH_DisableHook(target); MH_RemoveHook(target); }
    };
    remove(m_state_target,m_state_before,m_state_patch); remove(m_capabilities_target,m_caps_before,m_caps_patch);
    m_state_target = m_capabilities_target = nullptr; m_installed = false; m_healthy = false;
}
DWORD WINAPI VrControllerHook::state_hook(DWORD index,XINPUT_STATE* state) {
    auto& hook = get(); ++hook.m_polls;
    uint32_t result = hook.m_original_state(index,state);
    if (const auto input = hook.m_input.load()) { input->get_state(&result,index,state); }
    return result;
}
DWORD WINAPI VrControllerHook::capabilities_hook(DWORD index,DWORD flags,XINPUT_CAPABILITIES* capabilities) {
    auto& hook = get(); ++hook.m_capabilities;
    uint32_t result = hook.m_original_capabilities(index,flags,capabilities);
    if (const auto input = hook.m_input.load()) { input->get_capabilities(&result,index,capabilities); }
    return result;
}
