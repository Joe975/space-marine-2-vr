#pragma once
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// Diagnostic (docs/RIGHT_EYE_FLICKER_2026-10-28.md, SM2VR_DIAG_LIGHT_LIST_LOG): which local lights
// enter and leave each view's light list in the light job (0x1ef8dd0) and why. Light-job layout from
// build/flicker_re/NOTES.md (reference build):
//   light instance L (the occlusion query's object): desc = [L+0x18]; "visible" flag for view v =
//   byte L+0x90+v (cleared, then set once the light passes the occlusion threshold);
//   desc +0x30 position (3 floats), +0x6c type, +0x104 range squared (float), +0x20b shadow-capable.
//   Output per view: element array lm+0x8580+v*0x1ef08 (0x210 bytes each, a desc copy, position at
//   +0x30), count int at lm+0x27480+v*0x1ef08, at most 0xf0. Camera position: E+0x64, E = list+8+v*0xa60.
// The decision after the query: q under the threshold drops the light (flag stays 0); a
// shadow-capable light that was not visible last time is NOT appended this frame when inside its
// range, and is appended beyond its range only if it is in the far-light cache (0x1e8a900).
// The probe only reads. Per (light, view) it keeps the list membership and query history and, every
// `window` jobs, reports the lights that entered or left a view's list most often.
class LightListProbe {
public:
    enum class Reason { kept, occlusion, newly_visible_in_range, beyond_range_not_cached, other };
    static const char* name(Reason r){
        switch(r){case Reason::kept:return "kept";case Reason::occlusion:return "occlusion";case Reason::newly_visible_in_range:return "new_in_range";
            case Reason::beyond_range_not_cached:return "far_not_cached";default:return "other";}
    }
    // Why a queried light is (not) in the view's list this frame.
    static Reason classify(bool in_list,bool flag_after,bool prev,bool shadow,float dist2,float range2,bool in_cache){
        if(in_list)return Reason::kept;
        if(!flag_after)return Reason::occlusion;
        if(shadow && !prev && dist2<range2)return Reason::newly_visible_in_range;
        if(shadow && !prev && !in_cache)return Reason::beyond_range_not_cached;
        return Reason::other;
    }
    struct Facts { uintptr_t light{}; int view{}; int32_t own{}, other{}; bool flag_after{}, in_list{}, in_cache{}, shadow{}; int32_t type{}; float dist2{}, range2{}; };
    // Feed one job's facts (one per queried light and view); returns report lines every `window` jobs.
    std::vector<std::string> job(const std::vector<Facts>& facts,uint32_t window=60,size_t max_lines=6){
        ++m_jobs;
        for(const auto& f:facts){
            if(f.view<0 || f.view>1)continue;
            auto& s=m_state[key(f.light,f.view)];
            const auto reason=classify(f.in_list,f.flag_after,s.prev_flag,f.shadow,f.dist2,f.range2,f.in_cache);
            if(s.seen && f.in_list!=s.in_list){++s.flips;++m_view_flips[f.view];++m_reason_flips[f.view][size_t(f.in_list?s.last_reason:reason)];}
            s.in_list=f.in_list;s.prev_flag=f.flag_after;s.seen=true;s.last=f;if(!f.in_list)s.last_reason=reason;
            s.list_bits=(s.list_bits<<1)|(f.in_list?1u:0u);s.q[s.qpos++%s.q.size()]=f.own;
        }
        std::vector<std::string> lines;
        if(m_jobs%window)return lines;
        lines.push_back(summary(window));
        std::vector<std::pair<uint64_t,const State*>> top;
        for(const auto& [k,s]:m_state)if(s.flips>=4)top.push_back({s.flips,&s});
        std::sort(top.begin(),top.end(),[](const auto& a,const auto& b){return a.first>b.first;});
        for(size_t i=0;i<top.size() && i<max_lines;++i)lines.push_back(describe(*top[i].second));
        for(auto& [k,s]:m_state)s.flips=0;
        m_view_flips={};m_reason_flips={};
        if(m_state.size()>8192)m_state.clear();
        return lines;
    }
private:
    struct State {
        bool seen{},in_list{},prev_flag{};uint64_t flips{},list_bits{};Reason last_reason{Reason::other};Facts last;
        std::array<int32_t,16> q{};size_t qpos{};
    };
    static uint64_t key(uintptr_t light,int view){return uint64_t(light)^uint64_t(view);}
    std::string summary(uint32_t window)const{
        std::string out="LightList: jobs="+std::to_string(m_jobs)+" window="+std::to_string(window);
        for(int v=0;v<2;++v){
            out+=" view"+std::to_string(v)+" flips="+std::to_string(m_view_flips[v])+" [";
            for(size_t r=1;r<5;++r)out+=std::string(r>1?" ":"")+name(Reason(r))+"="+std::to_string(m_reason_flips[v][r]);
            out+="]";
        }
        return out;
    }
    static std::string describe(const State& s){
        const auto& f=s.last;char head[256];
        std::snprintf(head,sizeof head,"LightFlip: view=%d light=%#llx type=%d shadow=%d dist2=%.0f range2=%.0f in_cache=%d flips=%llu last_drop=%s own=%d other=%d list=",
            f.view,(unsigned long long)f.light,f.type,int(f.shadow),f.dist2,f.range2,int(f.in_cache),(unsigned long long)s.flips,name(s.last_reason),f.own,f.other);
        std::string out=head;
        for(int i=31;i>=0;--i)out+=(s.list_bits>>i)&1?'1':'0';
        out+=" q=";
        for(size_t i=0;i<s.q.size();++i){const auto v=s.q[(s.qpos+i)%s.q.size()];out+=std::to_string(v);if(i+1<s.q.size())out+=',';}
        return out;
    }
    std::unordered_map<uint64_t,State> m_state;uint64_t m_jobs{};
    std::array<uint64_t,2> m_view_flips{};std::array<std::array<uint64_t,5>,2> m_reason_flips{};
};
