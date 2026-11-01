#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

// Full-screen native menus that keep the world rendering (the Battle Barge's Datavault, mission
// select, training: 25 Oct playtest) publish the same native flags as walking the barge, so the
// pause-mask gate (0x908) never shows them on the menu panel and the headset only gets the small
// head-locked prompt crop of the back buffer. On a stereo frame the back buffer holds only the
// game's UI over black (the scene is redirected to the eyes), so how much of it the UI covers
// tells the two apart: 6-7% walking the barge, 88% in the Datavault, 98% on the pause menu
// (25 Oct 18:02 screenshots, 64x36 cells). Only stereo frames count: a mono back buffer holds the
// whole flat scene and would always look covered.
namespace ui_coverage {
    // Cell value: the brightest channel the probe saw in its block of the back buffer.
    inline constexpr uint8_t COVERED=3;
    inline float covered(std::span<const uint8_t> cells) {
        if(cells.empty())return 0;
        const auto count=std::ranges::count_if(cells,[](uint8_t v){return v>=COVERED;});
        return float(count)/float(cells.size());
    }
}

// Hysteresis over coverage samples (about ten a second). It turns on after ON_SAMPLES in a row
// at or above ON, off after OFF_SAMPLES in a row at or below OFF, and off when no stereo sample
// has arrived for STALE (stereo stopped: the ordinary path that shows the panel is idle anyway).
class VrScreenMenuDetector {
public:
    using Clock=std::chrono::steady_clock;
    // 26 Oct playtest: the battle simulator's loadout menu measured 0.53-0.59 and stayed off (head-
    // locked HUD crop, right A still a dodge that "scoots the cursor"). Over the 24-26 Oct logs play
    // never measured above 0.27 and menus never below 0.53, so ON sits between them.
    static constexpr float ON=.45f,OFF=.3f;
    static constexpr unsigned ON_SAMPLES=3,OFF_SAMPLES=2;
    static constexpr auto STALE=std::chrono::seconds(1);
    // coverage: nullopt for a sample that doesn't count (a mono frame).
    bool update(std::optional<float> coverage,Clock::time_point now) {
        if(coverage){
            m_last=now;m_coverage=*coverage;
            if(*coverage>=ON){m_off=0;if(++m_on>=ON_SAMPLES)m_active=true;}
            else if(*coverage<=OFF){m_on=0;if(++m_off>=OFF_SAMPLES)m_active=false;}
            else m_on=m_off=0;
        }
        if(m_active && (m_last==Clock::time_point{} || now-m_last>STALE)){m_active=false;m_on=m_off=0;}
        return m_active;
    }
    bool active(Clock::time_point now) const {return m_active && m_last!=Clock::time_point{} && now-m_last<=STALE;}
    // The latest sample reached ON but the count hasn't: a menu may be opening. Too soon for the panel
    // (a flash stays in the world), but the gaze camera holds off: its right stick is the open menu's
    // scroll (31 Oct 18:46: 11 and 9 trace samples at 0.68-0.8 between the menu's first logged coverage and the panel).
    bool opening(Clock::time_point now) const {return !m_active && m_on>0 && m_last!=Clock::time_point{} && now-m_last<=STALE;}
    float coverage() const {return m_coverage;}
private:
    bool m_active{};unsigned m_on{},m_off{};float m_coverage{};Clock::time_point m_last{};
};
