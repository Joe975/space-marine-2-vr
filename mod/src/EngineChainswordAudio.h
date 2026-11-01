#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

// Own Wwise object and playing IDs. Never stops or changes a game-owned emitter.
// Caller serializes access; engine calls enqueue work on Wwise's audio thread.
class EngineChainswordAudio {
public:
    struct Api {
        std::function<bool()> ready;
        std::function<bool(uint64_t)> register_object;
        std::function<uint32_t(const char*,uint64_t)> post;
        std::function<void(uint32_t)> stop;
        std::function<void(uint64_t)> unregister_object;
        std::function<bool(uint64_t,uint32_t)> playing;
        std::function<int(uint64_t)> listeners;
        std::function<bool(uint64_t,float)> volume; // Optional own-emitter gain.
    };
    struct Snapshot {bool bound{},registered{},held{},playing{};int listeners{-1};uint32_t loop{},tail{};uint64_t starts{},stops{},failures{};float gain{-1};};
    // Rev loop gain follows trigger depth: audible from the first touch, full at full trigger.
    static constexpr float MIN_GAIN=.2f;
    static float gain(float amount){return MIN_GAIN+(1-MIN_GAIN)*std::clamp(amount,0.f,1.f);}
    // Applies only while our loop is playing; repeated equal values are not re-sent.
    void intensity(float amount) {
        if(!m_state.bound || !m_state.loop || !m_api.volume || !std::isfinite(amount))return;
        const float value=gain(amount);if(std::abs(value-m_state.gain)<.01f)return;
        if(m_api.volume(object(),value))m_state.gain=value;
    }
    void bind(Api api) {reset();m_api=std::move(api);m_state={};m_state.bound=m_api.ready && m_api.register_object && m_api.post && m_api.stop && m_api.unregister_object;}
    void update(bool held) {
        if(!m_state.bound)return;
        if(!m_api.ready()){m_state.registered=false;m_state.playing=false;m_state.listeners=-1;m_state.loop=m_state.tail=0;m_state.held=held;return;}
        m_state.playing=m_state.loop && m_api.playing && m_api.playing(object(),m_state.loop);
        if(m_state.registered && m_api.listeners)m_state.listeners=m_api.listeners(object());
        if(held==m_state.held)return;
        m_state.held=held;
        if(held){
            if(m_state.tail){m_api.stop(m_state.tail);m_state.tail=0;}
            if(!m_state.registered)m_state.registered=m_api.register_object(object());
            if(!m_state.registered){++m_state.failures;return;}
            m_state.loop=m_api.post("wpn_melee_chswd_charge_loop_start",object());m_state.gain=-1;
            if(m_state.loop)++m_state.starts;else ++m_state.failures;
        }else if(m_state.loop){
            m_api.stop(m_state.loop);m_state.loop=0;m_state.playing=false;++m_state.stops;
            m_state.tail=m_api.post("wpn_melee_chswd_charge_loop_stop",object());
        }
    }
    void reset() {
        if(m_state.bound && m_api.ready()){
            if(m_state.loop)m_api.stop(m_state.loop);
            if(m_state.tail)m_api.stop(m_state.tail);
            if(m_state.registered)m_api.unregister_object(object());
        }
        m_state={};
    }
    Snapshot snapshot()const{return m_state;}
    uint64_t object()const{return reinterpret_cast<uint64_t>(this);}
private:
    Api m_api;Snapshot m_state;
};
