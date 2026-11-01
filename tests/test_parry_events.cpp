#include "VrParryEvents.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
}

int main() {
    try {
        using namespace vr_parry;
        // IDs found in the game's shipped sound banks (build/parry_re/wwise_ids.txt).
        require(fnv1("ui_parry_attack_indicator")==0xf0522ee1u && fnv1("ui_dodge_attack_indicator")==0xbaa19cc8u,"indicator IDs");
        require(fnv1("wpn_hit_parry_sword_3d")==0x57ea6069u && fnv1("wpn_hit_parry_sword_perfect_3d")==0x25b57057u,"parry IDs");
        require(fnv1("ui_hud_hit_player")==0x2e7a2859u && fnv1("ui_hud_hit_body")==0xacef33c2u && fnv1("ui_hud_hit_shield")==0xd64575a5u,"hit IDs");
        require(fnv1("wpn_hit_parry_warrior_sword")==0x6cffde32u && fnv1("wpn_hit_parry_warrior_sword_perfect")==0xb56cc754u,"other-player parry IDs");
        require(fnv1("UI_Parry_Attack_Indicator")==fnv1("ui_parry_attack_indicator"),"Wwise hashes the lowercase name");
        const auto by_name=sound("ui_parry_attack_indicator");
        require(by_name && by_name->label=="indicator_parry","a posted name is classified");
        const auto by_id=sound(0x25b57057u);
        require(by_id && by_id->label=="parried_riposte","a posted ID is classified");
        require(!sound("wpn_chainsword_rev") && !sound("char_primaris_foley_melee") && !sound(0x12345678u),"other sounds are not");
        require(sound("wpn_melee_chswd_parry") && sound("wpn_melee_chswd_parry")->label=="parry_animation","the parry animation's live name");
        require(sound("wpn_hit_parry_sword") && sound("wpn_hit_parry_sword")->label=="parry_other","an unlisted parry sound is still caught, by its own name");
        require(sound("ui_hud_hit_armor") && sound("ui_hud_hit_armor")->label=="player_hit","an unlisted HUD hit sound is still a player hit");
        require(indication("attack_notification")==std::string_view{"parry"} && indication("attack_indication_dodge_all")==std::string_view{"dodge_all"},"indication events");
        require(!indication("snd_melee_chswd_parry") && !indication(""),"other animation events are not");
        std::cout<<"parry events tests passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
