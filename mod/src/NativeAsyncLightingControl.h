#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include "GameBuild.h"

// Diagnostic for the second-eye light flicker (docs/RIGHT_EYE_FLICKER_2026-10-28.md).
// The clustered-lighting inputs (tiled depth min/max, the froxel light bitmask, the zbins) exist once
// on the renderer both Scenes share. Each Scene builds them on the async compute queue between named
// fences (ZDOWNSAMPLE_READY, ASYNC_DONE) that are looked up by name, not per Scene. If a wait is
// satisfied by the other Scene's signal, slot 1's lighting can read slot 0's or a half-built bitmask.
// Two native bytes move that work back onto the graphics queue:
//   EXE+0x4cb4075  tiled depth + light bitmask on async compute (native 1)
//   EXE+0x4cb73ee  async compute at all (native 1)
// (Reference-build RVAs, from the renderer reverse engineering in build/flicker_re/NOTES.md.)
// A one-word control file (SM2VR_DIAG_ASYNC_LIGHTING_CONTROL), polled once a second on the Present
// thread so the switch flips between frames: "native" (both 1), "graphics" (tiled byte 0) or
// "no_async" (both 0). Every write first checks that the byte holds a boolean and that the value
// seen at the first read was the native 1; anything else refuses and leaves memory alone.
namespace native_async_lighting {
// Not in GameBuildTable.inc (diagnostic only): rva() below explains how they are placed in a patched build.
constexpr uintptr_t TILED_SWITCH = 0x4cb4075, ASYNC_SWITCH = 0x4cb73ee;
enum class Mode { native, graphics, no_async };
enum class Result { applied, unchanged, unsupported_build, unexpected_value, write_failed };
inline const char* name(Mode m) { return m == Mode::native ? "native" : m == Mode::graphics ? "graphics" : "no_async"; }
inline const char* name(Result r) {
    switch (r) { case Result::applied: return "applied"; case Result::unchanged: return "unchanged";
        case Result::unsupported_build: return "unsupported_build"; case Result::unexpected_value: return "unexpected_value";
        default: return "write_failed"; }
}
inline std::optional<Mode> parse(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line == "native") return Mode::native;
    if (line == "graphics") return Mode::graphics;
    if (line == "no_async") return Mode::no_async;
    return std::nullopt;
}
// Byte values for a mode: {tiled, async}.
inline std::pair<uint8_t, uint8_t> values(Mode m) {
    return m == Mode::native ? std::pair<uint8_t, uint8_t>{1, 1} : m == Mode::graphics ? std::pair<uint8_t, uint8_t>{0, 1} : std::pair<uint8_t, uint8_t>{0, 0};
}
// This build's RVA of a switch. The build table has no entry for these two bytes; the data on either
// side (0x4cb4003, 0x4d47f00) maps to itself in every table build, so they are taken as unmoved there.
// The native-value check in apply() is the second guard.
inline uintptr_t rva(uintptr_t reference) {
    if (const auto r = game_build::rva(reference)) return r;
    return game_build::rva(0x4cb4003) == 0x4cb4003 && game_build::rva(0x4d47f00) == 0x4d47f00 ? reference : 0;
}
using Read = std::function<bool(uintptr_t, uint8_t&)>;
using Write = std::function<bool(uintptr_t, uint8_t)>;
class Switches {
public:
    // Sets both bytes for `mode` at base+offsets. Each byte must read as 0 or 1, and the first value
    // ever read must have been the native 1 (so a mode never writes over a value the game chose).
    Result apply(Mode mode, uintptr_t base, uintptr_t tiled, uintptr_t async, const Read& read, const Write& write) {
        if (!tiled || !async) return Result::unsupported_build;
        const auto [want_tiled, want_async] = values(mode);
        uint8_t now[2]{};
        const uintptr_t at[2]{base + tiled, base + async};
        const uint8_t want[2]{want_tiled, want_async};
        for (int i = 0; i < 2; ++i) {
            if (!read(at[i], now[i]) || now[i] > 1) return Result::unexpected_value;
            if (!m_first[i]) m_first[i] = now[i];
            if (*m_first[i] != 1) return Result::unexpected_value;
        }
        bool changed = false;
        for (int i = 0; i < 2; ++i) {
            if (now[i] == want[i]) continue;
            uint8_t after{};
            if (!write(at[i], want[i]) || !read(at[i], after) || after != want[i]) return Result::write_failed;
            changed = true;
        }
        return changed ? Result::applied : Result::unchanged;
    }
private:
    std::optional<uint8_t> m_first[2];
};
// The control file: returns the requested mode when it changed on this poll (once a second).
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
