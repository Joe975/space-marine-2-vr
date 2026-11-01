#pragma once
#include <atomic>
#include "GameBuild.h"
#include "EngineBodyProbe.h"
#include "VrCameraPose.h"
#include <optional>
#include "VrWeaponAim.h"

// Passive CtrlTarget ray observation. Ownership follows SceneTransform parent trackers
// to the fresh player's exact root entity. Scene-container ownership is not actor identity.
class EngineWeaponProbe {
public:
    static constexpr uintptr_t RAY_RVA=0xe58010;
    static constexpr uintptr_t AIM_RVA=0xe55820;
    static constexpr game_build::Code PROLOGUE=game_build::hashed(0xe58010,16,"fa57e639f17b44352ccc338db58f0495e86fb599394a88f9788bc9170c24938d");
    static constexpr game_build::Code AIM_PROLOGUE=game_build::hashed(0xe55820,17,"b3b646796675382f28c3af764a361b8ea56e03c1f72856fb858b4aed921fab7b");
    struct Sample {
        uintptr_t control{},scene{},tracker{},actor{},active_actor{},caller{},world_method_rva{},scene_owner{},root_scene{};
        uint32_t parent_depth{};
        std::array<uintptr_t,12> tracker_words{};
        vr_camera::Matrix world{};
        std::array<float,3> target{};
        float max_distance{};
        uint64_t calls{};
        std::chrono::steady_clock::time_point received{};
        bool valid{},owned{};
        std::optional<VrWeaponAim::Pose> driven;
        bool overridden{};
    };
    struct AimRead { uintptr_t tracker{},caller{},method{},scene{};vr_camera::Matrix world{};bool valid{}; };
    struct AimSample {
        uintptr_t control{},controller{},actor{};
        vr_camera::Matrix input_target{};
        std::array<AimRead,8> reads{};
        uint32_t read_count{};
        uint64_t calls{};
        std::chrono::steady_clock::time_point received{};
        bool target_valid{},direction_only{},aimed{};
    };
    struct Snapshot { uint64_t calls{},worlds{},rejected{},overflow{},aim_calls{},aim_owned{},ray_overrides{},target_overrides{};std::vector<Sample> targets;std::optional<AimSample> aim; };
    static EngineWeaponProbe& get();
    static bool matches_build(uint32_t,std::span<const uint8_t>);
    bool install_in_game(HMODULE);
    bool install(void* ray,uintptr_t module,std::function<uintptr_t()> active_actor);
    bool install_aim(void* update);
    void enable(bool value) { m_enabled=value; }
    bool installed() const { return m_installed; }
    void uninstall();
    Snapshot snapshot() const;
    // StereoStall guard (OrdinaryStereoRecovery.h ReadinessStall): drop every control row so the current level's are learned afresh.
    void forget() {std::scoped_lock lock{m_mutex};m_snapshot.targets.clear();}
    VrWeaponAim& aim_driver() {return m_driver;}
    // Where the player's own aim ray hit (game world), from a ray at most VrWeaponAim's 100 ms old and
    // the actor still in control; the aim dot (VrAimDot) and the moved crosshair (VrHudReticle) use it.
    // steered: the ray came from the controller gun (the weapon driver), not the game's own aim. A steered
    // hit stands for 100 ms over newer unsteered rays: single rays miss the gun's pose (31 Oct: 95 alone in
    // play), and each would throw the crosshair to the game's aim, a median 61 degrees away.
    struct OwnedTarget { std::array<float,3> point{};bool steered{}; };
    std::optional<OwnedTarget> owned_target(std::chrono::steady_clock::time_point now) const;
private:
    static void ray_hook(void*);
    static void world_observer(uintptr_t,void*,float*,bool);
    static void aim_hook(void*,void*);
    void observe(Sample&);
    void resolve_owner(Sample&) const;
    using Ray=void(*)(void*);
    using Aim=void(*)(void*,void*);
    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
    std::atomic<bool> m_enabled{},m_installed{};
    Ray m_original{};
    Aim m_original_aim{};
    void* m_target{};void* m_aim_target{};
    uintptr_t m_module{};
    std::function<uintptr_t()> m_active_actor;
    VrWeaponAim m_driver;
    uintptr_t m_ray_scene{},m_ray_actor{};
    std::array<float,3> m_ray_target{},m_steered_target{};bool m_ray_steered{};
    std::chrono::steady_clock::time_point m_ray_received{},m_steered_received{};
    std::atomic<std::chrono::steady_clock::rep> m_ray_received_ticks{}; // m_ray_received for the lock-free check in world_observer.
};
