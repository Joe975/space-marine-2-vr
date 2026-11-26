#pragma once
// Test helper: synthetic game code (game_build::testing) at a reference-build RVA as ported to the
// newest table build (2026-11-27, Steam build 26944055) by GameBuildTable.inc: the stream with the entry's delta, for its verified
// length. tests/test_game_build.cpp keeps checking the 2026-10-21 build too. Module suites feed these to their own matchers to check that each module asks the table
// for the right address and that its comparison window fits inside the verified prefix.
#include "GameBuild.h"
#include <stdexcept>
#include <vector>

namespace game_build_ported {
inline constexpr uint32_t EXE = 0x6afcca92;
inline constexpr uint32_t DLL = 0x6afccec4;
inline std::vector<uint8_t> code(uintptr_t reference, game_build::Image image = game_build::Image::exe)
{
    auto out = game_build::testing::ported(image == game_build::Image::dll ? DLL : EXE, reference, image);
    if (out.empty()) throw std::runtime_error{"reference RVA missing from the ported table"};
    return out;
}
}
