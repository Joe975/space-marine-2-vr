#pragma once
#include <cstdint>
#include <optional>
#include <set>

// One classifier for "what is the game showing" (docs/GENERIC_GUARDS_2026-10-26.md). Before 26 Oct
// the native flags were read seven ways: owned readiness's check order, the menu panel's pause mask,
// the screen-menu gate, the gaze stick, the HUD gate and the legacy eye path each had their own test,
// so a new flag combination was "gameplay" to some and not to others (24 Oct: the 0x800 menu rule
// put the whole Battle Barge on the panel and was reverted two hours later). Every consumer now asks
// classify() and reads one of the predicates below. Pure: no game access.
//
// Native flags (EngineCinematicState, published|requested; docs/GAME_AUTOMATION.md, VIEWER_MODE.md):
//   0x10 cinematic   0x8/0x100 with 0x800: the pause mask 0x908   0x100 photo (0x108)
//   0x400 paused     0x800 hub: the walkable Battle Barge, the main menu, a load
//   0x1000 title and startup screens (0x1400, 0x1800 dialog, 0x1c00 quitting)   0x4000 loading screen
// Seen masks and what they are: config/known_run_states.json (scripts/run_report.py marks new ones).
namespace scene_mode {

enum class Mode:uint32_t {unavailable,gameplay,hub,pause_menu,screen_menu,photo,paused,title,loading,cutscene,unknown};
inline const char* name(Mode value){switch(value){
    case Mode::unavailable:return "unavailable";case Mode::gameplay:return "gameplay";case Mode::hub:return "hub";case Mode::pause_menu:return "pause_menu";
    case Mode::screen_menu:return "screen_menu";case Mode::photo:return "photo";case Mode::paused:return "paused";case Mode::title:return "title";
    case Mode::loading:return "loading";case Mode::cutscene:return "cutscene";case Mode::unknown:return "unknown";}return "invalid";}

inline constexpr uint32_t CINEMATIC=0x10,PHOTO=0x100,PAUSED=0x400,HUB=0x800,TITLE=0x1000,LOADING=0x4000,PAUSE_MASK=0x908;
// Every bit a rule below reads. Other bits don't change the decision (the rules see only these, so a
// new bit can't silently flip a consumer), but they are reported (unknown_bits, logged at warn).
inline constexpr uint32_t KNOWN_BITS=CINEMATIC|0x8|PHOTO|PAUSED|HUB|TITLE|LOADING;

struct Inputs {
    bool available{};uint32_t published{},requested{};
    bool scripted{};    // EngineCinematicState::scripted: the cinematic bit, kept across a pause inside a cutscene
    bool screen_menu{}; // VrScreenMenuDetector: the UI covers most of a stereo frame (a menu over the world)
};
struct Scene {
    Mode mode{Mode::unavailable};uint32_t mask{},unknown_bits{};
    // Nothing the rules know explains this mask (Mode::unknown) or it carries a bit no rule reads.
    bool novel()const{return mode==Mode::unknown || unknown_bits;}
    // Owned stereo stands down for the loading screen: behind it is the half-streamed level (25 Oct, "white snow").
    bool loading()const{return mode==Mode::loading;}
    // A cutscene: stereo from the held spot or the cinematic camera, no player needed (24 Oct).
    bool cutscene()const{return mode==Mode::cutscene;}
    // The pause menu is UI only: the back buffer goes on the room-fixed menu panel, stereo stands down.
    bool pause_menu()const{return mode==Mode::pause_menu;}
    // A full-screen menu over a rendered world (Datavault, mission select): panel carried on stereo pairs.
    bool screen_menu()const{return mode==Mode::screen_menu;}
    bool menu()const{return pause_menu() || screen_menu();}
    // Plain play with nothing over it.
    bool gameplay()const{return mode==Mode::gameplay && !unknown_bits;}
    // Where the gaze camera may steer the native right stick: plain play and the walkable hub. The game picks
    // what Use acts on and draws its prompt from the game camera, so in a hub it never turned that camera stayed
    // at its spawn heading and every prompt stayed behind the player (31 Oct 18:44, 18:53, docs/INTERACTION.md).
    // Exactly 0x800: 0xc00 is the hub still paused coming out of a load.
    bool gaze_camera()const{return gameplay() || (mode==Mode::hub && mask==HUB);}
    // The native HUD is captured from stereo frames unless a cutscene or a native pause owns the screen.
    bool hud()const{return mode!=Mode::unavailable && mode!=Mode::cutscene && mode!=Mode::loading && !(mask&PAUSED);}
};

inline Scene classify(const Inputs& in){
    Scene out;out.mask=in.published|in.requested;const auto m=out.mask&KNOWN_BITS;out.unknown_bits=out.mask&~KNOWN_BITS;
    const auto set=[&](Mode mode){out.mode=mode;return out;};
    if(!in.available)return set(Mode::unavailable);
    // Priority, highest first. Each line names the playtest that put it there.
    if(m&LOADING)return set(Mode::loading);                       // 25 Oct 18:57: carries the cutscene bit too; loading wins
    if(in.scripted)return set(Mode::cutscene);                    // 24 Oct 20:00: story cutscenes; 0x918 (paused inside one) stays a cutscene
    if((m&PAUSE_MASK)==PAUSE_MASK)return set(Mode::pause_menu);   // 0x908
    if(in.screen_menu)return set(Mode::screen_menu);              // 25 Oct: Datavault 88%, pause 98%, walking the barge 6-7%
    if((m&PHOTO) && !(m&HUB))return set(Mode::photo);             // 0x108 (VrViewerSequencer::photo)
    if(m&TITLE)return set(Mode::title);                           // 0x1000, 0x1400, 0x1800, 0x1c00
    if(m&HUB)return set(Mode::hub);                               // 0x800, 0xc00: NOT a menu (24 Oct 19:52)
    if(m&PAUSED)return set(Mode::paused);                         // 0x400 alone, transient
    if(m==0)return set(Mode::gameplay);
    return set(Mode::unknown);                                    // 0x8 alone: no rule. Stereo as in play; UI coverage still brings the panel
}

// Present thread: what to log as the scene changes. A line on every mode change, and once per
// session for each mask the first time it is seen (run_report.py compares those with the table).
class Watch {
public:
    struct Event {Mode mode{},previous{};uint32_t mask{};bool changed{},first_mask{};};
    std::optional<Event> update(const Scene& scene){
        Event e{scene.mode,m_mode,scene.mask,!m_started || scene.mode!=m_mode,false};
        if(scene.mode!=Mode::unavailable && m_masks.size()<256)e.first_mask=m_masks.insert(scene.mask).second;
        m_started=true;m_mode=scene.mode;if(e.changed)++m_changes;
        if(!e.changed && !e.first_mask)return std::nullopt;return e;
    }
    uint64_t changes()const{return m_changes;}
    size_t masks()const{return m_masks.size();}
private:
    Mode m_mode{Mode::unavailable};bool m_started{};uint64_t m_changes{};std::set<uint32_t> m_masks;
};

}
