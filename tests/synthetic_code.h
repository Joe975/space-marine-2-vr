#pragma once
// Tests: the synthetic stand-in for a module's expected game code (game_build::testing). The repo
// holds only digests of the real code, so module suites run their guards against these bytes.
#include "GameBuild.h"
#include <array>

#define SYNTH(code) (game_build::testing::array<(code).size()>(code))

// An array of codes (all the same length), as the module's std::array<std::array<uint8_t,N>,M>.
template <size_t N, size_t M>
std::array<std::array<uint8_t, N>, M> synthetic_all(const std::array<game_build::Code, M>& codes)
{
    std::array<std::array<uint8_t, N>, M> out{};
    for (size_t i = 0; i < M; ++i) out[i] = game_build::testing::array<N>(codes[i]);
    return out;
}
#define SYNTH_ALL(codes) (synthetic_all<(codes)[0].size()>(codes))

// The synthetic code at an RVA, for a module whose expected code is private to its .cpp.
template <size_t N> std::array<uint8_t, N> synthetic_at(uintptr_t reference, game_build::Image image = game_build::Image::exe)
{
    return game_build::testing::array<N>(game_build::Code{uint32_t(reference), uint16_t(N), {}, image});
}
