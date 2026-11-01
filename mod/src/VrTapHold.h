#pragma once
#include <chrono>

// One button, two actions: a tap acts on release as a bounded pulse, a hold acts once
// when it reaches HOLD while still down. A hold never also taps. Used by Left A
// (ability / recenter) and Left B (native Start / viewer mode).
class VrTapHold {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto HOLD=std::chrono::milliseconds(1000),PULSE=std::chrono::milliseconds(150);
    struct Result {bool hold{},tap{};};
    // armed: the button currently owns its action. Disarming cancels a press in progress
    // (its release sends nothing) and any pending pulse.
    // min_tap: a release sooner than this after the press is a brush, not a tap, and sends nothing.
    Result update(bool pressed,bool armed,Clock::time_point now,Clock::duration min_tap=Clock::duration::zero()) {
        Result result;
        if(!armed){m_pressed=pressed;m_consumed=true;m_pulse_until={};return result;}
        if(pressed && !m_pressed){m_down=now;m_consumed=false;}
        if(pressed && !m_consumed && now-m_down>=HOLD){m_consumed=true;result.hold=true;}
        if(!pressed && m_pressed && !m_consumed){m_consumed=true;if(now-m_down>=min_tap)m_pulse_until=now+PULSE;}
        m_pressed=pressed;
        result.tap=now<m_pulse_until;
        return result;
    }
    // Ends a pulse in progress, leaving the press state alone (a held button stays held).
    void end_pulse(){m_pulse_until={};}
    void reset(){m_pressed=false;m_consumed=true;m_pulse_until={};}
private:
    bool m_pressed{},m_consumed{true};
    Clock::time_point m_down{},m_pulse_until{};
};
