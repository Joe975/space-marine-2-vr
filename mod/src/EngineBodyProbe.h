#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <vector>
#include <functional>
#include "LockedClockRead.h"

// Read-only observation on native simulation/tracker threads. No nearest-actor inference
// and no writes: candidates are evidence until an authoritative ownership link is verified.
class EngineBodyProbe {
public:
    using TimeSource=std::chrono::steady_clock::time_point(*)();
    explicit EngineBodyProbe(TimeSource clock=std::chrono::steady_clock::now):m_clock(clock) {}
    static constexpr uint32_t BUILD_TIMESTAMP = 0x6ac3e716;
    static constexpr uintptr_t BODY_UPDATE_RVA = 0x5623d0, TRACKER_WORLD_RVA = 0x171d2f0;
    struct Body {
        uintptr_t body{}, actor{}, entity{}, body_vtable{};
        std::array<float,3> position{}, forward{}, requested_position{};
        uint64_t calls{};
        std::chrono::steady_clock::time_point received{};
    };
    struct Tracker {
        uintptr_t object{}, caller_rva{}, vtable{}, world_method_rva{}, owner_actor{};
        std::array<uintptr_t,44> words{};
        std::array<float,16> world{};
        bool valid{};
        uint64_t calls{};
        std::chrono::steady_clock::time_point received{};
    };
    struct Snapshot { uint64_t updates{}, tracker_calls{}, rejected{}, overflow{}, evicted{}; std::vector<Body> bodies; std::vector<Tracker> trackers; };
    static EngineBodyProbe& get();
    static bool matches_build(uint32_t,std::span<const uint8_t>,std::span<const uint8_t>);
    bool install_in_game(HMODULE);
    bool install(void* body_update,void* tracker_world,uintptr_t module_base,size_t module_size);
    void enable(bool value) { m_enabled = value; }
    bool installed() const { return m_installed; }
    void uninstall(); // Only during quiescent fixture teardown / game process teardown.
    Snapshot snapshot() const;
    // StereoStall guard (OrdinaryStereoRecovery.h ReadinessStall): drop every row so the current level's objects are learned afresh.
    void forget() { std::scoped_lock lock{m_mutex}; m_snapshot.bodies.clear(); m_snapshot.trackers.clear(); ++m_forgets; }
    uint64_t forgets() const { std::scoped_lock lock{m_mutex}; return m_forgets; }
    uintptr_t active_actor() const { return active_actor_current(m_clock); }
    uintptr_t active_actor_current(TimeSource clock) const;
    uintptr_t active_actor(std::chrono::steady_clock::time_point now) const;
    void set_body_observer(std::function<void(const Body&)> observer);
    // A scoped weapon override may replace this call's temporary output after native work.
    using WorldObserver=void(*)(uintptr_t caller,void* tracker,float* world,bool valid);
    void set_world_observer(WorldObserver value) { m_world_observer=value; }
    // The caller filter is public to exercise the same production collection path in fixtures.
    void observe_body(void*,const void* update_arguments);
    void observe_tracker(uintptr_t caller,void*,const float* world,bool valid);
private:
    friend struct VrFreshnessTestAccess;
    TimeSource m_clock;
    uintptr_t active_actor_locked(std::chrono::steady_clock::time_point now) const;
    using Update = void(*)(void*,void*);
    using World = uint8_t(*)(void*,float*);
    static void update_hook(void*,void*);
    static uint8_t world_hook(void*,float*);
    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
    uint64_t m_forgets{};
    std::function<void(const Body&)> m_body_observer;
    std::atomic<bool> m_enabled{},m_installed{};
    std::atomic<WorldObserver> m_world_observer{};
    Update m_original_update{};
    World m_original_world{};
    void* m_update_target{};
    void* m_world_target{};
    uintptr_t m_base{};
    size_t m_size{};
};
