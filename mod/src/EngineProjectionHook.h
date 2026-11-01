#pragma once

#include "GameBuild.h"
#include "VrProjection.h"
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <span>

class EngineProjectionHook {
public:
    enum class State { idle, installed, wrong_build, hook_failed };
    static constexpr uintptr_t BUILD_RVA = 0x2222960;
    static constexpr uintptr_t ADD_RETURN_RVA = 0x1d7709d;
    static constexpr game_build::Code PROLOGUE=game_build::hashed(0x2222960,24,"debdce4a998c8f38daa72cadb422e992d9b577702ea6e60e899bd927802b4494");
    struct Binding { vr_projection::Tangents fov; int eye{}; uint64_t render_token{}; };
    struct Sample { Binding binding; vr_camera::Matrix projection; float near_z{}, far_z{}; };
    struct Snapshot { uint64_t calls{}, overridden{}; std::optional<Sample> latest; };

    // Only the matching builder call nested inside the current main add may use this binding.
    class Scope {
    public:
        explicit Scope(std::optional<Binding> binding);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        bool applied() const { return m_applied; }
        const std::optional<vr_camera::Matrix>& output() const { return m_output; }
    private:
        friend class EngineProjectionHook;
        std::optional<Binding> m_previous;
        Scope* m_previous_scope{};
        bool m_applied{};
        std::optional<vr_camera::Matrix> m_output;
    };
    static EngineProjectionHook& get();
    static bool matches_build(uint32_t timestamp, std::span<const uint8_t> bytes);
    State install_in_game(HMODULE game);
    State install(void* target, uintptr_t module_base);
    State state() const { return m_state.load(); }
    // Fixture uses its own fixed projection-builder call site.
    void set_add_return_rva(uintptr_t rva) { m_add_return_rva.store(rva); }
    Snapshot snapshot() const;
private:
    using BuildFn = void(void*, float*, float, float);
    static void on_build(void*, float*, float, float);
    static thread_local std::optional<Binding> s_binding;
    static thread_local Scope* s_scope;
    std::atomic<State> m_state{State::idle};
    std::atomic<BuildFn*> m_original{};
    std::atomic<uintptr_t> m_add_return_rva{ADD_RETURN_RVA};
    uintptr_t m_module_base{};
    std::mutex m_install_mutex;
    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
};
