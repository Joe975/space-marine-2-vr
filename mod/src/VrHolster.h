#pragma once
#include "VrBodyFrame.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

// Body-relative weapon holsters. A fresh right-grip squeeze at the right hip swaps weapons both
// ways, bolter to pistol and pistol to bolter (Joe, 31 Oct: the hip only, no more grab over the
// shoulder). The shoulder zone stays a zone: it still owns the grip and gates swings starting
// there, as before, but a squeeze there draws nothing;
// raising a closed right fist above the eyes requests the native emote once per raise.
// Zones live in the head's yaw-only frame (standing-space metres, before world scale
// or rig snaps), so looking up/down or snap turning cannot move them. VrSession passes
// the hand body-scaled about the head (docs/BODY_SCALE.md), so the metres are Joe's. Output is a
// short native button pulse; the game still decides whether the switch happens.
class VrHolster {
public:
    using Clock=std::chrono::steady_clock;
    enum class Slot { none, primary, secondary, salute };
    struct Parameter { const char* name; float initial,minimum,maximum; };
    // Heights are hand-minus-eye metres; lateral is towards the right, forward is gaze yaw.
    static constexpr std::array<Parameter,9> PARAMETERS{{
        {"shoulder_min_height",-0.4f,-0.6f,0.3f}, {"shoulder_max_forward",0.1f,-0.3f,0.3f},
        {"shoulder_min_lateral",-0.15f,-0.5f,0.3f}, {"hip_max_height",-0.42f,-1,-0.2f},
        {"hip_min_height",-1.1f,-1.6f,-0.4f}, {"hip_min_lateral",0.1f,-0.2f,0.5f},
        {"fist_min_height",0.35f,0.05f,0.6f}, {"fist_max_lateral",0.8f,0.3f,1.2f},
        {"fist_max_abs_forward",0.4f,0.1f,0.8f}}};
    static constexpr float HIP_MAX_FORWARD=0.3f, PRESS=0.65f, RELEASE=0.35f, RAISE_LEAD=0.15f, REARM_DROP=0.3f;
    // Joe, 21 Oct: a squeezed fist only owns the hand (no swing starts) once it is within
    // RAISE_LEAD of the emote height, so a hip swing with the grip held attacks. His real
    // emote raises peaked 0.49-0.72 m above the eyes (19 raises, 19-20 Oct), so the emote now
    // fires at 0.35 m (was 0.15, at a sword raised beside the head).
    static constexpr float SHOULDER_MAX_RADIUS=0.35f,HIP_MAX_RADIUS=0.45f;
    static constexpr auto PULSE=std::chrono::milliseconds(100), COOLDOWN=std::chrono::milliseconds(400), QUIET=std::chrono::milliseconds(600);
    // The right thumb pressing A, B, the stick (click) or the trackpad squeezes the Index grip's force
    // sensor. Joe's 30 Oct playtest (20:24, battle simulator): all six pistol draws were unasked, each
    // 50-100 ms after a dodge (right A) with the right hand hanging in the hip zone; at 20:35:56 the
    // grip went from 0.18 to 0.68 in the 100 ms after A went down. 10 of the run's 186 A presses
    // crossed PRESS and 52 more reached 0.4. He was left holding the pistol and could not get the
    // bolter back ("I don't know how to get my bolter out"). VrSession passes this as
    // !acquisition_allowed, so a squeeze that starts under the thumb is consumed: no draw, no emote,
    // and the grip must be released before a squeeze counts again.
    static bool thumb_squeeze(bool a,bool b,bool stick_click,float trackpad_force) { return a || b || stick_click || trackpad_force>RELEASE; }
    struct Settings {
        std::array<float,9> values=[] { std::array<float,9> v{};for(size_t i=0;i<v.size();++i) v[i]=PARAMETERS[i].initial;return v; }();
        bool operator==(const Settings&) const = default;
        bool valid() const {
            for(size_t i=0;i<values.size();++i) if(!std::isfinite(values[i]) || values[i]<PARAMETERS[i].minimum || values[i]>PARAMETERS[i].maximum) return false;
            return values[4]<values[3] && values[3]<values[0];
        }
        static std::optional<Settings> read(std::istream& input) {
            Settings result;std::array<bool,9> seen{};std::string line;
            while(std::getline(input,line)) {
                const auto first=line.find_first_not_of(" \t\r");
                if(first==std::string::npos || line[first]=='#') continue;
                const auto split=line.find('=');if(split==std::string::npos) return {};
                size_t index=0;while(index<PARAMETERS.size() && line.substr(0,split)!=PARAMETERS[index].name) ++index;
                if(index==PARAMETERS.size() || seen[index]) return {};
                std::istringstream number{line.substr(split+1)};
                if(!(number>>result.values[index]) || !(number>>std::ws).eof()) return {};
                seen[index]=true;
            }
            return input.eof() && std::ranges::all_of(seen,[](bool v){return v;}) && result.valid()?std::optional{result}:std::nullopt;
        }
        bool write(std::ostream& out) const {
            if(!valid()) return false;
            out<<std::setprecision(9);
            for(size_t i=0;i<values.size();++i) out<<PARAMETERS[i].name<<'='<<values[i]<<'\n';
            return bool(out);
        }
        static std::optional<Settings> load(const std::filesystem::path& path) { std::ifstream in{path};return read(in); }
        bool save(const std::filesystem::path& path) const {
            if(!path.is_absolute() || !valid()) return false;
            std::ofstream out{path};if(!write(out)) return false;out.flush();return bool(out);
        }
    };
    // last_* record where the hand was at the most recent fresh squeeze, including misses,
    // so a failed holster grab can be located in the log.
    struct Snapshot { Slot zone{},pressed{},last_slot{}; uint64_t primary{},secondary{},misses{},salutes{}; float height{},lateral{},forward{};
        std::array<float,3> last_press{}; };

    bool set_settings(const Settings& value) { if(!value.valid()) return false;m_settings=value;reset();return true; }
    Settings settings() const { return m_settings; }
    // Trace replays only: a large lead reproduces the 19 Oct rule (a fist squeezed anywhere
    // on the right owns the hand).
    void set_raise_lead(float metres) { m_raise_lead=metres; }
    Snapshot snapshot() const { return m_state; }
    void reset() { m_grip=true;m_salute_ready=false;m_raise_owned=false;m_selection_held=false;m_selected_held=false;m_state.zone=m_state.pressed=Slot::none;m_pulse_until={}; }

    // Classify one right-hand position against the head. Pure, for tests and the overlay.
    Slot classify(const vr_camera::Matrix& head,const vr_camera::Matrix& hand) {
        const auto offset=vr_body::locate(head,hand);
        if(!offset) { m_state.height=m_state.lateral=m_state.forward=0;return Slot::none; }
        m_state.height=offset->height;m_state.forward=offset->forward;m_state.lateral=offset->lateral;
        const auto& v=m_settings.values;
        const float radius=std::hypot(m_state.lateral,m_state.forward);
        if(m_state.height>=v[6] && m_state.lateral>=0 && m_state.lateral<=v[7] && std::abs(m_state.forward)<=v[8]) return Slot::salute;
        if(radius<=SHOULDER_MAX_RADIUS && m_state.height<=0.05f && m_state.height>=v[0] && m_state.forward<=v[1] && m_state.lateral>=v[2]) return Slot::primary;
        if(radius<=HIP_MAX_RADIUS && m_state.height<=v[3] && m_state.height>=v[4] && m_state.lateral>=v[5] && std::abs(m_state.forward)<=HIP_MAX_FORWARD) return Slot::secondary;
        return Slot::none;
    }
    // Grip is the right grip force. Only a press that starts inside a zone selects, so
    // holding the grip while moving the hand through a zone never switches weapons.
    Slot update(const std::optional<vr_camera::Matrix>& head,const std::optional<vr_camera::Matrix>& hand,
        float grip,bool allowed,Clock::time_point now,std::optional<bool> secondary_equipped=std::nullopt,bool acquisition_allowed=true) {
        if(!allowed || !head || !hand || !vr_camera::rigid(*head) || !vr_camera::rigid(*hand) || !std::isfinite(grip)) { reset();return Slot::none; }
        m_state.zone=classify(*head,*hand);
        if(grip<=RELEASE) m_grip=m_selection_held=m_raise_owned=m_selected_held=false;
        // Release after enable/focus loss, and lower at least REARM_DROP below the entry
        // threshold (back to about eye level) before another salute. Re-squeezing overhead never
        // repeats it, nor does pumping a raised fist.
        if(!m_grip && m_state.height<m_settings.values[6]-REARM_DROP) m_salute_ready=true;
        if(grip>=PRESS && m_state.lateral>=0 && m_state.lateral<=m_settings.values[7] && m_state.height>=m_settings.values[6]-m_raise_lead &&
           std::abs(m_state.forward)<=std::min(HIP_MAX_FORWARD,m_settings.values[8])) m_raise_owned=true;
        if(grip>=PRESS && m_salute_ready && m_state.zone==Slot::salute && now>=m_cooldown_until && acquisition_allowed) {
            m_salute_ready=false;m_state.pressed=Slot::salute;++m_state.salutes;
            m_pulse_until=now+PULSE;m_cooldown_until=now+COOLDOWN;m_quiet_until=now+QUIET;m_selection_held=m_selected_held=true;
        }
        if(grip>=PRESS && m_state.zone==Slot::salute && !acquisition_allowed) m_salute_ready=false;
        if(grip>=PRESS && !m_grip) {
            m_grip=true;
            m_state.last_press={m_state.height,m_state.lateral,m_state.forward};m_state.last_slot=m_state.zone;
            if(m_state.zone==Slot::none) ++m_state.misses;
            if(m_state.zone==Slot::secondary) m_selection_held=true;
            // Native cycle (D-pad down) is shared with the manual weapon switch, never with emote.
            // Decided once at squeeze onset; changing weapon state during the pulse cannot truncate
            // it. The hip always cycles; pressed names where it goes (the pistol unless the pistol is
            // known to be out), for the log and the trace. With the loadout unknown it cycles too, and
            // the gun then lands in a hand and is learned.
            const bool swap=m_state.zone==Slot::secondary;
            const Slot target=secondary_equipped && *secondary_equipped?Slot::primary:Slot::secondary;
            // Consume an inhibited squeeze too: letting go of aim/support while
            // keeping right grip held must never turn it into a delayed draw.
            if(swap && now>=m_cooldown_until && acquisition_allowed) {
                m_state.pressed=target;++(target==Slot::secondary?m_state.secondary:m_state.primary);
                m_pulse_until=now+PULSE;m_cooldown_until=now+COOLDOWN;m_quiet_until=now+QUIET;m_selection_held=m_selected_held=true;
            }
        }
        if(now>=m_pulse_until) m_state.pressed=Slot::none;
        return m_state.pressed;
    }
    // The right grip belongs to the holster while the hand is in a zone and until the
    // selecting squeeze is released, so drawing back from a holster never grip-attacks.
    bool owns_grip() const { return m_state.zone!=Slot::none || m_state.pressed!=Slot::none || m_selection_held || m_raise_owned; }
    // Reaching to a holster and bringing the hand back are fast arm motions with the
    // trigger finger free. Block physical swings in the zone, while the selecting squeeze
    // is still held, and briefly after a selection.
    bool suppresses_swing(Clock::time_point now) const { return blocks_swing() || quiets_swing(now); }
    // Joe's 21 Oct campaign trace: his sword rested in the hip zone and was raised by the
    // right shoulder (the primary zone) before most swings. Clearing the swing detector
    // whenever the hand was in a zone meant a swing leaving the zone could never start.
    // Split: a zone (or a squeezed fist rising from the hip) only stops a swing starting
    // there; an actual selection and its draw stop swings and require the hand to settle.
    // The overhead (emote) zone stays a hard gate: lowering the arm from it read as a
    // light attack in a replay of the 20 Oct trace (678.9 s), and Joe's sword guard sits
    // at the shoulder, below it.
    bool blocks_swing() const { return m_state.zone!=Slot::none || m_raise_owned; }
    bool quiets_swing(Clock::time_point now) const { return m_state.pressed!=Slot::none || now<m_quiet_until || m_selected_held || m_state.zone==Slot::salute; }
    // With the bolter out the shoulder zone is a hard gate too: a hand there is reaching for a
    // holster, and the reach on to the hip read as a chop (21 Oct replay, 190.9 s). The hip,
    // where the bolter hangs between fights, stays soft.
    bool quiets_gun_swing(Clock::time_point now) const { return quiets_swing(now) || m_state.zone==Slot::primary; }
private:
    Settings m_settings;
    Snapshot m_state;
    bool m_grip{true}; // Start latched: a squeeze held at enable/recovery must be released first.
    float m_raise_lead{RAISE_LEAD};
    bool m_selection_held{},m_salute_ready{},m_raise_owned{},m_selected_held{}; // m_selected_held: this squeeze sent a selection.
    Clock::time_point m_pulse_until{},m_cooldown_until{},m_quiet_until{};
};
