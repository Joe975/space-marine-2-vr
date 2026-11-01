#pragma once
#include "PerformanceTrace.h"
#include <functional>
#include <optional>

// Running thread CPU time between the third wait and DLSS entry. Windows thread
// accounting is quantized: use aggregates, never subtract/clamp each frame to
// manufacture an off-CPU duration. Includes kernel/driver CPU, excludes other jobs.
class SceneExecutionTiming {
public:
    struct Counter{uint64_t cpu_100ns{};DWORD thread{};};
    using Reader=std::function<std::optional<Counter>()>;
    using Observer=std::function<void(const char*,double,double,double)>;
    static std::optional<Counter> read(){
        FILETIME created{},exited{},kernel{},user{};
        if(!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))return {};
        const auto ticks=[](FILETIME time){return (uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;};
        return Counter{ticks(kernel)+ticks(user),GetCurrentThreadId()};
    }
    SceneExecutionTiming(unsigned eye,uint64_t dispatch,bool enabled,Reader reader=read,
        Observer observer=[](const char* name,double start,double duration,double value){PerformanceTrace::get().record(name,start,duration,value);})
        :m_parent(s_active),m_eye(eye),m_dispatch(dispatch),m_enabled(enabled && eye<2 && dispatch),m_reader(std::move(reader)),m_observer(std::move(observer)){s_active=this;}
    ~SceneExecutionTiming(){s_active=m_parent;}
    static void third_wait_end(double now){if(s_active)s_active->begin(now);}
    static void sdk_entry(double now){if(s_active)s_active->end(now);}
    void finish(){
        if(m_finished)return;m_finished=true;
        if(!m_enabled || !m_valid || m_entries!=1 || !m_begin || !m_end || m_end->thread!=m_begin->thread || m_end->cpu_100ns<m_begin->cpu_100ns || m_end_wall<m_start)return;
        m_observer(m_eye?"scene_right_pre_sdk_wall":"scene_left_pre_sdk_wall",m_start,m_end_wall-m_start,double(m_dispatch));
        m_observer(m_eye?"scene_right_pre_sdk_thread_cpu":"scene_left_pre_sdk_thread_cpu",m_start,double(m_end->cpu_100ns-m_begin->cpu_100ns)/10000.,double(m_dispatch));
    }
private:
    void begin(double now){if(!m_enabled)return;if(m_begin || m_entries){m_valid=false;return;}m_start=now;m_begin=m_reader();if(!m_begin)m_valid=false;}
    void end(double now){if(!m_enabled)return;++m_entries;if(m_entries!=1 || !m_begin){m_valid=false;return;}m_end_wall=now;m_end=m_reader();}
    inline static thread_local SceneExecutionTiming* s_active{};
    SceneExecutionTiming* m_parent{};unsigned m_eye{},m_entries{};uint64_t m_dispatch{};bool m_enabled{},m_valid{true},m_finished{};
    Reader m_reader;Observer m_observer;std::optional<Counter> m_begin,m_end;double m_start{},m_end_wall{};
};
