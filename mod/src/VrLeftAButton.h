#pragma once
#include "VrTapHold.h"

// Left A: a tap uses the class ability (native Y), a one-second hold recenters.
// The ability fires on release so a recenter hold never also triggers it; a tap
// is only ~100 ms, so the delay is not noticeable. The trackpad-top route to Y
// remains as a second way to use the ability.
class VrLeftAButton {
public:
    using Clock=VrTapHold::Clock;
    static constexpr auto HOLD=VrTapHold::HOLD,PULSE=VrTapHold::PULSE;
    struct Result {bool recenter{},ability{};};
    Result update(bool pressed,bool armed,Clock::time_point now) {
        const auto value=m_button.update(pressed,armed,now);
        return {value.hold,value.tap};
    }
    void reset(){m_button.reset();}
private:
    VrTapHold m_button;
};
