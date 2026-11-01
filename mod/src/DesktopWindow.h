#pragma once
// Optional smaller desktop window for headset play (docs/EYE_SIZE.md).
//
// The game creates its swap chain and resizes it at the configured *output* size
// (Video.OutputSizeX/Y, d3d_swapchain_12.cpp) with DXGI_SCALING_STRETCH, and every eye
// renders at that output size. In windowed mode the window can therefore be smaller
// than the output: DWM stretches the image into it and the eyes keep their size.
// SM2VR_DESKTOP_WINDOW=WxH asks for that client size once, after the first present.
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace desktop_window {

struct Size { uint32_t width{}, height{}; };

// Strict "WxH": digits only, no sign, spaces or leading zeros, 320x180 to 3840x2160.
inline std::optional<Size> parse(std::string_view text) {
    const auto x = text.find('x');
    if (x == std::string_view::npos || x == 0 || x + 1 >= text.size()) return std::nullopt;
    const auto number = [](std::string_view part) -> std::optional<uint32_t> {
        if (part.empty() || part.size() > 4 || part[0] == '0') return std::nullopt;
        for (const char c : part) if (c < '0' || c > '9') return std::nullopt;
        uint32_t value{};
        std::from_chars(part.data(), part.data() + part.size(), value);
        return value;
    };
    const auto w = number(text.substr(0, x)), h = number(text.substr(x + 1));
    if (!w || !h || *w < 320 || *w > 3840 || *h < 180 || *h > 2160) return std::nullopt;
    return Size{*w, *h};
}

enum class Decision { apply, already, fullscreen, borderless, maximized, larger_than_output };

// Only ever shrinks a plain captioned window: never fullscreen, never a borderless or maximized window, and
// never larger than the output (that would upscale on the desktop for no benefit).
inline Decision decide(Size requested, Size client, Size output, bool fullscreen, bool captioned, bool maximized) {
    if (fullscreen) return Decision::fullscreen;
    if (!captioned) return Decision::borderless;
    if (maximized) return Decision::maximized;
    if (requested.width > output.width || requested.height > output.height) return Decision::larger_than_output;
    if (requested.width == client.width && requested.height == client.height) return Decision::already;
    return Decision::apply;
}

inline const char* name(Decision d) {
    switch (d) {
    case Decision::apply: return "apply";
    case Decision::already: return "already";
    case Decision::fullscreen: return "refused_fullscreen";
    case Decision::borderless: return "refused_borderless";
    case Decision::maximized: return "refused_maximized";
    case Decision::larger_than_output: return "refused_larger_than_output";
    }
    return "unknown";
}

} // namespace desktop_window
