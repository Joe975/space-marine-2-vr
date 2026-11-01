#include "LightListProbe.h"
#include <cstdlib>
#include <iostream>
using P = LightListProbe;
void check(bool ok, const char* label) { if (!ok) { std::cerr << label << '\n'; std::exit(1); } }
int main() {
    check(P::classify(true, true, false, true, 1, 100, false) == P::Reason::kept, "in the list is kept");
    check(P::classify(false, false, true, true, 1, 100, true) == P::Reason::occlusion, "flag still 0 after the query: occlusion threshold");
    check(P::classify(false, true, false, true, 50, 100, true) == P::Reason::newly_visible_in_range, "shadow light back inside its range waits a frame");
    check(P::classify(false, true, false, true, 150, 100, false) == P::Reason::beyond_range_not_cached, "beyond range and not cached");
    check(P::classify(false, true, false, true, 150, 100, true) == P::Reason::other && P::classify(false, true, true, false, 1, 100, false) == P::Reason::other,
          "anything else (cap, shadow list) is other");
    P probe;
    std::vector<std::string> lines;
    for (int job = 0; job < 60; ++job) {
        P::Facts lamp; lamp.light = 0x1000; lamp.view = 1; lamp.shadow = true; lamp.range2 = 100; lamp.dist2 = 50;
        // In the list three jobs out of four; the fourth it re-enters after being dropped by the threshold.
        lamp.in_list = job % 4 != 3; lamp.flag_after = lamp.in_list; lamp.own = lamp.in_list ? 9 : 2;
        P::Facts steady = lamp; steady.light = 0x2000; steady.view = 0; steady.in_list = steady.flag_after = true;
        lines = probe.job({lamp, steady}, 60, 6);
        if (job < 59) check(lines.empty(), "reports only every window");
    }
    check(lines.size() == 2, "a summary and one flipping light");
    check(lines[0].find("view1 flips=29") != std::string::npos && lines[0].find("occlusion=29") != std::string::npos && lines[0].find("view0 flips=0") != std::string::npos,
          "flips counted per view and by the drop reason");
    check(lines[1].find("LightFlip: view=1 light=0x1000") != std::string::npos && lines[1].find("last_drop=occlusion") != std::string::npos, "the flipping light is described");
    check(probe.job({}, 60, 6).empty(), "the window restarts");
    std::cout << "Light list probe tests passed\n";
}
