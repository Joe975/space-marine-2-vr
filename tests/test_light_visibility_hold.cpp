#include "LightVisibilityHold.h"
#include <cstdlib>
#include <iostream>
void check(bool ok, const char* label) { if (!ok) { std::cerr << label << '\n'; std::exit(1); } }
using H = LightVisibilityHold;
int main() {
    check(H::parse("30")->frames == 30 && H::parse("30")->kind == H::Kind::missing, "a number holds missing results");
    check(H::parse("12 drops\r")->frames == 12 && H::parse("12 drops")->kind == H::Kind::drops, "'drops' also holds low results");
    check(H::parse("0")->frames == 0 && !H::parse("") && !H::parse("x") && !H::parse("601") && !H::parse("5 all"), "bounded words only");
    const H::Setting missing{30, H::Kind::missing}, drops{30, H::Kind::drops}, off{0, H::Kind::drops};
    {
        H h;
        const uintptr_t lamp = 0x1000, other = 0x2000;
        check(h.apply(lamp, 1, 900, 10, missing) == 900, "a real result passes through");
        check(h.apply(lamp, 1, -1, 14, missing) == 900, "a missing result 4 frames later is filled with the last real one");
        check(h.apply(lamp, 1, -1, 40, missing) == 900, "filled up to the hold length");
        check(h.apply(lamp, 1, -1, 41, missing) == -1, "left missing once older than the hold");
        check(h.apply(lamp, 1, 0, 42, missing) == 0, "missing mode never overrides a real 0");
        check(h.apply(lamp, 1, -1, 43, missing) == 0, "and fills a later missing result with it");
        check(h.apply(lamp, 1, -1, 44, off) == -1, "0 frames turns the hold off");
        check(h.apply(lamp, 0, -1, 44, missing) == -1, "eyes are tracked separately: eye 0 never had a result");
        check(h.apply(other, 1, -1, 12, missing) == -1, "an object never seen keeps its missing result");
        check(h.apply(lamp, 2, -1, 51, missing) == -1 && h.apply(0, 1, -1, 51, missing) == -1, "bad eye or null object passes through");
        const auto& s = h.stats(1);
        check(s.held == 3 && s.visible == 1 && s.zero == 1 && s.missing == 6, "results are counted by class and fills are counted");
        h.prune(200, 100);
        check(h.stats(1).tracked == 0 && h.apply(lamp, 1, -1, 201, {1000, H::Kind::missing}) == -1, "prune forgets objects without a recent result");
    }
    {
        H h;
        const uintptr_t spot = 0x3000;
        check(h.apply(spot, 1, 2, 5, drops) == 2, "a low result before any visible one passes through");
        check(h.apply(spot, 1, 5000, 10, drops) == 5000, "visible passes through");
        check(h.apply(spot, 1, 0, 11, drops) == 5000 && h.apply(spot, 1, 0, 14, drops) == 5000, "drops mode holds a periodic 0 dropout");
        check(h.apply(spot, 1, 3, 15, drops) == 5000 && h.apply(spot, 1, -1, 16, drops) == 5000, "and a low or missing result");
        check(h.apply(spot, 1, 4800, 17, drops) == 4800, "a new visible result replaces the held one");
        check(h.apply(spot, 1, 0, 48, drops) == 0, "a light occluded longer than the hold drops");
        check(h.apply(spot, 0, 0, 12, drops) == 0, "the other eye is separate");
    }
    std::cout << "Light visibility hold tests passed\n";
}
