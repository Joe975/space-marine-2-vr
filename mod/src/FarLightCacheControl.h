#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Diagnostic for the second-eye light flicker (docs/RIGHT_EYE_FLICKER_2026-10-28.md).
// The far-light shadow cache (object 0x4f80d80 in the reference build) is ranked from one camera
// and re-rendered on age timers. A light in it gets a different occlusion test (against its cached
// shadow map) from one that isn't (the plain proxy), so its result can change type in a cycle while
// nothing moves (build/flicker_re/NOTES.md). Byte cache+0x1b1d8 = 1 disables the cache: the light
// job then uses the plain test for every light. Side effect: shadow-capable lights beyond their
// range are no longer kept. A one-word control file ("native" | "off"), polled once a second on the
// Present thread. The cache address per build: reference 0x4f80d80; 0x6ad10fcb 0x4f80c60 (ported
// from the LEA before the call to 0x1e8aa30 in 0x1efa550, build/gamebin images; not in the table).
namespace far_light_cache {
constexpr uint32_t REFERENCE_BUILD = 0x6ac3e716, PATCHED_BUILD = 0x6ad10fcb;
constexpr uintptr_t DISABLE_OFFSET = 0x1b1d8;
inline uintptr_t cache_rva(uint32_t timestamp) {
    return timestamp == REFERENCE_BUILD ? 0x4f80d80 : timestamp == PATCHED_BUILD ? 0x4f80c60 : 0;
}
enum class Mode { native, off };
enum class Result { applied, unchanged, unsupported_build, unexpected_value, write_failed };
inline const char* name(Result r) {
    switch (r) { case Result::applied: return "applied"; case Result::unchanged: return "unchanged";
        case Result::unsupported_build: return "unsupported_build"; case Result::unexpected_value: return "unexpected_value";
        default: return "write_failed"; }
}
inline std::optional<Mode> parse(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line == "native") return Mode::native;
    if (line == "off") return Mode::off;
    return std::nullopt;
}
class Switch {
public:
    // Byte must read 0 or 1 and have been the native 0 at the first read; writes off=1 / native=0.
    Result apply(Mode mode, uintptr_t at, const std::function<bool(uintptr_t, uint8_t&)>& read, const std::function<bool(uintptr_t, uint8_t)>& write) {
        if (!at) return Result::unsupported_build;
        uint8_t now{};
        if (!read(at, now) || now > 1) return Result::unexpected_value;
        if (!m_first) m_first = now;
        if (*m_first != 0) return Result::unexpected_value;
        const uint8_t want = mode == Mode::off ? 1 : 0;
        if (now == want) return Result::unchanged;
        uint8_t after{};
        if (!write(at, want) || !read(at, after) || after != want) return Result::write_failed;
        return Result::applied;
    }
private:
    std::optional<uint8_t> m_first;
};
class Control {
public:
    bool configure(const std::filesystem::path& path) { if (!path.is_absolute()) return false; m_path = path; return true; }
    bool configured() const { return !m_path.empty(); }
    Mode mode() const { return m_mode; }
    std::optional<Mode> poll(double now_ms) {
        if (m_path.empty() || now_ms < m_next) return std::nullopt;
        m_next = now_ms + 1000;
        std::ifstream file{m_path}; std::string line;
        if (!std::getline(file, line) || line.size() > 16) return std::nullopt;
        const auto value = parse(line);
        if (!value || *value == m_mode) return std::nullopt;
        m_mode = *value; return m_mode;
    }
private:
    std::filesystem::path m_path; double m_next{}; Mode m_mode{Mode::native};
};
}
