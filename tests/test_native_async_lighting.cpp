#include "NativeAsyncLightingControl.h"
#include <cstdlib>
#include <iostream>
#include <map>
using namespace native_async_lighting;
void check(bool ok, const char* label) { if (!ok) { std::cerr << label << '\n'; std::exit(1); } }
int main() {
    check(parse("native") == Mode::native && parse("graphics\r") == Mode::graphics && parse("no_async") == Mode::no_async && !parse("") && !parse("grap"),
          "parse knows the three modes and nothing else");
    std::map<uintptr_t, uint8_t> memory{{0x1000 + 0x75, 1}, {0x1000 + 0x3ee, 1}};
    unsigned writes{};
    const Read read = [&](uintptr_t at, uint8_t& v) { auto it = memory.find(at); if (it == memory.end()) return false; v = it->second; return true; };
    const Write write = [&](uintptr_t at, uint8_t v) { ++writes; memory[at] = v; return true; };
    Switches s;
    check(s.apply(Mode::native, 0x1000, 0x75, 0x3ee, read, write) == Result::unchanged && writes == 0, "native over native writes nothing");
    check(s.apply(Mode::graphics, 0x1000, 0x75, 0x3ee, read, write) == Result::applied && memory[0x1075] == 0 && memory[0x13ee] == 1 && writes == 1,
          "graphics clears only the tiled byte");
    check(s.apply(Mode::no_async, 0x1000, 0x75, 0x3ee, read, write) == Result::applied && memory[0x1075] == 0 && memory[0x13ee] == 0 && writes == 2,
          "no_async clears both");
    check(s.apply(Mode::native, 0x1000, 0x75, 0x3ee, read, write) == Result::applied && memory[0x1075] == 1 && memory[0x13ee] == 1 && writes == 4,
          "native restores both after a mode wrote them");
    check(s.apply(Mode::graphics, 0x1000, 0, 0x3ee, read, write) == Result::unsupported_build && writes == 4, "an unplaced switch is never written");
    memory[0x1075] = 7;
    check(s.apply(Mode::graphics, 0x1000, 0x75, 0x3ee, read, write) == Result::unexpected_value && memory[0x1075] == 7 && writes == 4,
          "a non-boolean byte is left alone");
    Switches fresh;
    std::map<uintptr_t, uint8_t> off{{0x1075, 0}, {0x13ee, 1}};
    const Read read_off = [&](uintptr_t at, uint8_t& v) { v = off[at]; return true; };
    const Write write_off = [&](uintptr_t at, uint8_t v) { off[at] = v; return true; };
    check(fresh.apply(Mode::native, 0x1000, 0x75, 0x3ee, read_off, write_off) == Result::unexpected_value && off[0x1075] == 0,
          "a switch the game already had at 0 is not native; nothing is forced on");
    Switches stuck;
    memory = {{0x1075, 1}, {0x13ee, 1}};
    check(stuck.apply(Mode::graphics, 0x1000, 0x75, 0x3ee, read, [](uintptr_t, uint8_t) { return true; }) == Result::write_failed,
          "a write that does not stick is reported");
    check(values(Mode::graphics) == std::pair<uint8_t, uint8_t>{0, 1}, "graphics keeps async compute for the rest");
    std::cout << "Native async lighting tests passed\n";
}
