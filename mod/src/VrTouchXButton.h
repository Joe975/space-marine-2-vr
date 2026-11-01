#pragma once
#include "VrTapHold.h"

// Left X on Touch controllers (Quest, Rift S; docs/QUEST_STEAM_LINK.md). Touch has no trackpad,
// so X takes over the Index's left trackpad top: native Y held for as long as X is held. That is
// what the Assault leap (Y held a fifth of a second) and aimed abilities (aim while held, fire on
// release) need. Recenter, the Index's Left A hold, moves to X and Y pressed together and held
// for HOLD. X waits CHORD before it holds Y, so the X of a chord never uses the ability; a tap
// released sooner is a PULSE on release, as on the Index, and a hold lasts at least PULSE.
// chord tells the caller to cancel Y's own tap and hold.
class VrTouchXButton {
public:
    using Clock=VrTapHold::Clock;
    static constexpr auto HOLD=VrTapHold::HOLD,PULSE=VrTapHold::PULSE,CHORD=std::chrono::milliseconds(100);
    struct Result {bool recenter{},ability{},chord{};};
    // armed: the buttons currently own their actions. Disarming cancels a press in progress; an
    // X already held when arming must be released first.
    Result update(bool x,bool y,bool armed,Clock::time_point now) {
        Result result;
        if(!armed){m_state=x?State::spent:State::idle;m_x=x;m_pulse_until={};return result;}
        if(x && !m_x){m_from=now;m_state=y?State::chord:State::pending;}
        m_x=x;
        switch(m_state) {
        case State::pending:
            if(!x){m_state=State::idle;m_pulse_until=now+PULSE;}
            else if(y){m_state=State::chord;m_from=now;}
            else if(now-m_from>=CHORD){m_state=State::held;m_pulse_until=now+PULSE;} // A release just after still reaches the game.
            break;
        case State::held: if(!x) m_state=State::idle; break;
        case State::chord:
            if(!x || !y) m_state=x?State::spent:State::idle;
            else if(now-m_from>=HOLD){result.recenter=true;m_state=State::spent;}
            break;
        case State::spent: if(!x) m_state=State::idle; break;
        case State::idle: break;
        }
        result.ability=m_state==State::held || now<m_pulse_until;
        result.chord=m_state==State::chord;
        return result;
    }
    void reset(){m_state=State::idle;m_x=false;m_pulse_until={};}
private:
    enum class State {idle,pending,held,chord,spent};
    State m_state{State::idle};
    bool m_x{};
    Clock::time_point m_from{},m_pulse_until{};
};
