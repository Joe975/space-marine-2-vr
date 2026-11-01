#include "synthetic_code.h"
#include "EngineOutputSize.h"
#include "game_build_ported.h"
#include <array>
#include <utility>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace engine_output_size;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void parsing() {
    const auto ok = parse("1944x2160");
    require(ok && ok->width == 1944 && ok->height == 2160, "eye-shaped size parses");
    require(parse("640x360") && parse("4096x2160"), "inclusive bounds");
    for (const char* bad : {"", "1944", "1944x", "x2160", "1944X2160", " 1944x2160", "01944x2160", "1945x2160", "1944x2161",
                            "638x360", "640x358", "4098x2160", "1944x2162", "1944x2160x1", "-1944x2160", "19a4x2160"}) {
        require(!parse(bad), bad);
    }
    require(parse_mode("") == Mode::config && parse_mode("device") == Mode::device && parse_mode("config") == Mode::config,
            "config is the default: device-only left the renderer at 4K (cropped, zoomed eyes)");
    require(!parse_mode("Device") && !parse_mode("both"), "unknown modes refused");
}

std::array<uint8_t, 0x40> config(uint32_t width, uint32_t height) {
    std::array<uint8_t, 0x40> c{};
    std::memset(c.data(), 0xab, c.size());
    std::memcpy(c.data() + WIDTH_OFFSET, &width, 4);
    std::memcpy(c.data() + HEIGHT_OFFSET, &height, 4);
    return c;
}
uint32_t word(const std::array<uint8_t, 0x40>& c, size_t offset) { uint32_t v{}; std::memcpy(&v, c.data() + offset, 4); return v; }

void applying() {
    auto c = config(3840, 2160);
    const auto before = c;
    const auto change = apply(c.data(), {1944, 2160});
    require(change && change->written && change->old_width == 3840 && change->old_height == 2160, "writes over a 4K output");
    require(word(c, WIDTH_OFFSET) == 1944 && word(c, HEIGHT_OFFSET) == 2160, "new size stored");
    for (size_t i = 0; i < c.size(); ++i)
        if (i < WIDTH_OFFSET || i >= HEIGHT_OFFSET + 4) require(c[i] == before[i], "only the two size fields change");
    const auto again = apply(c.data(), {1944, 2160});
    require(again && !again->written, "already the size: no write");
    constexpr std::pair<uint32_t, uint32_t> implausible[]{{0u, 2160u}, {3840u, 0u}, {100u, 2160u}, {3840u, 99999u}, {0xababababu, 0xababababu}};
    for (const auto& [w, h] : implausible) {
        auto bad = config(w, h);
        const auto copy = bad;
        require(!apply(bad.data(), {1944, 2160}) && bad == copy, "implausible layout is never overwritten");
    }
    require(!apply(nullptr, {1944, 2160}), "null config");
}

void windowed() {
    // A loaded VideoConfig: output +0x18/+0x1c, window size +0x20/+0x24, window mode +0x30.
    std::array<uint8_t, 0x48> c{};
    std::memset(c.data(), 0xcd, c.size());
    const auto put = [&](size_t offset, uint32_t v) { std::memcpy(c.data() + offset, &v, 4); };
    const auto get = [&](size_t offset) { uint32_t v{}; std::memcpy(&v, c.data() + offset, 4); return v; };
    put(WIDTH_OFFSET, 3840); put(HEIGHT_OFFSET, 2160); put(WINDOW_WIDTH_OFFSET, 3840); put(WINDOW_HEIGHT_OFFSET, 2160); put(WINDOW_MODE_OFFSET, BORDERLESS);
    const auto before = c;
    const auto change = apply_windowed(c.data(), {1944, 2160});
    require(change && change->written && change->old_mode == BORDERLESS && change->old_width == 3840, "borderless 4K becomes windowed");
    require(get(WINDOW_MODE_OFFSET) == WINDOWED && get(WINDOW_WIDTH_OFFSET) == 1944 && get(WINDOW_HEIGHT_OFFSET) == 2160, "window mode and size written");
    for (size_t i = 0; i < c.size(); ++i) {
        const bool field = (i >= WINDOW_WIDTH_OFFSET && i < WINDOW_HEIGHT_OFFSET + 4) || (i >= WINDOW_MODE_OFFSET && i < WINDOW_MODE_OFFSET + 4);
        if (!field) require(c[i] == before[i], "only window mode and size change (output size is apply()'s job)");
    }
    require(!apply_windowed(c.data(), {1944, 2160})->written, "already windowed at the size: no write");
    put(WINDOW_MODE_OFFSET, FULLSCREEN); require(apply_windowed(c.data(), {1944, 2160})->old_mode == FULLSCREEN, "fullscreen too");
    put(WINDOW_WIDTH_OFFSET, 0); put(WINDOW_HEIGHT_OFFSET, 0); put(WINDOW_MODE_OFFSET, BORDERLESS);
    require(apply_windowed(c.data(), {1944, 2160}).has_value(), "an unset (zero) window size is fine");
    for (const uint32_t bad_mode : {3u, 7u, 0xcdcdcdcdu}) {
        put(WINDOW_MODE_OFFSET, bad_mode);
        const auto copy = c;
        require(!apply_windowed(c.data(), {1944, 2160}) && c == copy, "unknown window mode: layout not trusted, nothing written");
    }
    put(WINDOW_MODE_OFFSET, BORDERLESS); put(WINDOW_WIDTH_OFFSET, 99999);
    require(!apply_windowed(c.data(), {1944, 2160}), "implausible window size refused");
    require(!apply_windowed(nullptr, {1944, 2160}), "null");
    require(WINDOWED == 0 && BORDERLESS == 1 && FULLSCREEN == 2, "engine window mode values");
}

void builds() {
    using game_build::REFERENCE_EXE;
    require(matches(REFERENCE_EXE, Target::create, SYNTH(CREATE_CODE)) && matches(REFERENCE_EXE, Target::resize, SYNTH(RESIZE_CODE)) &&
            matches(REFERENCE_EXE, Target::load, SYNTH(LOAD_CODE)), "reference build code matches");
    auto changed = SYNTH(RESIZE_CODE);
    changed[0] ^= 1;
    require(!matches(REFERENCE_EXE, Target::resize, changed), "different code refused");
    require(!matches(0x12345678, Target::create, SYNTH(CREATE_CODE)), "unknown build refused");
    // Patched build of 2026-10-21: addresses moved, the table's verified bytes match.
    const auto ported = game_build_ported::EXE;
    require(rva(ported, Target::create) == 0x2110b40 && rva(ported, Target::resize) == 0x21112a0 &&
            rva(ported, Target::load) == 0x2089190 && rva(REFERENCE_EXE, Target::create) == CREATE_RVA && rva(0x12345678, Target::load) == 0, "ported addresses");
    require(matches(ported, Target::create, game_build_ported::code(CREATE_RVA)) && matches(ported, Target::resize, game_build_ported::code(RESIZE_RVA)) &&
            matches(ported, Target::load, game_build_ported::code(LOAD_RVA)), "ported code matches within the verified prefix");
    require(!matches(ported, Target::load, SYNTH(LOAD_CODE)), "the reference rip displacements must not pass on the patched build");
}
}

int main() {
    try {
        parsing();
        applying();
        windowed();
        builds();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "engine_output_size tests passed\n";
    return 0;
}
