#pragma once
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

// Profiling A/B only (SM2VR_STEREO_HOLD_CONTROL, set by run-ordinary-headset.ps1 -PerfCampaign).
// A one-word control file, polled once a second on the Present thread: "mono" holds the stereo
// producer in native mono (the game renders one view; the headset shows the flat panel), "stereo"
// releases it and the next job resumes through every normal pair check. Anything else, a partial
// write or a missing file keeps the current state. No file configured means always stereo.
class StereoHoldControl {
public:
    static std::optional<bool> parse(std::string_view line) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "mono") return true;
        if (line == "stereo") return false;
        return std::nullopt;
    }
    bool configure(const std::filesystem::path& path) {
        if (!path.is_absolute()) return false;
        m_path = path; return true;
    }
    bool configured() const { return !m_path.empty(); }
    bool held() const { return m_held; }
    // Returns the new state when it changed on this poll.
    std::optional<bool> poll(double now_ms) {
        if (m_path.empty() || now_ms < m_next) return std::nullopt;
        m_next = now_ms + 1000;
        std::ifstream file{m_path}; std::string line;
        if (!std::getline(file, line) || line.size() > 16) return std::nullopt;
        const auto value = parse(line);
        if (!value || *value == m_held) return std::nullopt;
        m_held = *value; return m_held;
    }
private:
    std::filesystem::path m_path; double m_next{}; bool m_held{};
};
