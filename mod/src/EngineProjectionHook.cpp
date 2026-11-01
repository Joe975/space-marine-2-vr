#include "EngineProjectionHook.h"
#include "EngineCameraProbe.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <spdlog/spdlog.h>

thread_local std::optional<EngineProjectionHook::Binding> EngineProjectionHook::s_binding;
thread_local EngineProjectionHook::Scope* EngineProjectionHook::s_scope{};
EngineProjectionHook::Scope::Scope(std::optional<Binding> binding) : m_previous{s_binding}, m_previous_scope{s_scope} { s_binding = binding; s_scope = this; }
EngineProjectionHook::Scope::~Scope() { s_binding = m_previous; s_scope = m_previous_scope; }

EngineProjectionHook& EngineProjectionHook::get() {
    static auto* instance = new EngineProjectionHook;
    return *instance;
}
bool EngineProjectionHook::matches_build(uint32_t timestamp, std::span<const uint8_t> bytes) {
    return game_build::code(timestamp, BUILD_RVA, bytes, PROLOGUE);
}
EngineProjectionHook::State EngineProjectionHook::install_in_game(HMODULE game) {
    if (state() != State::idle) { return state(); }
    const auto* base = reinterpret_cast<const uint8_t*>(game);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto stamp = nt->FileHeader.TimeDateStamp;
    const auto build = game_build::rva(stamp, BUILD_RVA);
    if (!build || build + PROLOGUE.size() > nt->OptionalHeader.SizeOfImage ||
        !matches_build(stamp, {base + build, PROLOGUE.size()})) {
        m_state = State::wrong_build;
        spdlog::error("VrProjection: game build/prologue mismatch; not hooking");
        return state();
    }
    m_add_return_rva.store(game_build::rva(stamp, ADD_RETURN_RVA));
    return install(const_cast<uint8_t*>(base) + build, reinterpret_cast<uintptr_t>(base));
}
EngineProjectionHook::State EngineProjectionHook::install(void* target, uintptr_t module_base) {
    std::lock_guard lock{m_install_mutex};
    if (state() != State::idle) { return state(); }
    m_module_base = module_base;
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) { m_state = State::hook_failed; return state(); }
    void* original{};
    status = MH_CreateHook(target, reinterpret_cast<void*>(&on_build), &original);
    if (status == MH_OK) {
        m_original.store(reinterpret_cast<BuildFn*>(original));
        status = MH_EnableHook(target);
        if (status != MH_OK) { MH_RemoveHook(target); }
    }
    if (status != MH_OK) { m_state = State::hook_failed; spdlog::error("VrProjection: hook: {}", MH_StatusToString(status)); return state(); }
    m_state = State::installed;
    spdlog::info("VrProjection: hooked builder at {:x}; passive until a scoped eye is provided", reinterpret_cast<uintptr_t>(target));
    return state();
}
void EngineProjectionHook::on_build(void* camera, float* out, float near_z, float far_z) {
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto& hook = get();
    hook.m_original.load()(camera, out, near_z, far_z);
    try {
        vr_camera::Matrix projection{};
        const bool apply = out && s_binding && caller - hook.m_module_base == hook.m_add_return_rva.load() &&
            s_binding->eye >= 0 && s_binding->eye <= 1 && vr_projection::build(s_binding->fov, near_z, far_z, projection);
        if (apply) { std::memcpy(out, projection.data(), sizeof(projection)); }
        if (out && s_scope && caller - hook.m_module_base == hook.m_add_return_rva.load()) {
            vr_camera::Matrix actual{};
            std::memcpy(actual.data(), out, sizeof(actual));
            s_scope->m_output = actual;
            s_scope->m_applied = apply;
        }
        std::lock_guard lock{hook.m_mutex};
        ++hook.m_snapshot.calls;
        if (apply) {
            ++hook.m_snapshot.overridden;
            hook.m_snapshot.latest = Sample{*s_binding, projection, near_z, far_z};
        }
    } catch (...) {}
}
EngineProjectionHook::Snapshot EngineProjectionHook::snapshot() const {
    std::lock_guard lock{m_mutex};
    return m_snapshot;
}
