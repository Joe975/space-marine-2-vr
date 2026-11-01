#pragma once
#include "NativeOrdinaryTextureCreation.h"
#include "NativeTemporalProbe.h"
#include "OrdinaryTaaHistory.h"
#include <atomic>
#include <mutex>

// Stereo under the game's TAA (docs/TAA_STEREO.md). The temporal pass is the one the owned bloom already
// splits: the same selector and pass, pool targets 0x16/0x17 (TAA_PREV_FRAME_0/1) instead of 0xc4/0xc5,
// and the per-eye bank word next to bloom's. The right eye takes a private pair at the selector; the
// bank then carries it to the next job as that eye's previous texture, so no bank or pool is edited.
// NativeOrdinaryBloom's selector hook and NativeTemporalProbe's pass hook call in here.
class NativeOrdinaryTaa {
public:
    using Scope=NativeTemporalProbe::Scope;
    static constexpr uint32_t EVEN=0x16,ODD=0x17;
    static NativeOrdinaryTaa& get(){static auto* value=new NativeOrdinaryTaa;return *value;}
    void enable(bool value){m_enabled.store(value,std::memory_order_release);}
    bool enabled()const{return m_enabled.load(std::memory_order_acquire);}
    // Every selection of the TAA targets, mono jobs too: the game is running TAA. DLSS never selects them.
    void selected(){m_calls.fetch_add(1,std::memory_order_relaxed);}
    // A job's selections for OrdinaryDlssPresence: none while off, or while no private pair can be had.
    uint64_t take_calls(){const auto calls=m_calls.exchange(0,std::memory_order_relaxed);if(!enabled())return 0;std::lock_guard lock{m_mutex};return calls && m_history.wait_job()?0:calls;}
    // The right eye's selection: its private texture, or 0 for the game's own (a bootstrap).
    uintptr_t select(const Scope& scope,uintptr_t renderer,uintptr_t base){
        if(!enabled() || scope.scene.slot!=1 || renderer!=scope.scene.renderer)return 0;
        std::lock_guard lock{m_mutex};auto& creation=NativeOrdinaryTextureCreation::taa();
        const auto parity=scope.scene.render_counter_before&1u;auto owned=creation.owned_pair();
        if(owned && base){
            const auto observed=native_texture_contract::capture_pair(base,{},{EVEN,ODD},scope.scene.render_counter_before);
            // A rebuilt pool (class change, level load, render size): the pair no longer fits. Renew it
            // and bootstrap; the temporal hook then takes a pair made for this layout, usually a retired one.
            if(const auto difference=native_texture_contract::difference(observed,owned->contract)){
                m_difference=difference;if(native_texture_contract::rebuilt(difference) && creation.renew(observed))++m_renewals;owned.reset();
            }
        }
        const auto destination=owned?owned->native[parity]:uintptr_t{};
        m_history.begin(scope.scene.dispatch_sequence,parity,destination);return destination;
    }
    // The previous texture the right eye's pass samples.
    uintptr_t previous(NativeTemporalProbe::Event* event,const std::array<uintptr_t,7>& args){
        if(!enabled() || args[6]!=0 || !event || event->scope.scene.slot!=1)return args[3];
        std::lock_guard lock{m_mutex};const auto dispatch=event->scope.scene.dispatch_sequence;
        if(m_history.owned(dispatch))event->controlled=true;
        return m_history.previous(dispatch,args[3]);
    }
    void after_temporal(const NativeTemporalProbe::Event& event){
        if(!enabled() || event.mode!=0 || event.scope.scene.slot!=1)return;std::lock_guard lock{m_mutex};
        m_history.temporal(event.scope.scene.dispatch_sequence,event.refusal==NativeTemporalProbe::Refusal::none,event.arguments[2]);
    }
    bool accepted(uint64_t dispatch)const{if(!enabled())return false;std::lock_guard lock{m_mutex};return m_history.accepted(dispatch);}
    struct Status {OrdinaryTaaHistory::Stats history;uint64_t renewals{};uint32_t difference{};bool stalled{};};
    Status status()const{std::lock_guard lock{m_mutex};return {m_history.stats(),m_renewals,m_difference,m_history.stalled()};}
private:
    std::atomic_bool m_enabled{};std::atomic_uint64_t m_calls{};mutable std::mutex m_mutex;
    OrdinaryTaaHistory m_history;uint64_t m_renewals{};uint32_t m_difference{};
};
