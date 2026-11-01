#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// An explicit diagnostic name belongs to one completed native draw frame.
// Later images never substitute for a missed target. No GPU ownership is inferred.
class OrdinaryJoinedCapture {
public:
    struct Target {
        std::string id;
        uint64_t dispatch{},render_id{};
        bool matches(uint64_t d,uint64_t left,uint64_t right)const noexcept {
            return dispatch && render_id && dispatch==d && render_id==left && left==right;
        }
    };
    static OrdinaryJoinedCapture& get(){static OrdinaryJoinedCapture value;return value;}
    static bool safe(std::string_view id){return !id.empty() && id.size()<=40 && std::ranges::all_of(id,[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';});}
    bool reserve(std::string_view id,uint64_t dispatch,uint64_t render_id)noexcept {
        if(!safe(id) || !dispatch || !render_id)return false;
        try{std::lock_guard lock{m_mutex};if(m_count==m_slots.size())return false;
            for(size_t i=0;i<m_count;++i)if(m_slots[i].target.id==id || m_slots[i].target.dispatch==dispatch)return false;
            Slot next{{std::string{id},dispatch,render_id}};
            m_slots[m_count]=std::move(next);++m_count;m_active=true;return true;
        }catch(...){return false;}
    }
    bool complete(std::string_view id,uint64_t dispatch,uint64_t render_id)noexcept {
        try{std::lock_guard lock{m_mutex};for(size_t i=0;i<m_count;++i){auto& s=m_slots[i];
            if(s.target.id==id && s.target.matches(dispatch,render_id,render_id) && !s.claimed){s.complete=true;return true;}}
        }catch(...){}return false;
    }
    std::optional<Target> claim(uint64_t dispatch,uint64_t left,uint64_t right,bool submitted)noexcept {
        if(!submitted || !m_active)return {};
        try{std::lock_guard lock{m_mutex};for(size_t i=0;i<m_count;++i){auto& s=m_slots[i];
            if(!s.complete || s.claimed || !s.target.matches(dispatch,left,right))continue;
            std::optional<Target> result{s.target};s.claimed=true;return result;}
        }catch(...){}return {};
    }
private:
    struct Slot{Target target;bool complete{},claimed{};};
    std::mutex m_mutex;std::array<Slot,8> m_slots{};size_t m_count{};std::atomic_bool m_active{};
};
