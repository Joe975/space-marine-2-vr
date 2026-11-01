#pragma once
#include "VrMeleeSweep.h"
#include <Windows.h>
#include <atomic>
#include <cstdint>

// The game's melee hit check sees the sword VrMeleeSweep picks; the drawn sword is unchanged
// (docs/MELEE_SWEEP.md).
//
// Reference build 0x6ac3e716 (static RE notes: build/melee_re/NOTES.md, 28 Oct):
// - a hook on MeleeDetector::Update (vtable[1], 0x1007a20; only rcx is read) marks the calling
//   thread as inside a detector update for the duration of the native call;
// - a filter on the named-object tracker's GetTransform (0x134c2e0, already detoured by
//   NativeTrackerHandleGuard, whose executable filter this is): that getter copies node objIndex's
//   matrix from the evaluated cache C0 (animINST+0xc0; handle at tracker+0xd8 = animINST |
//   objIndex<<48). Only on a marked thread, for the player's animINST and GUN_R's node, the copy is
//   replaced by the chosen sword: chosen relative * inverse(latest relative) * the native copy, so
//   the root's world comes from the read itself (reading the root bone's C0 slot was refused in
//   the 29 Oct battle simulator).
// Every other tracker read, on every thread, returns the native copy untouched. Without the guard
// (SM2VR_TRACKER_GUARD=0) the detector sees the hand's sword, as before.
class EngineMeleeSweep {
public:
    using Clock=vr_melee_sweep::Clock;
    using Matrix=vr_melee_sweep::Matrix;
    static constexpr uintptr_t UPDATE_RVA=0x1007a20;
    // One driven publication of the player's pose: the animINST, the evaluated-node indices of GUN_R
    // and the root bone, and GUN_R relative to the root bone as drawn and as animated.
    struct Sword { uintptr_t instance{}; int32_t sword{-1},root{-1}; Matrix final{},native{}; Clock::time_point time{}; };
    struct Snapshot { bool installed{}; uint64_t detector_updates{},player_reads{},replaced{},refused{}; vr_melee_sweep::Sweep::Snapshot sweep; };
    static EngineMeleeSweep& get();
    bool install_in_game(HMODULE);
    bool installed() const { return m_update_target!=nullptr; }
    void observe(const Sword&);
    void swing(Clock::time_point now) { m_sweep.swing(now); }
    void set_mode(vr_melee_sweep::Mode m) { m_sweep.set_mode(m); }
    vr_melee_sweep::Mode mode() const { return m_sweep.mode(); }
    Snapshot snapshot() const;
private:
    using Update=void(*)(void*,void*,void*,void*);
    static void update_hook(void*,void*,void*,void*);
    static void filter(void*,void*);
    void substitute(uintptr_t detector,uintptr_t impl,void* out);
    vr_melee_sweep::Sweep m_sweep;
    std::atomic<uintptr_t> m_instance{};
    std::atomic<int32_t> m_sword{-1};
    std::atomic<uint64_t> m_detector_updates{},m_player_reads{},m_replaced{};
    Update m_update{};
    void* m_update_target{};
};
