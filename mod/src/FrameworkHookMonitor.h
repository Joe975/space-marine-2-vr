#pragma once
#include <windows.h>

// The framework's hook monitor (refs/vrframework-core Framework::hook_monitor) rehooks D3D12 when no
// present has arrived for 6 s: it destroys the D3D12Hook (its swap chain, queue and device pointers
// read null until the next present), suspends every thread to rewrite the present vtable slot, and
// does it again every ~11 s while the stall lasts. When the game only stalls, the slot still holds
// the framework's present detour and the rehook writes the same pointer back into the same slot: it
// cannot bring presents back, and the null swap chain it leaves stopped owned stereo (27 Oct 09:47,
// docs/FREEZE_REHOOK_2026-10-27.md). The patched framework sources (cmake/FrameworkHookMonitor.cmake)
// record the slot when D3D12Hook::hook() writes it, and skip the rehook while the slot is intact.
// An overlay or anything else that rewrites the slot still gets the framework's rehook, as before.
namespace sm2_framework_monitor {
struct PresentSlot {void** slot{};void* detour{};};
inline PresentSlot& present_slot(){static PresentSlot recorded;return recorded;}
// D3D12Hook::hook(), right after its PointerHook replaced the present slot.
inline void remember(void** slot,void* detour){present_slot()={slot,detour};}
// The hook monitor, before it would rehook: true while D3D12 is hooked and the slot still holds the detour.
inline bool present_hook_intact(bool hooked){
    const auto recorded=present_slot();
    if(!hooked || !recorded.slot || !recorded.detour || IsBadReadPtr(recorded.slot,sizeof(void*)))return false;
    return *recorded.slot==recorded.detour;
}
}
