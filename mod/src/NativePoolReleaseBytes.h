#pragma once
#include "GameBuild.h"
// The whole shipping pool release function (535 bytes at 0x2079230), checked by digest before the
// lookup mirrors it.
namespace native_pool_membership {
inline constexpr game_build::Code RELEASE_BYTES=game_build::hashed(0x2079230,535,"b346a5fad67726d920fb2e496c1c37a9799998875c68a1edc0da154041ec9fe5");
}
