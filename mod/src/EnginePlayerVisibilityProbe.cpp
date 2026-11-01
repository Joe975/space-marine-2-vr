#include "EnginePlayerVisibilityProbe.h"
#include "GameBuild.h"
#include "EngineBodyProbe.h"
#include <MinHook.h>
#include <array>
#include <algorithm>
namespace {
constexpr game_build::Code PROLOGUE=game_build::hashed(0x1e798d0,25,"5df28669c3ce30ad8135cb255f2e51c6832eff5039230cada86a094aef095b5d");
bool read(uintptr_t base,size_t offset,uintptr_t& out) {
    SIZE_T count{};return base && base<=UINTPTR_MAX-offset-sizeof(out) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+offset),&out,sizeof(out),&count) && count==sizeof(out);
}
}
EnginePlayerVisibilityProbe& EnginePlayerVisibilityProbe::get() {static auto* value=new EnginePlayerVisibilityProbe;return *value;}
bool EnginePlayerVisibilityProbe::matches_build(uint32_t stamp,std::span<const uint8_t> bytes) {
    return game_build::code(stamp,FILTER_RVA,bytes,PROLOGUE);
}
bool EnginePlayerVisibilityProbe::install_in_game(HMODULE module) {
    const auto base=reinterpret_cast<uint8_t*>(module);if(!base) return false;
    const auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);const auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto filter=game_build::rva(nt->FileHeader.TimeDateStamp,FILTER_RVA);
    if(!filter || nt->OptionalHeader.SizeOfImage<filter+PROLOGUE.size() || !matches_build(nt->FileHeader.TimeDateStamp,{base+filter,PROLOGUE.size()})) return false;
    return install(base+filter,[](auto now){return EngineBodyProbe::get().active_actor(now);});
}
bool EnginePlayerVisibilityProbe::install(void* target,std::function<uintptr_t(Clock::time_point)> actor) {
    if(m_target) return true;if(!target || !actor) return false;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    m_actor=std::move(actor);
    if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&m_original))!=MH_OK) return false;
    m_target=target;if(MH_EnableHook(target)!=MH_OK) {uninstall();return false;}
    {std::scoped_lock lock{m_mutex};m_state.installed=true;}
    refresh_owner();return true;
}
void EnginePlayerVisibilityProbe::refresh_owner() {
    uintptr_t instance{};const auto actor=m_actor?m_actor(Clock::now()):0;
    if(actor) read(actor,0xd8,instance);m_candidate=instance;
}
bool EnginePlayerVisibilityProbe::owned(void* object) const {
    const auto instance=reinterpret_cast<uintptr_t>(object);
    if(!instance || instance!=m_candidate.load()) return false;
    const auto actor=m_actor(Clock::now());uintptr_t selected{},props{},instance_props{},entity{};
    return actor && read(actor,0xd8,selected) && selected==instance && read(actor,0x20,props) && props &&
        read(instance,0x58,instance_props) && props==instance_props && read(actor,0x38,entity) && entity;
}
uint64_t EnginePlayerVisibilityProbe::hook(void* views,uint32_t frame,float time,void* instance,uint64_t* mask,uint8_t mode) {
    if(const auto filter=s_mask_filter.load(std::memory_order_relaxed);filter && mask)filter(views,instance,mask);
    auto& self=get();bool selected=false;uint64_t before{};SIZE_T count{};
    try {selected=self.owned(instance) && mask && ReadProcessMemory(GetCurrentProcess(),mask,&before,sizeof(before),&count) && count==sizeof(before);} catch(...) {}
    // Preserve all arguments, native side effects and the full return register exactly once.
    const auto result=self.m_original(views,frame,time,instance,mask,mode);
    if(selected) try {
        uint64_t after{};
        if(ReadProcessMemory(GetCurrentProcess(),mask,&after,sizeof(after),&count) && count==sizeof(after) && self.owned(instance)) {
            std::scoped_lock lock{self.m_mutex};auto& state=self.m_state;++state.owned_calls;
            if((result&255)==0) ++state.rejected;
            if(before & ~after) ++state.mask_removed;
            state.last_before=before;state.last_after=after;state.instance=reinterpret_cast<uintptr_t>(instance);state.native_time=time;
        }
    } catch(...) {}
    return result;
}
EnginePlayerVisibilityProbe::Snapshot EnginePlayerVisibilityProbe::snapshot() const {std::scoped_lock lock{m_mutex};return m_state;}
void EnginePlayerVisibilityProbe::uninstall() {
    m_candidate=0;if(m_target) {MH_DisableHook(m_target);MH_RemoveHook(m_target);}m_target=nullptr;
    std::scoped_lock lock{m_mutex};m_state.installed=false;
}
