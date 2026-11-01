#pragma once
#include <string>
#include <string_view>
#include <algorithm>
#include <array>

// A bounded diagnostic request names a new subdirectory, never an arbitrary path.
// 24 per process: one parity sweep (scripts/capture_eye_parity.py) is 4 stations x 4 variants.
class EyeDumpRequest {
public:
    static constexpr unsigned LIMIT=24;
    bool take(std::string text,bool pair_ready) {
        while(!text.empty() && (text.back()=='\r' || text.back()=='\n')) text.pop_back();
        if(!pair_ready || m_count>=LIMIT || text.empty() || text.size()>40 || std::ranges::find(m_seen,text)!=m_seen.end() ||
            !std::ranges::all_of(text,[](unsigned char c) {return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';})) return false;
        m_seen[m_count]=text;m_last=std::move(text);++m_count;return true;
    }
    const std::string& id() const {return m_last;}
private:
    std::string m_last;std::array<std::string,LIMIT> m_seen{};unsigned m_count{};
};
