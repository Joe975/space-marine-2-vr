#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Opt-in, bounded observation of the native physics RigidBody velocity setter.
// The probe never writes memory. An explicit launch candidate may replace this
// call's temporary velocity through a separately ownership-checked callback.
class EngineRigidBodyVelocityProbe {
public:
    using Vector=std::array<float,3>;
    struct Api {
        std::function<bool(uintptr_t,void*,size_t)> read;
        std::function<std::optional<Vector>(uintptr_t)> vector;
        std::function<uintptr_t(uintptr_t)> user_data;
    };
    struct Property {uintptr_t address{};std::array<uintptr_t,12> words{};};
    struct Sample {uint64_t serial{};uintptr_t body{},handle{},owner_props{},system{},system_owner{},caller{};Vector velocity{};std::string name;std::vector<Property> properties;};
    struct Snapshot {uint64_t calls{},rejected{},overflow{};std::vector<Sample> samples;};
    static EngineRigidBodyVelocityProbe& get();
    bool install_in_game(HMODULE);
    bool install(void*);
    void uninstall();
    void bind(uintptr_t base,Api);
    void observe(uintptr_t body,uintptr_t velocity,uintptr_t caller=0);
    Snapshot snapshot()const;
    void set_launch_override(std::function<std::optional<Vector>(uintptr_t,uintptr_t,uintptr_t)> value){m_override=std::move(value);}
    // Sample capture (diagnostic). Off, the hook only runs the launch override.
    void set_observe(bool value){m_observe=value;}
private:
    template<class T>bool read(uintptr_t p,T& v)const{return p && m_api.read && m_api.read(p,&v,sizeof v);}
    uintptr_t q(uintptr_t p)const{uintptr_t v{};read(p,v);return v;}
    static void hook(uintptr_t,uintptr_t,uintptr_t);
    using Setter=void(*)(uintptr_t,uintptr_t,uintptr_t);
    uintptr_t m_base{};Api m_api;void* m_target{};Setter m_original{};
    mutable std::mutex m_mutex;Snapshot m_state;
    std::function<std::optional<Vector>(uintptr_t,uintptr_t,uintptr_t)> m_override;
    std::atomic<bool> m_observe{true};
};
