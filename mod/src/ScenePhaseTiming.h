#pragma once
#include "PerformanceTrace.h"
#include <array>
#include <functional>

// Passive partition of one native scene at its three known synchronization
// callsites. Includes all intervening work/waits; not active CPU or GPU time.
// TLS scopes restore their parent so nested native calls cannot corrupt it.
class ScenePhaseTiming {
public:
    using Observer=std::function<void(const char*,double,double,double)>;
    ScenePhaseTiming(unsigned eye,uint64_t dispatch,double start,bool enabled,
        Observer observer=[](const char* name,double begin,double duration,double value){PerformanceTrace::get().record(name,begin,duration,value);})
        :m_parent(s_active),m_eye(eye),m_dispatch(dispatch),m_last(start),m_enabled(enabled && eye<2 && dispatch),m_observer(std::move(observer)) {s_active=this;}
    ~ScenePhaseTiming(){s_active=m_parent;}
    ScenePhaseTiming(const ScenePhaseTiming&)=delete;
    static void wait(unsigned index,bool entering,double now){if(s_active)s_active->boundary(index,entering,now);}
    void finish(double now){
        if(!m_enabled || m_finished)return;m_finished=true;
        const bool complete=m_valid && m_stage==3 && !m_waiting;
        emit(complete?names[m_eye][3]:unknown[m_eye],now);
        if(complete)m_observer(m_eye?"scene_right_phase_complete":"scene_left_phase_complete",now,0,double(m_dispatch));
    }
private:
    void boundary(unsigned index,bool entering,double now){
        if(!m_enabled || m_finished)return;
        if(!m_valid || index>=3 || index!=m_stage || entering==m_waiting || now<m_last){m_valid=false;return;}
        if(entering){emit(names[m_eye][index],now);m_waiting=true;}
        else {m_last=now;m_waiting=false;++m_stage;}
    }
    void emit(const char* name,double now){if(now>=m_last)m_observer(name,m_last,now-m_last,double(m_dispatch));m_last=now;}
    inline static constexpr const char* names[2][4]{{"scene_left_before_depth_cpu","scene_left_depth_to_shadow_cpu","scene_left_shadow_to_third_cpu","scene_left_after_third_cpu"},
        {"scene_right_before_depth_cpu","scene_right_depth_to_shadow_cpu","scene_right_shadow_to_third_cpu","scene_right_after_third_cpu"}};
    inline static constexpr const char* unknown[2]{"scene_left_unclassified_tail_cpu","scene_right_unclassified_tail_cpu"};
    inline static thread_local ScenePhaseTiming* s_active{};
    ScenePhaseTiming* m_parent{};unsigned m_eye{},m_stage{};uint64_t m_dispatch{};double m_last{};
    bool m_enabled{},m_valid{true},m_waiting{},m_finished{};Observer m_observer;
};
