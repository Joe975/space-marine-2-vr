#pragma once
#include <algorithm>
#include <chrono>
#include "VrControllerInput.h"

// Input ownership follows a fresh committed gun attachment, not a requested holster.
// Any hand/loadout change needs both triggers released, preventing an already held
// aim trigger from becoming a shot on a weapon switch or tracking recovery.
//
// The chainsword rev (right trigger with the pistol/chainsword out) is not a shot. Its
// loadout is debounced: a different confirmed loadout must hold for REV_HOLDOVER before the
// rev re-arms, and the rev keeps running for REV_HOLDOVER after the pistol/chainsword was
// last seen. A one-frame false "primary" or a short loss of the attachment (both seen live
// on 20 October: a held rev stopped and stayed dead until release) no longer stops it.
// A genuine switch to the rifle ends the rev within REV_HOLDOVER; rifle fire still needs
// both triggers released, so a held rev never becomes a shot.
//
// Gun strike (the native fire after a perfect parry or dodge): while a parry is pressed (guard pose or
// left grip) or a dodge, and for STRIKE_WINDOW after, either trigger is the gun's fire (Joe, 30 Oct:
// "make both triggers do gun strike"). Otherwise it is the loadout's trigger only: the right with the
// primary, the left with the pistol, whose right trigger revs the chainsword; in combat the loadout is
// easy to lose track of. The aim and the rev are off for the window.
class VrCombatRouting {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto REV_HOLDOVER=std::chrono::milliseconds(250),STRIKE_WINDOW=std::chrono::milliseconds(1500);
    struct Snapshot {std::optional<bool> secondary,confirmed;bool rearmed{},rev_rearmed{},rev_holdover{},strike{};};
    // Pause, viewer and stalled head/stereo polls reset trigger ownership, so a shot needs a
    // release afterwards. The rev's debounced loadout survives: in the 19:25 live run a 280 ms
    // render stall reset routing mid-hold and the rev stayed dead until release. The stale
    // input itself still stops the native rev during such a stall (100 ms freshness).
    void reset() {m_secondary.reset();m_rearmed=m_holdover=m_unknown_primary=m_strike=false;m_strike_until={};}
    bool unknown_as_primary()const{return m_unknown_primary;}
    // unknown_as_primary: the held weapon has been unknown for its grace (UnknownGrace, VrSession).
    // The triggers then route as for the primary (right fires, left aims) instead of staying
    // dead: a weapon the attachment check never recognises (GENERIC_GUARDS: 25 Oct, "couldn't
    // fire the heavy gun" in the battle simulator) could otherwise never fire. Entering it is a
    // loadout change like any other: both triggers must be released first.
    void update(VrControllerSample& s,std::optional<bool> secondary,bool support,Clock::time_point now,bool unknown_as_primary=false) {
        if(secondary!=m_secondary) {m_secondary=secondary;m_rearmed=false;}
        const bool unknown_primary=unknown_as_primary && !secondary;
        if(unknown_primary!=m_unknown_primary) {m_unknown_primary=unknown_primary;m_rearmed=false;}
        if(secondary) {
            if(!m_confirmed) {m_confirmed=secondary;m_rev_rearmed=false;m_candidate.reset();}
            else if(secondary==m_confirmed) m_candidate.reset();
            else {
                if(m_candidate!=secondary) {m_candidate=secondary;m_candidate_at=now;}
                if(now>=m_candidate_at && now-m_candidate_at>=REV_HOLDOVER) {m_confirmed=secondary;m_candidate.reset();m_rev_rearmed=false;}
            }
            if(*secondary) m_secondary_at=now;else m_primary_at=now;
        }
        const float right=s.fire,left=s.aim;
        if(right<=.05f && left<=.05f) m_rearmed=true;
        if(right<=.05f) m_rev_rearmed=true;
        s.fire=s.aim=s.chainsword_rev=0;
        const bool recent=m_secondary_at!=Clock::time_point{} && now>=m_secondary_at && now-m_secondary_at<=REV_HOLDOVER;
        m_holdover=secondary!=std::optional{true} && m_confirmed==std::optional{true} && recent;
        if(m_confirmed==std::optional{true} && (secondary==std::optional{true} || m_holdover) && m_rev_rearmed) s.chainsword_rev=right;
        if(s.guard || s.parry>.35f || s.confirm) m_strike_until=now+STRIKE_WINDOW;
        m_strike=now<m_strike_until;
        if(!m_rearmed) return;
        if(!secondary && !m_unknown_primary) return;
        if(m_strike) {s.fire=(std::max)(right,left);s.aim=0;s.chainsword_rev=0;return;}
        if(!secondary) {s.fire=right;s.aim=support?1.0f:left;return;}
        if(*secondary) s.fire=left;
        else {s.fire=right;s.aim=support?1.0f:left;}
    }
    Snapshot snapshot() const {return {m_secondary,m_confirmed,m_rearmed,m_rev_rearmed,m_holdover,m_strike};}
    // The pistol/chainsword loadout as the sword sees it, debounced like the rev: observed
    // now, or confirmed and seen within REV_HOLDOVER. The live 21 Oct driver run lost the
    // attachment reading for one or two polls during each native attack, and a raw gate
    // reset the swing detector mid-combo. A real switch to the rifle ends it within 250 ms.
    bool sword_out(std::optional<bool> secondary,Clock::time_point now) const {
        if(secondary==std::optional{true}) return true;
        return m_confirmed==std::optional{true} && m_secondary_at!=Clock::time_point{} && now>=m_secondary_at && now-m_secondary_at<=REV_HOLDOVER;
    }
    // The bolter as a swing sees it, debounced the same way: the reading drops out (unknown) for
    // a poll or two during every native attack with the bolter out (29 Oct battle simulator), and
    // a raw gate reset the swing detector each time, so the next stroke never armed: 19 of Joe's
    // 48 strong strokes sent nothing. A real switch to the pistol ends it within 250 ms.
    bool gun_out(std::optional<bool> secondary,Clock::time_point now) const {
        if(secondary==std::optional{false}) return true;
        return !secondary && m_confirmed==std::optional{false} && m_primary_at!=Clock::time_point{} && now>=m_primary_at && now-m_primary_at<=REV_HOLDOVER;
    }
private:
    std::optional<bool> m_secondary,m_confirmed,m_candidate;Clock::time_point m_candidate_at{},m_secondary_at{},m_primary_at{};
    bool m_rearmed{},m_rev_rearmed{},m_holdover{},m_unknown_primary{},m_strike{};Clock::time_point m_strike_until{};
};
