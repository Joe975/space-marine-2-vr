#pragma once
#include "NativeRenderProbe.h"
#include <deque>
#include "GameBuild.h"
#include "NativeTextureContract.h"
#include "NativeGlobalHistoryContract.h"
#include "NativeOrdinarySceneScope.h"
#include <atomic>
#include <mutex>

// Observes native calls. The separate opt-in bloom controller may reset the
// actual predecessor; v2 exports both observed and invoked arguments explicitly.
// Resource addresses/descriptors are POD; no cached COM queries, AddRef or leases.
class NativeTemporalProbe {
public:
    static constexpr uintptr_t RVA=0x1f95610;
    inline static constexpr game_build::Code BYTES=game_build::hashed(0x1f95610,16,"e60bc0f195cdfb5bdceee7a57cba744caf60efcdbff835911fb66eb15946246c");
    inline static constexpr std::array<uintptr_t,3> CALLS{0x180a643,0x180a849,0x180a70c}; // Indexed by native mode 0/1/2.
    inline static constexpr std::array<game_build::Code,3> CALL_BYTES{game_build::hashed(0x180a643,5,"7fc63838d6e08d7deef9f58102fb7af9d3bc89683423cfdb005b4e49ff47534e"),
        game_build::hashed(0x180a849,5,"7c4f2cc27fc5fb0d2cec30fa22347cc43284feb902233ca95c377d37cbd7cb38"),
        game_build::hashed(0x180a70c,5,"a8fab586e4ec97b2f29d7d025611054342bf87491101a715cddecb3ef092f920")};
    using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint8_t);
    using Scope=ordinary_scene_scope::Scope;
    using Provider=std::function<std::optional<Scope>()>;
    enum class Refusal:uint32_t {none,scope,mode,callsite,bank,changed,resource_changed,exception};
    struct State {uint32_t counter{};uintptr_t active{},previous{},next{};std::array<uintptr_t,11> previous_bank{},next_bank{};std::array<uintptr_t,5> inputs{};bool operator==(const State&)const=default;};
    struct Event {
        uint64_t serial{};Scope scope;uint32_t thread{};uint8_t mode{};uintptr_t caller{};
        std::array<uintptr_t,7> arguments{};Refusal refusal{Refusal::none};std::optional<State> before,after;
        bool controlled{};std::array<uintptr_t,7> invoked_arguments{};
        std::array<NativeRenderProbe::Output,9> invoked_outputs_before,invoked_outputs_after;
        std::array<NativeRenderProbe::Output,9> outputs_before,outputs_after;
        std::optional<native_texture_contract::Snapshot> texture_contract_before,texture_contract_after;
        std::optional<native_global_history::Snapshot> global_history_before,global_history_after;
    };
    struct Sample {bool installed{};uint64_t refused{},errors{},dropped{};std::vector<Event> events;};
    static NativeTemporalProbe& get();
    static bool matches(uint32_t,std::span<const uint8_t>,const std::array<std::array<uint8_t,5>,3>&);
    // The native temporal call for `mode` returns just after its 5-byte call at CALLS[mode],
    // translated to the running build (a reference RVA would refuse every call on a ported build).
    static bool expected_callsite(uint8_t mode,uintptr_t caller_rva){
        if(mode>2)return false;const auto site=game_build::rva(CALLS[mode]);return site && caller_rva==site+5;
    }
    bool install_in_game(HMODULE,NativeRenderProbe::OutputObserver);
    bool install(void*,Provider,NativeRenderProbe::OutputObserver,uintptr_t image_base=0,uintptr_t contract_base=0,bool global_history=false);
    Sample sample(uint64_t dispatch)const;
    static void write(std::ostream&,const Sample&);
    static void write_global_history(std::ostream&,const Sample&);
    bool observes_global_history()const{return m_global_history;}
private:
    static void on_native(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint8_t);
    std::optional<State> state(const Scope&,uintptr_t renderer,uintptr_t descriptors,uint8_t mode)const;
    std::array<NativeRenderProbe::Output,9> outputs(const std::array<uintptr_t,7>&,const State&)const;
    native_texture_contract::Snapshot texture_contract(const std::array<uintptr_t,7>&,const State&)const;
    void store(Event);
    std::atomic<Native> m_original{};Provider m_provider;NativeRenderProbe::OutputObserver m_outputs;uintptr_t m_base{},m_contract_base{};
    bool m_global_history{};
    mutable std::mutex m_mutex;std::mutex m_install;std::deque<Event> m_events;uint64_t m_serial{},m_refused{},m_errors{},m_dropped{};
};
