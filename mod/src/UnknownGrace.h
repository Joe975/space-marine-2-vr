#pragma once
#include <chrono>

// The rule for every gate that refuses while a state is unknown (docs/GENERIC_GUARDS_2026-10-26.md,
// piece 5). Refusing during a brief unknown is right: it is usually a change in progress (a weapon
// switch) and acting on it would be a guess. Refusing forever is a deadlock when the action refused
// is the one that would make the state known: 25 Oct in the battle simulator the held weapon was
// never learned, so the draw that would put the gun in a hand was refused, and so were the swings
// and the trigger. update() says when the unknown has lasted long enough that the gate should act
// on its best default instead. Pure; one per gated state.
class UnknownGrace {
public:
    using Clock=std::chrono::steady_clock;
    explicit UnknownGrace(std::chrono::milliseconds grace):m_grace(grace){}
    // known: the state is known on this poll. True once it has been unknown for the grace.
    bool update(bool known,Clock::time_point now){
        if(known){m_since={};return false;}
        if(m_since==Clock::time_point{} || now<m_since)m_since=now;
        return now-m_since>=m_grace;
    }
    void reset(){m_since={};}
    std::chrono::milliseconds grace()const{return m_grace;}
private:
    std::chrono::milliseconds m_grace;Clock::time_point m_since{};
};
