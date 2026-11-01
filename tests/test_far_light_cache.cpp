#include "FarLightCacheControl.h"
#include <cstdlib>
#include <iostream>
#include <map>
using namespace far_light_cache;
void check(bool ok, const char* label) { if (!ok) { std::cerr << label << '\n'; std::exit(1); } }
int main() {
    check(parse("off") == Mode::off && parse("native\r") == Mode::native && !parse("") && !parse("of"), "control words");
    check(cache_rva(REFERENCE_BUILD) == 0x4f80d80 && cache_rva(PATCHED_BUILD) == 0x4f80c60 && cache_rva(PATCHED_BUILD + 1) == 0, "known builds only");
    std::map<uintptr_t, uint8_t> memory{{0x5000, 0}};
    unsigned writes{};
    const auto read = [&](uintptr_t at, uint8_t& v) { auto it = memory.find(at); if (it == memory.end()) return false; v = it->second; return true; };
    const auto write = [&](uintptr_t at, uint8_t v) { ++writes; memory[at] = v; return true; };
    Switch s;
    check(s.apply(Mode::native, 0x5000, read, write) == Result::unchanged && writes == 0, "native over native writes nothing");
    check(s.apply(Mode::off, 0x5000, read, write) == Result::applied && memory[0x5000] == 1, "off sets the disable byte");
    check(s.apply(Mode::off, 0x5000, read, write) == Result::unchanged && writes == 1, "off twice writes once");
    check(s.apply(Mode::native, 0x5000, read, write) == Result::applied && memory[0x5000] == 0, "native restores it");
    check(s.apply(Mode::off, 0, read, write) == Result::unsupported_build, "no address, no write");
    memory[0x5000] = 5;
    check(s.apply(Mode::off, 0x5000, read, write) == Result::unexpected_value && memory[0x5000] == 5, "a non-boolean byte is left alone");
    Switch fresh; std::map<uintptr_t, uint8_t> was_off{{0x6000, 1}};
    check(fresh.apply(Mode::native, 0x6000, [&](uintptr_t a, uint8_t& v) { v = was_off[a]; return true; }, [&](uintptr_t a, uint8_t v) { was_off[a] = v; return true; })
          == Result::unexpected_value && was_off[0x6000] == 1, "a cache the game had already disabled is never enabled");
    std::cout << "Far light cache tests passed\n";
}
