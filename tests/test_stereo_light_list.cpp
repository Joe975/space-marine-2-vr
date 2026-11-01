#include "StereoLightList.h"
#include <cstdlib>
#include <iostream>
using namespace stereo_light_list;
void check(bool ok, const char* label) { if (!ok) { std::cerr << label << '\n'; std::exit(1); } }
// One descriptor copy: position, type and range set; `fill` marks the rest of the bytes.
std::vector<uint8_t> light(float x, float y, float z, int32_t type = 1, float range2 = 100, uint8_t fill = 0) {
    std::vector<uint8_t> e(ENTRY, fill);
    const float p[3]{x, y, z};
    std::memcpy(e.data() + POSITION, p, 12); std::memcpy(e.data() + TYPE, &type, 4); std::memcpy(e.data() + RANGE, &range2, 4);
    return e;
}
std::vector<uint8_t> list(const std::vector<std::vector<uint8_t>>& lights) {
    std::vector<uint8_t> out;
    for (const auto& l : lights) out.insert(out.end(), l.begin(), l.end());
    return out;
}
float x_of(const Result& r, size_t i) { float p[3]; std::memcpy(p, r.entries.data() + i * ENTRY + POSITION, 12); return p[0]; }
int main() {
    check(parse("shared") == Mode::shared && parse("copy\r") == Mode::copy && parse("capsort") == Mode::capsort && parse("native") == Mode::native && !parse("union"), "control words");
    check(cap_by_distance(Mode::shared) && cap_by_distance(Mode::capsort) && !cap_by_distance(Mode::copy) && !cap_by_distance(Mode::native), "cap ranking follows the mode");
    const std::array<float, 3> e0{-1, 0, 0}, e1{1, 0, 0};
    {   // Union: the left eye's lights in its order, then lights only the right eye kept.
        const auto a = list({light(1, 0, 0), light(2, 0, 0), light(3, 0, 0)});
        const auto b = list({light(2, 0, 0), light(9, 0, 0), light(1, 0, 0)});
        const auto r = merge(a.data(), 3, b.data(), 3, e0, e1);
        check(r.count == 4 && r.from_second == 1 && r.capped == 0, "each light once; the right eye adds its own");
        check(x_of(r, 0) == 1 && x_of(r, 1) == 2 && x_of(r, 2) == 3 && x_of(r, 3) == 9, "stable order: left list, then right-only lights");
        const auto again = merge(b.data(), 3, a.data(), 3, e0, e1);
        check(again.count == 4, "the union does not depend on which list holds which light");
    }
    {   // The same light can differ in other bytes between the views' copies: matched once, counted.
        const auto a = list({light(1, 0, 0, 1, 100, 0)});
        const auto b = list({light(1, 0, 0, 1, 100, 7)});
        const auto r = merge(a.data(), 1, b.data(), 1, e0, e1);
        check(r.count == 1 && r.differing_copies == 1 && r.entries[0] == 0, "matched by position/type/range; the left eye's copy is kept");
        const auto c = list({light(1, 0, 0, 2, 100)});  // same place, other type: a different light
        check(merge(a.data(), 1, c.data(), 1, e0, e1).count == 2, "type separates lights at one position");
    }
    {   // Over the cap: the nearest to the point between the eyes, whatever each eye's own ranking.
        std::vector<std::vector<uint8_t>> left, right;
        for (int i = 0; i < 6; ++i) left.push_back(light(float(10 + i), 0, 0));
        for (int i = 0; i < 6; ++i) right.push_back(light(float(-10 - i), 0, 0));
        right.push_back(light(0.5f, 0, 0));  // nearest of all, only in the right eye
        const auto a = list(left), b = list(right);
        const auto r = merge(a.data(), 6, b.data(), 7, e0, e1, 5);
        check(r.count == 5 && r.capped == 8, "at most `cap` lights");
        check(x_of(r, 0) == 0.5f && x_of(r, 1) == 10 && x_of(r, 2) == -10 && x_of(r, 3) == 11 && x_of(r, 4) == -11, "nearest first, ties by key");
        // The cut must not depend on list order: shuffle both inputs and get the same set.
        const auto a2 = list({left[5], left[0], left[3], left[1], left[4], left[2]});
        const auto b2 = list({right[6], right[2], right[0], right[5], right[1], right[4], right[3]});
        const auto r2 = merge(b2.data(), 7, a2.data(), 6, e0, e1, 5);
        check(r2.entries == r.entries, "the over-cap cut ignores the views' list order");
    }
    {   // Empty lists are fine.
        const auto r = merge(nullptr, 0, nullptr, 0, e0, e1);
        check(r.count == 0 && r.entries.empty(), "no lights, no list");
    }
    std::cout << "Stereo light list tests passed\n";
}
