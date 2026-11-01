#pragma once
#include "PerformanceTrace.h"
#include <array>
#include <algorithm>
#include <functional>

// Passive, bounded evidence. A matching key means a CPU lookup of a column
// published by this Scene's collector, not proof that older results are safe.
class SceneQueryDependencyTiming {
public:
    struct Snapshot {uintptr_t manager{};std::array<uint64_t,64> keys{};unsigned count{},captured{};bool valid{};};
    using Observer=std::function<void(const char*,double,double,double)>;
    inline static bool enabled{}; // Set before render callbacks, never changed live.
    SceneQueryDependencyTiming(unsigned eye,uint64_t dispatch,bool admitted,Observer observer=[](const char* name,double start,double duration,double value){PerformanceTrace::get().record(name,start,duration,value);})
        :m_parent(s_active),m_eye(eye),m_dispatch(dispatch),m_enabled(admitted && eye<2 && dispatch),m_observer(std::move(observer)){s_active=this;}
    ~SceneQueryDependencyTiming(){s_active=m_parent;}
    static bool active(){return s_active && s_active->m_enabled;}
    static void fence(double now,int gc){if(active()){s_active->m_fence=now;s_active->m_gc=gc;}}
    static void collect_begin(const Snapshot& snapshot,double now){if(active()){auto& s=*s_active;++s.m_collects;s.m_snapshot=snapshot;if(s.m_snapshot.captured>s.m_snapshot.keys.size() || s.m_snapshot.count<s.m_snapshot.captured){s.m_snapshot.valid=false;s.m_snapshot.captured=0;}s.m_collect_start=now;s.m_published=0;}}
    static void collect_end(double now){if(active() && s_active->m_collect_start)s_active->m_published=now;}
    static void consume(uintptr_t manager,uintptr_t object,int column,int32_t result,uintptr_t caller,double now){
        if(!active())return;auto& s=*s_active;++s.m_reads;
        if(!s.m_published || now<s.m_published){++s.m_before;return;}
        if(!s.m_snapshot.valid || manager!=s.m_snapshot.manager || column<0 || column>7)return;
        const auto key=(uint64_t(object)&~uint64_t(7))|uint64_t(column);
        if(std::find(s.m_snapshot.keys.begin(),s.m_snapshot.keys.begin()+s.m_snapshot.captured,key)==s.m_snapshot.keys.begin()+s.m_snapshot.captured)return;
        ++s.m_matches;
        if(!s.m_first){s.m_first=now;s.m_caller=caller;s.m_result=result;s.m_column=column;}
    }
    void finish(double now){
        if(m_finished || !m_enabled)return;m_finished=true;
        const auto emit=[&](const char* left,const char* right,double start,double duration,double value){m_observer(m_eye?right:left,start,duration,value);};
        emit("scene_left_dependency_summary","scene_right_dependency_summary",now,0,double(m_dispatch));
        emit("scene_left_dependency_collects","scene_right_dependency_collects",now,0,m_collects);
        emit("scene_left_dependency_reads","scene_right_dependency_reads",now,0,m_reads);
        emit("scene_left_dependency_reads_before","scene_right_dependency_reads_before",now,0,m_before);
        emit("scene_left_dependency_matched_reads","scene_right_dependency_matched_reads",now,0,m_matches);
        emit("scene_left_dependency_gc_pending","scene_right_dependency_gc_pending",now,0,m_gc);
        emit("scene_left_dependency_query_count","scene_right_dependency_query_count",now,0,m_snapshot.valid?double(m_snapshot.count):-1);
        emit("scene_left_dependency_keys_captured","scene_right_dependency_keys_captured",now,0,m_snapshot.captured);
        if(m_collects==1 && m_first && m_published>=m_collect_start && m_collect_start>=m_fence && m_fence){
            emit("scene_left_dependency_collect_cpu","scene_right_dependency_collect_cpu",m_collect_start,m_published-m_collect_start,double(m_dispatch));
            emit("scene_left_dependency_publish_to_use_cpu","scene_right_dependency_publish_to_use_cpu",m_published,m_first-m_published,double(m_dispatch));
            emit("scene_left_dependency_consumer_rva","scene_right_dependency_consumer_rva",m_first,0,double(m_caller));
            emit("scene_left_dependency_consumer_result","scene_right_dependency_consumer_result",m_first,0,m_result);
            emit("scene_left_dependency_consumer_column","scene_right_dependency_consumer_column",m_first,0,m_column);
        }
    }
private:
    inline static thread_local SceneQueryDependencyTiming* s_active{};
    SceneQueryDependencyTiming* m_parent{};unsigned m_eye{};uint64_t m_dispatch{};bool m_enabled{},m_finished{};Observer m_observer;
    Snapshot m_snapshot;unsigned m_collects{},m_reads{},m_before{},m_matches{};int m_gc{-1},m_column{};int32_t m_result{};uintptr_t m_caller{};
    double m_fence{},m_collect_start{},m_published{},m_first{};
};
