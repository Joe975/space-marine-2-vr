#pragma once
#include "GameBuild.h"
#include "EngineCameraProbe.h"
#include <array>

// Correct only native world marker jobs bound to a prepublished scene eye. Other UI stays native.
class EngineWorldMarkers {
public:
    static constexpr uintptr_t WORLD_VIEW_RVA=0x322540;
    static constexpr game_build::Code PROLOGUE=game_build::hashed(0x322540,14,"ea4aba630556642ebcce66b1e92763e35f1cf8229bf708fdf942a823181df99e");
    struct Sample {
        uintptr_t caller{},camera{};
        uint32_t thread{};
        uint64_t calls{},simulation{},rendering{},presenting{},local{},open{},matching{};
        unsigned simulation_phases{},rendering_phases{};
        bool vr{};
        bool corrected{};
        std::array<float,3> world{},view{};
        std::array<float,3> effective{};
        std::array<float,4> rectangle{};
        float near_z{},fov_h{},fov_v{};
    };
    struct Snapshot { uint64_t calls{},overflow{},corrected{},unbound{},reader_overrides{}; bool readers_installed{},enabled{}; std::vector<Sample> callers; };
    static EngineWorldMarkers& get();
    static bool matches_build(uint32_t,std::span<const uint8_t>);
    bool install_in_game(HMODULE);
    bool install(void*,uintptr_t);
    bool installed() const { return m_installed.load(); }
    Snapshot snapshot() const;
    void enable(bool value) { m_enabled=value && !m_reader_failed; }
    void set_marker_return_rva(uintptr_t value) { m_fixture_caller=value; }
    // view-to-world, transform, view, position, forward, right, up, aspect, vertical FOV.
    bool install_readers(const std::array<void*,9>&);
    void set_reader_return_rvas(const std::array<uintptr_t,3>& values) {for(size_t i=0;i<3;++i) m_fixture_readers[i]=values[i];}
private:
    using WorldViewFn=void(void*,const float*,float*);
    static void on_world_view(void*,const float*,float*);
    static void on_view_world(void*,const float*,float*);
    template<size_t I> static void on_value(void*,float*);
    template<size_t I> static float on_float(void*);
    std::optional<EngineCameraProbe::UiEye> reader_eye(void*,uintptr_t);
    bool marker_caller(uintptr_t) const;
    std::atomic<WorldViewFn*> m_original{};
    std::atomic<bool> m_installed{};
    std::atomic<bool> m_enabled{};
    std::atomic<uintptr_t> m_fixture_caller{};
    std::array<std::atomic<uintptr_t>,3> m_fixture_readers{};
    std::array<std::atomic<void*>,9> m_reader_originals{};
    std::atomic<bool> m_readers_installed{};
    std::atomic<bool> m_reader_failed{};
    std::atomic<uint64_t> m_reader_overrides{};
    uintptr_t m_module{};
    std::mutex m_install_mutex;
    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
};
