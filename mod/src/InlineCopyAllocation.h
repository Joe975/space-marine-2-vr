#pragma once
#include <d3d12.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

// An optional file supports same-process committed-versus-pooled A/B.
class InlineCopyAllocation {
public:
    using Clock=std::chrono::steady_clock;
    enum class Mode {committed,pooled};
    static std::optional<Mode> parse(std::string_view value) {
        if(value=="committed")return Mode::committed;
        if(value=="pooled")return Mode::pooled;
        return {};
    }
    bool control(const std::filesystem::path& path) {
        if(!path.empty() && !path.is_absolute())return false;
        m_path=path;m_next={};m_mode=Mode::pooled;return true;
    }
    Mode mode(Clock::time_point now=Clock::now()) {
        if(!m_path.empty() && now>=m_next) {
            m_next=now+std::chrono::seconds(1);std::ifstream file{m_path};std::string line;
            if(std::getline(file,line) && line.size()<=16) {
                if(!line.empty() && line.back()=='\r')line.pop_back();
                if(const auto value=parse(line))m_mode=*value;
            }
        }
        return m_mode;
    }
private:
    std::filesystem::path m_path;Clock::time_point m_next{};
    Mode m_mode{Mode::pooled};
};
