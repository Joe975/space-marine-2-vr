#pragma once
// Optional render output size for headset play (docs/EYE_SIZE.md, SM2VR_OUTPUT_SIZE=WxH).
//
// Each eye renders at the game's output size. The game sizes its swap chain from the video
// config it hands the D3D12 swap chain wrapper (config +0x18 width, +0x1c height), with
// DXGI_SCALING_STRETCH, so the desktop window just shows a stretched copy. The saved display
// settings cannot be changed from a direct launch (the 21 Oct E2 run), so the mod sets the size:
//   config  (default) - in the swap chain create (ref 0x2110800) and resize (ref 0x2110f60)
//                       calls and in VideoConfig::LoadFromConfigVars (ref 0x2088e50), so every
//                       config built from the engine variables carries the size, and is made
//                       windowed at that size (the device takes its output from the window size
//                       in windowed mode and from the desktop in borderless: see apply_windowed);
//   device            - only the device's own config copy. Diagnostic only: Joe's 21 Oct
//                       campaign run showed the renderer's colour targets and DLSS input stay at
//                       the 4K config, so the 1944-wide eye was the left half of a 3840-wide
//                       image (2x horizontal zoom). Check runs with scripts/check_output_size_run.py.
// All three addresses go through game_build and are verified per build; anything else fails
// closed (no hook). Installed from the proxy's init thread, before the game creates its device.
#include "GameBuild.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

namespace engine_output_size {

struct Size { uint32_t width{}, height{}; };
enum class Mode { device, config };

// Strict "WxH": digits only, no leading zeros, even, 640x360 to 4096x2160 (the mod's eye-image cap).
inline std::optional<Size> parse(std::string_view text)
{
    const auto x = text.find('x');
    if (x == std::string_view::npos || x == 0 || x + 1 >= text.size()) return std::nullopt;
    const auto number = [](std::string_view part) -> std::optional<uint32_t> {
        if (part.empty() || part.size() > 4 || part[0] == '0') return std::nullopt;
        uint32_t value{};
        for (const char c : part) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + static_cast<uint32_t>(c - '0');
        }
        return value;
    };
    const auto w = number(text.substr(0, x)), h = number(text.substr(x + 1));
    if (!w || !h || *w < 640 || *w > 4096 || *h < 360 || *h > 2160 || (*w & 1) || (*h & 1)) return std::nullopt;
    return Size{*w, *h};
}

inline std::optional<Mode> parse_mode(std::string_view text)
{
    if (text.empty() || text == "config") return Mode::config;
    if (text == "device") return Mode::device;
    return std::nullopt;
}

inline constexpr uint32_t WIDTH_OFFSET = 0x18, HEIGHT_OFFSET = 0x1c;

struct Change { uint32_t old_width{}, old_height{}; bool written{}; };

// Writes the size into a video config. Refuses values that do not look like an output size
// (an unexpected layout must never be overwritten).
inline std::optional<Change> apply(uint8_t* config, Size size)
{
    if (!config) return std::nullopt;
    uint32_t width{}, height{};
    std::memcpy(&width, config + WIDTH_OFFSET, sizeof width);
    std::memcpy(&height, config + HEIGHT_OFFSET, sizeof height);
    if (width < 320 || width > 16384 || height < 180 || height > 16384) return std::nullopt;
    Change change{width, height, width != size.width || height != size.height};
    if (change.written) {
        std::memcpy(config + WIDTH_OFFSET, &size.width, sizeof size.width);
        std::memcpy(config + HEIGHT_OFFSET, &size.height, sizeof size.height);
    }
    return change;
}

// The device decides its output size from the window mode, not from Video.OutputSizeX/Y
// (Device::ApplyVideoMode, patched 0x209b7e0; docs/EYE_SIZE.md):
//   0 windowed   -> Video.WindowSizeX/Y (config +0x20/+0x24)
//   1 borderless -> the current display's desktop mode (EnumDisplaySettings)
//   2 fullscreen -> the configured resolution, falling back to borderless
// Evidence for the values: the window placement code (patched 0x20a2d00) moves a styled window to
// the window size for 0, makes a WS_POPUP window of the monitor size otherwise, and HWND_TOPMOST
// only for 2; the apply code calls exclusive mode for 2 and stores 1 when that fails. The SSL
// DisplayMode enum has the same order (WINDOWED, BORDERLESS, FULL_SCREEN). The config loader
// fills +0x20/+0x24 from Video.WindowSizeX/Y and +0x30 from Video.WindowMode.
inline constexpr uint32_t WINDOW_WIDTH_OFFSET = 0x20, WINDOW_HEIGHT_OFFSET = 0x24, WINDOW_MODE_OFFSET = 0x30;
inline constexpr uint32_t WINDOWED = 0, BORDERLESS = 1, FULLSCREEN = 2;

struct WindowChange { uint32_t old_mode{}, old_width{}, old_height{}; bool written{}; };

// Makes a loaded config windowed at `size`, so the device's output (and every eye) is that size.
// Refuses a mode outside 0..2 or an implausible window size (unexpected layout).
inline std::optional<WindowChange> apply_windowed(uint8_t* config, Size size)
{
    if (!config) return std::nullopt;
    uint32_t mode{}, width{}, height{};
    std::memcpy(&mode, config + WINDOW_MODE_OFFSET, sizeof mode);
    std::memcpy(&width, config + WINDOW_WIDTH_OFFSET, sizeof width);
    std::memcpy(&height, config + WINDOW_HEIGHT_OFFSET, sizeof height);
    if (mode > FULLSCREEN || width > 16384 || height > 16384) return std::nullopt;
    WindowChange change{mode, width, height, mode != WINDOWED || width != size.width || height != size.height};
    if (change.written) {
        constexpr uint32_t windowed = WINDOWED;
        std::memcpy(config + WINDOW_MODE_OFFSET, &windowed, sizeof windowed);
        std::memcpy(config + WINDOW_WIDTH_OFFSET, &size.width, sizeof size.width);
        std::memcpy(config + WINDOW_HEIGHT_OFFSET, &size.height, sizeof size.height);
    }
    return change;
}

// Reference-build (2026-10-05) code at the three functions; game_build translates and verifies.
inline constexpr uintptr_t CREATE_RVA = 0x2110800;  // d3d_swapchain_12.cpp:92 caller, CreateSwapChainForHwnd
inline constexpr game_build::Code CREATE_CODE=game_build::hashed(0x2110800,38,"5fe0eaf0c3cb0208953b12151a7d0fbd40eea5cd9342eab5439934881377e0c3");
inline constexpr uintptr_t RESIZE_RVA = 0x2110f60;  // ResizeBuffers(3, config+0x18, config+0x1c, ...)
inline constexpr game_build::Code RESIZE_CODE=game_build::hashed(0x2110f60,28,"b3882a557611a23bcb232ea97b39d69530bbb75f6529b0decb8bdd089885cf58");
inline constexpr uintptr_t LOAD_RVA = 0x2088e50;    // VideoConfig from Video.OutputSizeX/Y, WindowSize, WindowMode ...
inline constexpr game_build::Code LOAD_CODE=game_build::hashed(0x2088e50,27,"1b843cb778e9de5164d553b4265e9f7d7b08d7e72186b5da6871950225148272");

enum class Target { create, resize, load };
inline const char* name(Target t) { return t == Target::create ? "create" : t == Target::resize ? "resize" : "load"; }
inline size_t code_length(Target t) { return t == Target::create ? CREATE_CODE.size() : t == Target::resize ? RESIZE_CODE.size() : LOAD_CODE.size(); }
// Address of the target in the running build; 0 when the build or address is not verified.
inline uintptr_t rva(uint32_t timestamp, Target t)
{
    if (t == Target::create) return game_build::rva(timestamp, CREATE_RVA);
    if (t == Target::resize) return game_build::rva(timestamp, RESIZE_RVA);
    return game_build::rva(timestamp, LOAD_RVA);
}
inline bool matches(uint32_t timestamp, Target t, std::span<const uint8_t> actual)
{
    if (t == Target::create) return game_build::code(timestamp, CREATE_RVA, actual, CREATE_CODE);
    if (t == Target::resize) return game_build::code(timestamp, RESIZE_RVA, actual, RESIZE_CODE);
    return game_build::code(timestamp, LOAD_RVA, actual, LOAD_CODE);
}

struct Snapshot {
    bool requested{}, installed{};
    Size size{};
    Mode mode{Mode::config};
    uint64_t creates{}, resizes{}, loads{}, refused{}, windowed{};
    Change last{};
    WindowChange window{};
};

class Hook {
public:
    static Hook& get();
    // Reads SM2VR_OUTPUT_SIZE / SM2VR_OUTPUT_SIZE_MODE; installs nothing when unset or invalid.
    void install_from_environment();
    Snapshot snapshot() const { std::lock_guard lock{m_mutex}; return m_snapshot; }
    std::string status() const;

private:
    using CreateFn = bool(void*, void*, void*, uint32_t, void*, void*, uint8_t*);
    using ResizeFn = void(void*, uint8_t*, uint32_t);
    using LoadFn = void(uint8_t*);
    static bool on_create(void*, void*, void*, uint32_t, void*, void*, uint8_t*);
    static void on_resize(void*, uint8_t*, uint32_t);
    static void on_load(uint8_t*);
    void record(uint8_t* config, uint64_t Snapshot::*counter);
    std::atomic<CreateFn*> m_create{};
    std::atomic<ResizeFn*> m_resize{};
    std::atomic<LoadFn*> m_load{};
    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
};

} // namespace engine_output_size
