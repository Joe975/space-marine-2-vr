#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Candidate fix for the second-eye light flicker (docs/RIGHT_EYE_FLICKER_2026-10-28.md).
// When a view has more than 240 candidate local lights, the light job (0x1ef8dd0) keeps the 240
// with the highest stored occlusion result (a pixel count, mode byte BL = 1 set by `MOV BL,1` at
// 0x1ef9b5b), then re-sorts those by distance. The pixel counts jitter by a few pixels every frame,
// so near the cut-off lights swap in and out of the list every frame: one-frame light flips in that
// view only, even at a still view (build/flicker_re/NOTES.md). In stereo the second view goes over
// the cap at views where the first doesn't. With "distance" in the control file the immediate of
// that instruction is 0, so the cap keeps the 240 nearest lights instead, a key that holds still.
// The same 8 bytes (JLE +4; MOV BL,1; JMP +2; XOR BL,BL) are at 0x1ef9b59 in the reference build
// and 0x1ef9e99 in 0x6ad10fcb (+0x340, build/gamebin images); the write is checked against them.
namespace light_cap_sort {
constexpr uint32_t REFERENCE_BUILD = 0x6ac3e716, PATCHED_BUILD = 0x6ad10fcb;
constexpr std::array<uint8_t, 8> NATIVE{0x7e, 0x04, 0xb3, 0x01, 0xeb, 0x02, 0x32, 0xdb};
constexpr size_t IMMEDIATE = 3;  // the 01 of MOV BL,1
inline uintptr_t site_rva(uint32_t timestamp) {
    return timestamp == REFERENCE_BUILD ? 0x1ef9b59 : timestamp == PATCHED_BUILD ? 0x1ef9e99 : 0;
}
enum class Mode { native, distance };
enum class Result { applied, unchanged, unsupported_build, unexpected_bytes, write_failed };
inline const char* name(Result r) {
    switch (r) { case Result::applied: return "applied"; case Result::unchanged: return "unchanged";
        case Result::unsupported_build: return "unsupported_build"; case Result::unexpected_bytes: return "unexpected_bytes";
        default: return "write_failed"; }
}
inline std::optional<Mode> parse(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line == "native") return Mode::native;
    if (line == "distance") return Mode::distance;
    return std::nullopt;
}
using Bytes = std::array<uint8_t, 8>;
// The site must hold the native bytes or the native bytes with the immediate 0; writes the immediate.
inline Result apply(Mode mode, uintptr_t at, const std::function<bool(uintptr_t, Bytes&)>& read, const std::function<bool(uintptr_t, uint8_t)>& write) {
    if (!at) return Result::unsupported_build;
    Bytes now{};
    if (!read(at, now)) return Result::unexpected_bytes;
    Bytes patched = NATIVE; patched[IMMEDIATE] = 0;
    if (now != NATIVE && now != patched) return Result::unexpected_bytes;
    const uint8_t want = mode == Mode::distance ? 0 : 1;
    if (now[IMMEDIATE] == want) return Result::unchanged;
    Bytes after{};
    if (!write(at + IMMEDIATE, want) || !read(at, after) || after[IMMEDIATE] != want) return Result::write_failed;
    return Result::applied;
}
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
