#pragma once
#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_map>

// Diagnostic candidate fix for the second-eye light flicker (docs/RIGHT_EYE_FLICKER_2026-10-28.md).
// The light job keeps a local light only while its delayed GPU occlusion result (a pixel count from
// 0x2093860) clears a threshold (4, or 2 if it was visible last frame). -1 means no result for that
// object and camera. Two hold modes, per eye:
//   missing  fill a -1 with the last real result seen within `frames` light-job calls.
//   drops    also fill a result under 4 (occluded or tiny) with the last visible (>= 4) result seen
//            within `frames`. An occluded light's volume is hidden, so keeping it a little longer
//            costs light-pass work, not correctness (light_visibility keeps every light).
// Anything older than the hold passes through. 0 frames = off.
class LightVisibilityHold {
public:
    enum class Kind {missing,drops};
    struct Setting { uint32_t frames{}; Kind kind{Kind::missing}; };
    // Control file text: "<frames>" (missing) or "<frames> drops"; frames 0..600.
    static std::optional<Setting> parse(std::string_view line){
        while(!line.empty() && (line.back()=='\r' || line.back()==' '))line.remove_suffix(1);
        uint32_t frames{};size_t i=0;
        for(;i<line.size() && line[i]>='0' && line[i]<='9';++i){frames=frames*10+uint32_t(line[i]-'0');if(frames>600)return std::nullopt;}
        if(!i)return std::nullopt;
        const auto rest=line.substr(i);
        if(rest.empty())return Setting{frames,Kind::missing};
        if(rest==" drops")return Setting{frames,Kind::drops};
        return std::nullopt;
    }
    // Raw results by class: missing (negative), 0, 1-3, 4 and more.
    struct Stats { uint64_t queries{}, missing{}, zero{}, low{}, visible{}, held{}, tracked{}; };
    int32_t apply(uintptr_t object, int eye, int32_t result, uint64_t frame, Setting setting) {
        if (eye < 0 || eye > 1 || !object) return result;
        auto& s = m_stats[eye]; ++s.queries;
        (result < 0 ? s.missing : result == 0 ? s.zero : result < 4 ? s.low : s.visible) += 1;
        auto& seen = m_seen[eye];
        const bool drops = setting.kind == Kind::drops;
        // What a later fill may use: any real result in missing mode, only a visible one in drops mode.
        if (result >= (drops ? 4 : 0)) { seen[object] = {frame, result}; return result; }
        if (!setting.frames || (result >= 0 && !drops)) return result;
        const auto it = seen.find(object);
        if (it == seen.end() || frame < it->second.frame || frame - it->second.frame > setting.frames) return result;
        if (it->second.value <= result) return result;
        ++s.held; return it->second.value;
    }
    // Forget objects without a usable result for `older` frames (call now and then; keeps the maps small).
    void prune(uint64_t frame, uint64_t older) {
        for (auto& seen : m_seen)
            for (auto it = seen.begin(); it != seen.end();) it = frame > it->second.frame + older ? seen.erase(it) : std::next(it);
        for (int e = 0; e < 2; ++e) m_stats[e].tracked = m_seen[e].size();
    }
    const Stats& stats(int eye) const { return m_stats[eye]; }
private:
    struct Seen { uint64_t frame{}; int32_t value{}; };
    std::array<std::unordered_map<uintptr_t, Seen>, 2> m_seen;
    std::array<Stats, 2> m_stats{};
};
