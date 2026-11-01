#pragma once
#include <algorithm>
#include <cmath>
#include <optional>

// Body scale (docs/BODY_SCALE.md). A player-initiated recenter measures the player's eye
// height (the head's standing-space Y; the floor is 0 in OpenVR's standing universe and
// OpenXR's STAGE space). The first-person world scale is then multiplied by
// reference / player, so a shorter player's hands, holster zones and floor land where Joe's
// do on the Marine. Held for this launch only; nothing is saved.
class VrBodyScale {
public:
    static constexpr float MIN_HEIGHT=1.0f,MAX_HEIGHT=2.1f; // Plausible standing eye heights, metres.
    static constexpr float CROUCH=0.15f; // A reading this far below the launch's highest is a crouch.
    static constexpr float MIN_FACTOR=0.75f,MAX_FACTOR=1.35f;
    enum class Reading { accepted, implausible, crouched };
    static const char* name(Reading r) {
        switch(r){case Reading::accepted:return "accepted";case Reading::implausible:return "implausible";case Reading::crouched:return "crouched";}
        return "unknown";
    }
    // One explicit recenter's head height. Accepted readings replace the held height, up or
    // down; a crouched or implausible one keeps it.
    Reading measure(float head_height) {
        m_last=Reading::accepted;
        if(!std::isfinite(head_height) || head_height<MIN_HEIGHT || head_height>MAX_HEIGHT) m_last=Reading::implausible;
        else if(m_highest && head_height<*m_highest-CROUCH) m_last=Reading::crouched;
        else { m_height=head_height;m_highest=std::max(m_highest.value_or(head_height),head_height); }
        return *m_last;
    }
    std::optional<float> height() const { return m_height; }
    std::optional<Reading> last() const { return m_last; }
    // reference / player, clamped; 1 until both are known.
    static float factor(std::optional<float> reference,std::optional<float> player) {
        if(!reference || !player || !std::isfinite(*reference) || !(*reference>0) || !std::isfinite(*player) || !(*player>0)) return 1;
        return std::clamp(*reference / *player,MIN_FACTOR,MAX_FACTOR);
    }
    float factor(std::optional<float> reference) const { return factor(reference,m_height); }
private:
    std::optional<float> m_height,m_highest;
    std::optional<Reading> m_last;
};
