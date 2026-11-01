#pragma once
#include "VrFingerPose.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

// Thumbs-up on the Index (docs/THUMBS_UP_2026-10-26.md).
//
// SteamVR already lifts the skeleton's thumb when it leaves the stick, the face buttons and the
// trackpad (capacitive touch), and the finger driver already poses the Marine's thumb from that
// curl. 26 Oct traces (OpenXR hand joints): the left thumb reads about 0.84 on the stick, 0.98-1.0
// on a button or the trackpad and 0.0-0.3 in the air. Two edits on top of the live curl:
//
// - shape: a thumb in the air over a closed grip is a thumbs-up. The thumb opens fully (the
//   rig's bind pose) and middle, ring and pinky close fully (the native grip). The index stays
//   live, so a finger gun still works. Off the thumbs-up, the curl is untouched.
// - PhotoThumb: photo mode is entered by holding left B, so the frozen left thumb was pressing B.
//   If the left thumb was in the air for at least min_lift and came down at most recall before
//   the viewer request, the left thumb shows that lifted curl until the freeze. So: lift the
//   thumb off the stick, then hold B. The right thumb is free during the hold and stays live.
namespace vr_thumbs_up {
using Curl=vr_finger::Curl;
using Clock=std::chrono::steady_clock;

// play: shape in normal play and photo mode. photo: shape only between the viewer request and
// the freeze. off: the live curl as before 26 Oct. The photo thumb runs in play and photo.
enum class Mode { off, photo, play };
inline std::optional<Mode> parse(std::string_view text) {
    if(text.empty() || text=="play") return Mode::play;
    if(text=="photo") return Mode::photo;
    if(text=="off") return Mode::off;
    return {};
}
inline const char* name(Mode mode) { return mode==Mode::off?"off":mode==Mode::photo?"photo":"play"; }

struct Settings {
    float lifted{0.5f};  // thumb curl below this: off every touch surface
    float closed{0.7f};  // mean of middle, ring and pinky at or above this: a closed grip
    std::chrono::milliseconds min_lift{300};  // shorter lifts are the thumb moving between controls
    std::chrono::milliseconds recall{2500};   // 1 s B hold plus the reach to B
};
inline bool thumb_lifted(const Curl& c,const Settings& s={}) { return c[0]<s.lifted; }
inline bool grip_closed(const Curl& c,const Settings& s={}) { return (c[2]+c[3]+c[4])/3>=s.closed; }
inline Curl shape(Curl c,const Settings& s={}) {
    if(thumb_lifted(c,s) && grip_closed(c,s)) { c[0]=0;c[2]=c[3]=c[4]=1; }
    return c;
}

class PhotoThumb {
public:
    explicit PhotoThumb(Settings settings={}):m_settings{settings} {}
    // left: this frame's live left curl, if valid. entering: the viewer sequence runs and the pose
    // is not frozen yet (vr_viewer::before_freeze). Returns the left thumb curl to show instead
    // of the live one, only while entering.
    std::optional<float> update(const std::optional<Curl>& left,bool entering,Clock::time_point now) {
        if(!left) { m_lift_start.reset(); }  // a lost hand breaks the lift
        else if(thumb_lifted(*left,m_settings)) {
            if(!m_lift_start) { m_lift_start=now;m_lift_min=(*left)[0]; }
            m_lift_min=std::min(m_lift_min,(*left)[0]);
        } else if(m_lift_start) {
            if(now-*m_lift_start>=m_settings.min_lift) { m_lift_end=now;m_lift_value=m_lift_min; }
            m_lift_start.reset();
        }
        if(entering && !m_entering) {
            ++m_requests;m_latched.reset();
            if(m_lift_start && now-*m_lift_start>=m_settings.min_lift) m_latched=m_lift_min;
            else if(m_lift_end && now-*m_lift_end<=m_settings.recall) m_latched=m_lift_value;
            if(m_latched) ++m_latches;
        }
        if(!entering) m_latched.reset();
        m_entering=entering;
        return m_latched;
    }
    uint64_t requests() const { return m_requests; }
    uint64_t latches() const { return m_latches; }
private:
    Settings m_settings;
    std::optional<Clock::time_point> m_lift_start,m_lift_end;
    float m_lift_min{},m_lift_value{};
    std::optional<float> m_latched;
    bool m_entering{};
    uint64_t m_requests{},m_latches{};
};

// The finger targets the arm driver gets: live curls in, edited curls out (left, right).
class Shaper {
public:
    using Fingers=std::array<std::optional<Curl>,2>;
    explicit Shaper(Mode mode=Mode::play,Settings settings={}):m_mode{mode},m_settings{settings},m_photo{settings} {}
    Fingers apply(Fingers fingers,bool entering,Clock::time_point now) {
        if(m_mode==Mode::off) return fingers;
        const auto thumb=m_photo.update(fingers[0],entering,now);
        if(thumb && fingers[0]) (*fingers[0])[0]=*thumb;
        if(m_mode==Mode::play || entering) for(size_t hand=0;hand<2;++hand) if(fingers[hand]) {
            const auto shaped=shape(*fingers[hand],m_settings);
            if(shaped!=*fingers[hand]) ++m_shaped[hand];
            fingers[hand]=shaped;
        }
        return fingers;
    }
    Mode mode() const { return m_mode; }
    uint64_t shaped(size_t hand) const { return hand<2?m_shaped[hand]:0; }
    uint64_t photo_requests() const { return m_photo.requests(); }
    uint64_t photo_latches() const { return m_photo.latches(); }
private:
    Mode m_mode;
    Settings m_settings;
    PhotoThumb m_photo;
    std::array<uint64_t,2> m_shaped{};
};
} // namespace vr_thumbs_up
