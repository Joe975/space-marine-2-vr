#include "EngineMeleeSweep.h"
#include "GameBuild.h"
#include "NativeTrackerHandleGuard.h"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace {
constexpr game_build::Code UPDATE=game_build::hashed(0x1007a20,16,"b6391417b8ee255feff18d62a67f6d49e514560faa0efb883ecdb8026eb5fb1e");
// The detector whose native update is running on this thread (0 outside one).
thread_local uintptr_t t_detector{};
bool read_handle(uintptr_t impl,uint64_t& handle) {
    __try { handle=*reinterpret_cast<const uint64_t*>(impl+0xd8);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

EngineMeleeSweep& EngineMeleeSweep::get() { static auto* value=new EngineMeleeSweep;return *value; }

bool EngineMeleeSweep::install_in_game(HMODULE module) {
    if(installed()) return true;
    auto* base=reinterpret_cast<uint8_t*>(module);if(!base) return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;
    const auto update=game_build::rva(stamp,UPDATE_RVA);
    if(!update || nt->OptionalHeader.SizeOfImage<update+UPDATE.size()) return false;
    if(!game_build::code(stamp,UPDATE_RVA,{base+update,UPDATE.size()},UPDATE)) return false;
    // The filter first: until the update hook marks a thread it returns at once.
    NativeTrackerHandleGuard::get().set_filter(NativeTrackerHandleGuard::Image::executable,&filter);
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(MH_CreateHook(base+update,reinterpret_cast<void*>(&update_hook),reinterpret_cast<void**>(&m_update))!=MH_OK) return false;
    if(MH_EnableHook(base+update)!=MH_OK) { MH_RemoveHook(base+update);return false; }
    m_update_target=base+update;
    return true;
}

void EngineMeleeSweep::observe(const Sword& s) {
    if(!s.instance || s.sword<0) return;
    m_sword.store(-1);m_instance.store(s.instance);m_sword.store(s.sword);
    m_sweep.record(s.time,s.final,s.native);
}

void EngineMeleeSweep::update_hook(void* detector,void* a,void* b,void* c) {
    auto& self=get();
    const auto outer=t_detector;t_detector=reinterpret_cast<uintptr_t>(detector);
    ++self.m_detector_updates;
    self.m_update(detector,a,b,c);
    t_detector=outer;
}

void EngineMeleeSweep::filter(void* tracker,void* out) {
    if(t_detector && tracker && out) get().substitute(t_detector,reinterpret_cast<uintptr_t>(tracker),out);
}

void EngineMeleeSweep::substitute(uintptr_t detector,uintptr_t impl,void* out) {
    uint64_t handle{};if(!read_handle(impl,handle)) return;
    const auto instance=uintptr_t(handle&0xffffffffffffull);const auto index=int32_t(int16_t(handle>>48));
    const auto sword=m_sword.load();
    if(sword<0 || index!=sword || instance!=m_instance.load()) return;
    ++m_player_reads;
    // The native read is the hand's sword (latest relative * root world); the root follows from it.
    Matrix native{};std::memcpy(native.data(),out,sizeof(native));
    const auto world=m_sweep.world(detector,Clock::now(),native);
    if(!world) return;
    std::memcpy(out,world->data(),sizeof(*world));
    ++m_replaced;
}

EngineMeleeSweep::Snapshot EngineMeleeSweep::snapshot() const {
    const auto sweep=m_sweep.snapshot();
    return {installed(),m_detector_updates.load(),m_player_reads.load(),m_replaced.load(),sweep.refused,sweep};
}
