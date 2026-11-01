#pragma once
#include "VrControllerInput.h"
#include <openvr.h>
#include <array>

namespace openvr_combat {
inline constexpr std::array<const char*,7> ANALOG{"fire","aim","melee","parry","grenade","utility","utility_press"};
// Optional combat bindings may be absent in existing user bindings. Failure releases that
// combat action immediately, without breaking the established stick/menu/recenter controls.
// Trigger touch is optional too: without it, physical swings fall back to trigger pull.
template<class AnalogReader,class DigitalReader>
bool poll(const std::array<vr::VRActionHandle_t,7>& handles,vr::VRActionHandle_t finisher,vr::VRActionHandle_t fire_touch,
          AnalogReader analog,DigitalReader digital,VrControllerSample& sample) {
    std::array<float,8> values{};
    bool complete=true;
    for(size_t i=0;i<handles.size();++i) {
        vr::InputAnalogActionData_t data{};
        const bool valid=handles[i] && analog(handles[i],data)==vr::VRInputError_None && data.bActive &&
            std::isfinite(data.x) && (i==5 ? std::abs(data.x)<=1.01f && std::isfinite(data.y) && std::abs(data.y)<=1.01f : data.x>=0 && data.x<=1.01f);
        if(!valid) { complete=false;continue; }
        values[i]=data.x;
        if(i==5) values[7]=data.y;
    }
    sample.fire=values[0];sample.aim=values[1];sample.melee=values[2];sample.parry=values[3];sample.grenade=values[4];
    sample.utility_x=values[5];sample.utility_y=values[7];sample.utility_press=values[6];
    vr::InputDigitalActionData_t button{};
    const bool valid=finisher && digital(finisher,button)==vr::VRInputError_None && button.bActive;
    sample.finisher=valid && button.bState;
    vr::InputDigitalActionData_t touch{};
    sample.fire_touch_valid=fire_touch && digital(fire_touch,touch)==vr::VRInputError_None && touch.bActive;
    sample.fire_touch=sample.fire_touch_valid && touch.bState;
    return complete && valid;
}
}
