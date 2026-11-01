#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include <memory>
#include <string>
#include "LockedClockRead.h"

// Player-scoped, read-only observation of Morpheme's final pose publication.
// A matching camera actor is required; proximity, mesh names and vtables are not ownership.
class EngineArmProbe {
public:
    using Clock = std::chrono::steady_clock;
    using TimeSource=Clock::time_point(*)();
    explicit EngineArmProbe(TimeSource clock=Clock::now):m_clock(clock) {}
    using Matrix = std::array<float,16>;
    static constexpr uintptr_t UPDATE_RVA=0xe46dc0, PUBLISH_RVA=0xe31800;
    struct Owner { uintptr_t actor{},entity{},controller{},wrapper{},network{},definition{},instance{}; Clock::time_point received{}; };
    struct Bone { uint32_t rig_index{}; int16_t skin_index{}; Matrix local{}; };
    struct RigLayout { std::vector<std::string> names; std::vector<int32_t> parents; std::vector<Matrix> bind_locals; };
    struct Pose { Owner owner; uint16_t animset{}; uintptr_t rig{},mapping{},buffer{}; std::vector<Bone> bones;
        std::shared_ptr<const RigLayout> layout; Matrix world{}; bool world_valid{}; Clock::time_point received{};
        float world_scale{1}; }; // The entity world's uniform scale, divided out of world (vr_camera::unscaled); 1 when rigid.
    // world_scaled: captures whose entity world carried a uniform scale (accepted, unscaled).
    // world_refused: captures whose entity world was read but refused; refused_world is the latest.
    struct Snapshot { uint64_t updates{},matched{},publications{},captures{},rejected{}; std::optional<Pose> pose;
        uint64_t world_scaled{},world_refused{}; float last_world_scale{1}; std::optional<Matrix> refused_world; };
    static EngineArmProbe& get();
    static bool matches_build(uint32_t,std::span<const uint8_t>,std::span<const uint8_t>);
    bool install_in_game(HMODULE);
    bool install(void* update,void* publish,std::function<uintptr_t(Clock::time_point)> actor,std::function<uintptr_t()> actor_current={});
    void enable(bool);
    bool installed() const { return m_installed; }
    void uninstall(); // Quiescent teardown only.
    Snapshot snapshot() const { return snapshot_current(m_clock); }
    Snapshot snapshot_current(TimeSource clock=Clock::now) const;
    Snapshot snapshot(Clock::time_point now) const;
    void observe_controller(void*,Clock::time_point now=Clock::now());
    void observe_publication(void*,uint64_t result,Clock::time_point now=Clock::now());
    void set_pose_observer(std::function<void(const Pose&)>);
    bool current(const Pose& pose) const { return current_current(pose,m_clock); }
    bool current_current(const Pose&,TimeSource clock=Clock::now) const;
    bool current(const Pose&,Clock::time_point now) const;
private:
    friend struct VrFreshnessTestAccess;
    Snapshot snapshot_locked(Clock::time_point,std::optional<uintptr_t> actor_current={}) const;
    TimeSource m_clock;
    using Update=void(*)(void*,void*);
    using Publish=uint64_t(*)(void*);
    static void update_hook(void*,void*);
    static uint64_t publish_hook(void*);
    std::optional<Owner> owner(uintptr_t,Clock::time_point,std::optional<uintptr_t> actor_current={}) const;
    bool pose_current_locked(const Pose&) const;
    static std::shared_ptr<const RigLayout> rig_layout(uintptr_t,uint32_t);
    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
    std::optional<Owner> m_owner;
    uintptr_t m_ambiguous_actor{};
    Clock::time_point m_ambiguous_received{};
    std::function<uintptr_t(Clock::time_point)> m_actor;
    std::function<uintptr_t()> m_actor_current;
    std::function<void(const Pose&)> m_pose_observer;
    std::atomic<bool> m_enabled{},m_installed{};
    Update m_update{}; Publish m_publish{};
    void* m_update_target{}; void* m_publish_target{};
};
