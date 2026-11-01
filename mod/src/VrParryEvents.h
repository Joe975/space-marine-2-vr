#pragma once
// What the game tells the player about an incoming attack and how a parry went, for the trace
// (docs/PARRY_TIMING_2026-10-26.md, 29 Oct). Parry and attack indication are game script; the
// native seams are the attacker's Morpheme animation events and the Wwise sounds the script posts
// (static RE: build/parry_re/NOTES.md). Wwise event IDs are the FNV-1 32-bit hash of the
// lowercase name.
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vr_parry {
constexpr uint32_t fnv1(std::string_view name) {
    uint32_t h=2166136261u;
    for(char c:name) { if(c>='A' && c<='Z') c=char(c-'A'+'a');h*=16777619u;h^=uint8_t(c); }
    return h;
}
struct Sound { std::string_view name,label; uint32_t id; };
inline constexpr std::array<Sound,12> SOUNDS{{
    {"wpn_melee_chswd_parry","parry_animation",fnv1("wpn_melee_chswd_parry")},           // posted live (29 Oct test room)
    {"ui_parry_attack_indicator","indicator_parry",fnv1("ui_parry_attack_indicator")},   // blue: the attack can be parried
    {"ui_dodge_attack_indicator","indicator_dodge",fnv1("ui_dodge_attack_indicator")},   // red: dodge it
    {"wpn_melee_chswd_3d_parry","parry_animation",fnv1("wpn_melee_chswd_3d_parry")},     // the parry animation plays
    {"wpn_hit_parry_sword_3d","parried",fnv1("wpn_hit_parry_sword_3d")},                 // OnMeleeParried
    {"wpn_hit_parry_sword_perfect_3d","parried_riposte",fnv1("wpn_hit_parry_sword_perfect_3d")},
    {"wpn_hit_parry_warrior_sword","parried",fnv1("wpn_hit_parry_warrior_sword")},
    {"wpn_hit_parry_warrior_sword_perfect","parried_riposte",fnv1("wpn_hit_parry_warrior_sword_perfect")},
    {"wpn_hit_parry_player_duardbreak","guard_break",fnv1("wpn_hit_parry_player_duardbreak")}, // (sic)
    {"ui_hud_hit_player","player_hit",fnv1("ui_hud_hit_player")},                        // SubscribeOnGotDamage
    {"ui_hud_hit_body","player_hit",fnv1("ui_hud_hit_body")},
    {"ui_hud_hit_shield","player_hit",fnv1("ui_hud_hit_shield")},
}};
// A posted name: one of SOUNDS, or by pattern (the shipped names are not all the ones posted:
// the parry animation posts wpn_melee_chswd_parry, not the _3d_ name the banks list).
inline std::optional<Sound> sound(std::string_view name) {
    for(const auto& s:SOUNDS) if(name.size()==s.name.size() && fnv1(name)==s.id) return s;
    const auto has=[&](std::string_view part){return name.find(part)!=std::string_view::npos;};
    if(has("attack_indicator")) return Sound{name,"indicator_other",fnv1(name)};
    if(has("hud_hit")) return Sound{name,"player_hit",fnv1(name)};
    if(has("parry")) return Sound{name,"parry_other",fnv1(name)};
    return std::nullopt;
}
inline std::optional<Sound> sound(uint32_t id) {
    for(const auto& s:SOUNDS) if(id==s.id) return s;
    return std::nullopt;
}
// The attacker's animation events that start an indication (prop_attack_indication_attacker.prop).
inline std::optional<std::string_view> indication(std::string_view event) {
    if(event=="attack_notification") return "parry";
    if(event=="attack_notification_parry_all") return "parry_all";
    if(event=="attack_indication_dodge") return "dodge";
    if(event=="attack_indication_dodge_all") return "dodge_all";
    return std::nullopt;
}
}
