#include "LightCapSortControl.h"
#include <cstdlib>
#include <iostream>
using namespace light_cap_sort;
void check(bool ok, const char* label) { if (!ok) { std::cerr << label << '\n'; std::exit(1); } }
int main() {
    check(parse("distance") == Mode::distance && parse("native\r") == Mode::native && !parse("dist"), "control words");
    check(site_rva(REFERENCE_BUILD) == 0x1ef9b59 && site_rva(PATCHED_BUILD) == 0x1ef9e99 && !site_rva(1), "known builds only");
    Bytes code = NATIVE; unsigned writes{};
    const auto read = [&](uintptr_t at, Bytes& b) { if (at != 0x5000) return false; b = code; return true; };
    const auto write = [&](uintptr_t at, uint8_t v) { ++writes; if (at != 0x5000 + IMMEDIATE) return false; code[IMMEDIATE] = v; return true; };
    check(apply(Mode::native, 0x5000, read, write) == Result::unchanged && writes == 0, "native over native writes nothing");
    check(apply(Mode::distance, 0x5000, read, write) == Result::applied && code[IMMEDIATE] == 0 && code[2] == 0xb3, "distance zeroes the MOV BL immediate only");
    check(apply(Mode::distance, 0x5000, read, write) == Result::unchanged && writes == 1, "twice writes once");
    check(apply(Mode::native, 0x5000, read, write) == Result::applied && code == NATIVE, "native restores the original bytes");
    check(apply(Mode::distance, 0, read, write) == Result::unsupported_build, "no site, no write");
    code[0] = 0x90;
    check(apply(Mode::distance, 0x5000, read, write) == Result::unexpected_bytes && code[IMMEDIATE] == 1, "other code is never patched");
    std::cout << "Light cap sort tests passed\n";
}
