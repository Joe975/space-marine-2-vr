#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <algorithm>

// Argument POD from successful actual slGetNewFrameToken returns. Never
// dereference a retained object address; lookups compare a current API argument.
class SdkFrameTokens {
public:
    struct Returned {
        uint64_t ordinal{},epoch{};uintptr_t object{};uint32_t token{},thread{};
        std::optional<uint32_t> index;
        bool operator==(const Returned&)const=default;
    };
    void returned(uintptr_t object,uint32_t token,std::optional<uint32_t> index,uint32_t thread,bool accepted){
        std::lock_guard lock{m_mutex};++m_ordinal;
        if(!accepted || !object || !thread){m_values.clear();++m_epoch;return;}
        for(const auto& value:m_values)if(value.object==object && value.token==token && value.index==index)return;
        std::erase_if(m_values,[&](const auto& value){return value.object==object;});
        if(m_values.size()==8)m_values.pop_front();m_values.push_back({m_ordinal,m_epoch,object,token,thread,index});
    }
    std::optional<Returned> match(uintptr_t object,uint32_t token)const{
        std::lock_guard lock{m_mutex};std::optional<Returned> result;unsigned same_token{};
        for(const auto& value:m_values){if(value.token==token)++same_token;if(value.object==object && value.token==token)result=value;}
        return same_token==1?result:std::nullopt;
    }
private:
    mutable std::mutex m_mutex;std::deque<Returned> m_values;uint64_t m_ordinal{},m_epoch{1};
};
