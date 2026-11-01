#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

// Decide once, render twice, for local lights (docs/DECIDE_ONCE_STEREO.md). The light job
// (0x1ef8dd0) builds each view's unshadowed light list from that view's own camera: frustum,
// delayed occlusion and, over 240 candidates, a ranked cut. The eyes then disagree on lights near
// any of those edges, and a light that only one eye's list holds flips in that eye alone. After the
// job, both views get one list: every light either eye kept, in a stable order, at most 240. Over
// the cap the 240 nearest to the point between the eyes stay, so the set changes only as the head
// moves. Each element is a 0x210-byte copy of the light's descriptor; lights are matched across the
// lists by position, type and range (desc +0x30, +0x6c, +0x104), never by the bytes of a view's copy.
namespace stereo_light_list {
constexpr size_t ENTRY = 0x210, CAP = 0xf0, POSITION = 0x30, TYPE = 0x6c, RANGE = 0x104;

enum class Mode { shared, copy, capsort, native };
inline std::optional<Mode> parse(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line == "shared") return Mode::shared;
    if (line == "copy") return Mode::copy;
    if (line == "capsort") return Mode::capsort;
    if (line == "native") return Mode::native;
    return std::nullopt;
}
inline const char* name(Mode m) {
    switch (m) { case Mode::shared: return "shared"; case Mode::copy: return "copy"; case Mode::capsort: return "capsort"; default: return "native"; }
}
// Whether the light job's over-cap ranking should keep the nearest lights (light_cap_sort::Mode::distance).
inline bool cap_by_distance(Mode m) { return m == Mode::shared || m == Mode::capsort; }

using Key = std::array<uint8_t, 20>;  // position (12), type (4), range squared (4), compared bitwise
inline Key key(const uint8_t* entry) {
    Key k{};
    std::memcpy(k.data(), entry + POSITION, 12);
    std::memcpy(k.data() + 12, entry + TYPE, 4);
    std::memcpy(k.data() + 16, entry + RANGE, 4);
    return k;
}
struct Result {
    std::vector<uint8_t> entries;  // count * ENTRY bytes, written to both views
    size_t count{}, from_second{}, capped{}, differing_copies{};
};
// list0/list1: n0/n1 entries of ENTRY bytes each; eye0/eye1: the views' camera positions.
inline Result merge(const uint8_t* list0, size_t n0, const uint8_t* list1, size_t n1,
                    const std::array<float, 3>& eye0, const std::array<float, 3>& eye1, size_t cap = CAP) {
    Result out;
    std::vector<const uint8_t*> order;
    order.reserve(n0 + n1);
    std::vector<Key> keys;
    keys.reserve(n0 + n1);
    // Sorted (key, entry) index of what is already in, for an O(log n) membership test per light.
    std::vector<std::pair<Key, const uint8_t*>> seen;
    seen.reserve(n0 + n1);
    const auto add = [&](const uint8_t* entry) {
        const auto k = key(entry);
        const auto found = std::lower_bound(seen.begin(), seen.end(), k, [](const auto& s, const Key& v) { return s.first < v; });
        if (found != seen.end() && found->first == k) {
            if (std::memcmp(found->second, entry, ENTRY) != 0) ++out.differing_copies;
            return false;
        }
        seen.insert(found, {k, entry});
        keys.push_back(k); order.push_back(entry); return true;
    };
    for (size_t i = 0; i < n0; ++i) add(list0 + i * ENTRY);
    for (size_t i = 0; i < n1; ++i) if (add(list1 + i * ENTRY)) ++out.from_second;
    if (order.size() > cap) {
        // Nearest to the point between the eyes; ties by the light's key, so equal distances never reorder.
        const std::array<float, 3> mid{(eye0[0] + eye1[0]) / 2, (eye0[1] + eye1[1]) / 2, (eye0[2] + eye1[2]) / 2};
        std::vector<size_t> index(order.size());
        for (size_t i = 0; i < index.size(); ++i) index[i] = i;
        std::vector<float> distance(order.size());
        for (size_t i = 0; i < order.size(); ++i) {
            float p[3]; std::memcpy(p, order[i] + POSITION, 12);
            distance[i] = (p[0] - mid[0]) * (p[0] - mid[0]) + (p[1] - mid[1]) * (p[1] - mid[1]) + (p[2] - mid[2]) * (p[2] - mid[2]);
        }
        std::sort(index.begin(), index.end(), [&](size_t a, size_t b) {
            if (distance[a] != distance[b]) return distance[a] < distance[b];
            return keys[a] < keys[b];
        });
        out.capped = order.size() - cap;
        index.resize(cap);
        std::vector<const uint8_t*> kept;
        kept.reserve(cap);
        for (const auto i : index) kept.push_back(order[i]);
        order = std::move(kept);
    }
    out.count = order.size();
    out.entries.resize(out.count * ENTRY);
    for (size_t i = 0; i < out.count; ++i) std::memcpy(out.entries.data() + i * ENTRY, order[i], ENTRY);
    return out;
}
}
