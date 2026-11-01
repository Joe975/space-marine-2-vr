#pragma once
#include "VrControllerInput.h"
#include <array>
#include <cmath>
#include <span>
#include <string_view>

// OpenXR counterpart of config/vr_actions.json + config/bindings_index.json. The action
// names match the SteamVR manifest so both runtimes produce the same VrControllerSample.
// tests/test_openxr_bindings.py parses the BINDING lines below and checks the Index table
// against bindings_index.json: keep one binding per line in exactly this form.
namespace openxr_input {
enum class Kind { boolean, floating, vector2 };
enum Action : size_t { move,turn,confirm,interact,sprint,menu,recenter,fire,fire_touch,aim,melee,parry,grenade,utility,utility_press,finisher,COUNT };
struct Definition { const char* name; Kind kind; };
inline constexpr std::array<Definition,COUNT> ACTIONS{{
    {"move",Kind::vector2},{"turn",Kind::vector2},{"confirm",Kind::boolean},{"interact",Kind::boolean},
    {"sprint",Kind::boolean},{"menu",Kind::boolean},{"recenter",Kind::boolean},{"fire",Kind::floating},
    {"fire_touch",Kind::boolean},{"aim",Kind::floating},{"melee",Kind::floating},{"parry",Kind::floating},
    {"grenade",Kind::floating},{"utility",Kind::vector2},{"utility_press",Kind::floating},{"finisher",Kind::boolean}}};
struct Binding { Action action; const char* path; };
// touch_layout: the profile has no trackpads; its samples take the Touch routes (docs/QUEST_STEAM_LINK.md).
struct Profile { const char* path; const char* name; std::span<const Binding> bindings; bool touch_layout{}; };

// Index: SteamVR force_sensor "force" outputs read the same /input/{grip,trackpad}/force
// components that OpenXR exposes as squeeze/force and trackpad/force.
inline constexpr Binding INDEX[]{
    {move,"/user/hand/left/input/thumbstick"}, // BINDING
    {sprint,"/user/hand/left/input/thumbstick/click"}, // BINDING
    {turn,"/user/hand/right/input/thumbstick"}, // BINDING
    {finisher,"/user/hand/right/input/thumbstick/click"}, // BINDING
    {confirm,"/user/hand/right/input/a/click"}, // BINDING
    {interact,"/user/hand/right/input/b/click"}, // BINDING
    {menu,"/user/hand/left/input/b/click"}, // BINDING
    {recenter,"/user/hand/left/input/a/click"}, // BINDING
    {fire,"/user/hand/right/input/trigger/value"}, // BINDING
    {fire_touch,"/user/hand/right/input/trigger/touch"}, // BINDING
    {aim,"/user/hand/left/input/trigger/value"}, // BINDING
    {melee,"/user/hand/right/input/squeeze/force"}, // BINDING
    {parry,"/user/hand/left/input/squeeze/force"}, // BINDING
    {grenade,"/user/hand/right/input/trackpad/force"}, // BINDING
    {utility,"/user/hand/left/input/trackpad"}, // BINDING
    {utility_press,"/user/hand/left/input/trackpad/force"}, // BINDING
};
// Quest/Rift S Touch (about 70% of Steam VR users, mostly via Steam Link/Virtual Desktop
// on SteamVR). Same physical layout as Index for sticks, face buttons and triggers; upper
// face buttons (B/Y) match Index B. No trackpad or force sensor: squeeze uses the analog
// value; grenade and utility stay unbound (the left-hand grenade gesture and holsters cover
// them; stim, scoreboard and the held ability take the Touch routes, touch_layout below).
// Unbound optional actions release themselves.
inline constexpr Binding TOUCH[]{
    {move,"/user/hand/left/input/thumbstick"},
    {sprint,"/user/hand/left/input/thumbstick/click"},
    {turn,"/user/hand/right/input/thumbstick"},
    {finisher,"/user/hand/right/input/thumbstick/click"},
    {confirm,"/user/hand/right/input/a/click"},
    {interact,"/user/hand/right/input/b/click"},
    {menu,"/user/hand/left/input/y/click"},
    {recenter,"/user/hand/left/input/x/click"},
    {fire,"/user/hand/right/input/trigger/value"},
    {fire_touch,"/user/hand/right/input/trigger/touch"},
    {aim,"/user/hand/left/input/trigger/value"},
    {melee,"/user/hand/right/input/squeeze/value"},
    {parry,"/user/hand/left/input/squeeze/value"},
};
inline constexpr std::array<Profile,2> PROFILES{{
    {"/interaction_profiles/valve/index_controller","Index",INDEX,false},
    {"/interaction_profiles/oculus/touch_controller","Touch",TOUCH,true}}};
inline constexpr std::array<const char*,2> HANDS{"/user/hand/left","/user/hand/right"};

struct Value { bool active{}; float x{},y{}; }; // Boolean state is x != 0.
using Values=std::array<Value,COUNT>;
struct Sample { VrControllerSample sample; bool complete{}; };

// Same rules as openvr_combat::poll: both sticks are mandatory. Inactive buttons read
// released. An inactive or out-of-range optional analog releases only itself and marks
// the bindings incomplete; a missing trigger touch falls back to trigger pull.
inline std::optional<Sample> sample(const Values& v,bool touch_layout=false) {
    const auto stick=[&](Action a){return v[a].active && std::isfinite(v[a].x) && std::isfinite(v[a].y);};
    if(!stick(move) || !stick(turn)) return {};
    const auto pressed=[&](Action a){return v[a].active && v[a].x!=0;};
    Sample out{};auto& s=out.sample;
    s.move_x=v[move].x;s.move_y=v[move].y;s.turn_x=v[turn].x;s.turn_y=v[turn].y; // turn_y: viewer flight.
    s.confirm=pressed(confirm);s.interact=pressed(interact);s.sprint=pressed(sprint);s.menu=pressed(menu);s.recenter=pressed(recenter);
    bool complete=true;
    const auto analog=[&](Action a)->float{
        const auto& x=v[a];
        if(!x.active || !std::isfinite(x.x) || x.x<0 || x.x>1.01f) {complete=false;return 0;}
        return x.x;
    };
    s.fire=analog(fire);s.aim=analog(aim);s.melee=analog(melee);s.parry=analog(parry);s.grenade=analog(grenade);s.utility_press=analog(utility_press);
    const auto& pad=v[utility];
    if(pad.active && std::isfinite(pad.x) && std::isfinite(pad.y) && std::abs(pad.x)<=1.01f && std::abs(pad.y)<=1.01f) {s.utility_x=pad.x;s.utility_y=pad.y;}
    else complete=false;
    s.finisher=pressed(finisher);complete=complete && v[finisher].active;
    s.fire_touch_valid=v[fire_touch].active;s.fire_touch=pressed(fire_touch);
    s.touch_layout=touch_layout;
    out.complete=complete;return out;
}
inline const Profile* profile(std::string_view path) {
    for(const auto& p:PROFILES) if(path==p.path) return &p;
    return nullptr;
}
}
