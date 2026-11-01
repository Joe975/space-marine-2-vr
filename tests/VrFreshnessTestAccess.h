#pragma once
#include "EngineBodyProbe.h"
#include "EngineArmProbe.h"
#include "VrControllerInput.h"
#include <future>

// This fixture checks the real publication mutex from a different thread.
// try_lock returns immediately; future::get synchronizes without timing/sleeps.
struct VrFreshnessTestAccess {
    static bool held(std::mutex& mutex) {
        return std::async(std::launch::async,[&]{
            if(!mutex.try_lock())return true;
            mutex.unlock();return false;
        }).get();
    }
    static bool held(const VrControllerInput& input){return held(input.m_mutex);}
    static bool held(const EngineBodyProbe& body){return held(body.m_mutex);}
    static bool held(const EngineArmProbe& arms){return held(arms.m_mutex);}
    static void clock(EngineBodyProbe& body,EngineBodyProbe::TimeSource value){body.m_clock=value;}
    static void clock(EngineArmProbe& arms,EngineArmProbe::TimeSource value){arms.m_clock=value;}
    static std::unique_lock<std::mutex> lock(EngineArmProbe& arms){return std::unique_lock{arms.m_mutex};}
    static void received_locked(EngineArmProbe& arms,EngineArmProbe::Clock::time_point time){arms.m_snapshot.pose->received=time;arms.m_snapshot.pose->owner.received=time;}
};
