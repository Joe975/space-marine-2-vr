#include "EngineBodyProbe.h"
#include "GameBuild.h"
#include "BoundedTable.h"
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr game_build::Code UPDATE=game_build::hashed(0x5623d0,16,"22b53227d2122970158239253c67e0632adc6d157d71b5abb76c1f0a91ffa7d0");
constexpr game_build::Code WORLD=game_build::hashed(0x171d2f0,16,"728f90eac3254e1e458700e5b9e024b0ee145d865631cfadf13d00e026275950");
bool copy(uintptr_t source,void* destination,size_t size) {
    SIZE_T copied{};
    return source && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(source),destination,size,&copied) && copied == size;
}
template<class T> bool read(uintptr_t object,size_t offset,T& output) { return object <= UINTPTR_MAX-offset && copy(object+offset,&output,sizeof(T)); }
bool finite(const std::array<float,3>& p) { return std::all_of(p.begin(),p.end(),[](float v) { return std::isfinite(v) && std::abs(v) < 1000000; }); }
}
EngineBodyProbe& EngineBodyProbe::get() { static auto* instance = new EngineBodyProbe; return *instance; }
bool EngineBodyProbe::matches_build(uint32_t stamp,std::span<const uint8_t> update,std::span<const uint8_t> world) {
    return game_build::code(stamp,BODY_UPDATE_RVA,update,UPDATE) && game_build::code(stamp,TRACKER_WORLD_RVA,world,WORLD);
}
bool EngineBodyProbe::install_in_game(HMODULE game) {
    if (installed()) { return true; }
    const auto base = reinterpret_cast<uint8_t*>(game);
    if (!base) { return false; }
    const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;
    const auto update=game_build::rva(stamp,BODY_UPDATE_RVA),world=game_build::rva(stamp,TRACKER_WORLD_RVA);
    if (!update || !world || nt->OptionalHeader.SizeOfImage < world+WORLD.size() ||
        !matches_build(stamp,{base+update,UPDATE.size()},{base+world,WORLD.size()})) { return false; }
    return install(base+update,base+world,reinterpret_cast<uintptr_t>(base),nt->OptionalHeader.SizeOfImage);
}
bool EngineBodyProbe::install(void* update,void* world,uintptr_t base,size_t size) {
    if (installed()) { return true; }
    if (!update || !world) { return false; }
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { return false; }
    m_base = base; m_size = size;
    if (MH_CreateHook(update,reinterpret_cast<void*>(&update_hook),reinterpret_cast<void**>(&m_original_update)) != MH_OK) { return false; }
    m_update_target = update;
    if (MH_CreateHook(world,reinterpret_cast<void*>(&world_hook),reinterpret_cast<void**>(&m_original_world)) != MH_OK) { uninstall(); return false; }
    m_world_target = world;
    if (MH_EnableHook(update) != MH_OK || MH_EnableHook(world) != MH_OK) { uninstall(); return false; }
    m_installed = true; return true;
}
void EngineBodyProbe::uninstall() {
    m_enabled = false;
    m_world_observer=nullptr;
    for (auto target:{m_update_target,m_world_target}) if (target) { MH_DisableHook(target); MH_RemoveHook(target); }
    m_update_target = m_world_target = nullptr; m_installed = false;
}
EngineBodyProbe::Snapshot EngineBodyProbe::snapshot() const { std::scoped_lock lock{m_mutex}; return m_snapshot; }
void EngineBodyProbe::update_hook(void* body,void* arguments) {
    auto& self = get(); self.m_original_update(body,arguments);
    try { self.observe_body(body,arguments); } catch (...) {}
}
uint8_t EngineBodyProbe::world_hook(void* object,float* world) {
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto& self = get(); const auto result = self.m_original_world(object,world);
    try { self.observe_tracker(caller,object,world,result != 0); } catch (...) {}
    try { if(const auto observer=self.m_world_observer.load()) observer(caller,object,world,result!=0); } catch (...) {}
    return result;
}
void EngineBodyProbe::set_body_observer(std::function<void(const Body&)> observer) {
    std::scoped_lock lock{m_mutex}; m_body_observer = std::move(observer);
}
uintptr_t EngineBodyProbe::active_actor(std::chrono::steady_clock::time_point now) const {
    std::scoped_lock lock{m_mutex}; return active_actor_locked(now);
}
uintptr_t EngineBodyProbe::active_actor_current(TimeSource clock) const {
    return locked_clock_read(m_mutex,clock,[this](auto now){return active_actor_locked(now);});
}
uintptr_t EngineBodyProbe::active_actor_locked(std::chrono::steady_clock::time_point now) const {
    uintptr_t actor{};
    for (const auto& tracker:m_snapshot.trackers) {
        if (tracker.caller_rva != game_build::rva(0x13a4476) || !tracker.valid || !tracker.owner_actor || now < tracker.received ||
            now-tracker.received > std::chrono::milliseconds(100)) { continue; }
        if (actor && actor != tracker.owner_actor) { return 0; }
        actor = tracker.owner_actor;
    }
    return actor;
}
void EngineBodyProbe::observe_body(void* object,const void* arguments) {
    if (!m_enabled) { return; }
    std::unique_lock lock{m_mutex}; ++m_snapshot.updates;
    Body sample{}; sample.body = reinterpret_cast<uintptr_t>(object);
    uintptr_t wrapper{}, physics{}, requested{};
    if (!read(sample.body,0,sample.body_vtable) || !read(sample.body,8,sample.entity) ||
        !read(sample.body,0x130,physics) || !physics || !read(sample.body,0x138,wrapper) ||
        !read(wrapper,0,sample.actor) || !sample.actor ||
        !read(sample.entity,0xc8,sample.position) || !read(sample.entity,0xb8,sample.forward) ||
        !read(reinterpret_cast<uintptr_t>(arguments),0,requested) || !read(requested,0x30,sample.requested_position) ||
        !finite(sample.position) || !finite(sample.forward) || !finite(sample.requested_position)) { ++m_snapshot.rejected; return; }
    sample.received = m_clock(); // steady_clock::now in the game; injectable for tests
    // A full table gives the least recently seen row to the new body (BoundedTable.h).
    switch (bounded_table::upsert(m_snapshot.bodies,sample,[&](const Body& row) { return row.body == sample.body; },64)) {
        case bounded_table::Result::evicted: ++m_snapshot.evicted; break;
        case bounded_table::Result::dropped: ++m_snapshot.overflow; break;
        default: break;
    }
    auto observer = m_body_observer;
    lock.unlock(); // Native callbacks must never run while the passive collection lock is held.
    if (observer) { observer(sample); }
}
void EngineBodyProbe::observe_tracker(uintptr_t caller,void* object,const float* world,bool valid) {
    if (!m_enabled || caller < m_base || caller-m_base >= m_size) { return; }
    const auto rva = caller-m_base;
    // TPS camera root/controller only. General entity transforms are far too frequent.
    static const std::array<uintptr_t,4> r{game_build::rva(0x13a2000),game_build::rva(0x13a7400),game_build::rva(0x16efa00),game_build::rva(0x16efd00)};
    if (!((r[0] && rva >= r[0] && rva < r[1]) || (r[2] && rva >= r[2] && rva < r[3]))) { return; }
    std::scoped_lock lock{m_mutex}; ++m_snapshot.tracker_calls;
    Tracker sample{}; sample.object = reinterpret_cast<uintptr_t>(object); sample.caller_rva = rva; sample.valid = valid;
    uintptr_t method{};
    if (!copy(sample.object,sample.words.data(),sizeof(sample.words)) || !copy(reinterpret_cast<uintptr_t>(world),sample.world.data(),sizeof(sample.world))) { ++m_snapshot.rejected; return; }
    sample.vtable = sample.words[0];
    if (read(sample.vtable,0x48,method) && method >= m_base && method-m_base < m_size) { sample.world_method_rva = method-m_base; }
    if (sample.world_method_rva == game_build::rva(0x134c2e0)) { read(sample.words[0xd0/8],0,sample.owner_actor); }
    sample.received = m_clock();
    // 26 Oct: after enough level loads the 32 rows all held earlier levels' trackers, the new
    // player's tracker was dropped and active_actor() read 0 for good (BoundedTable.h).
    switch (bounded_table::upsert(m_snapshot.trackers,sample,[&](const Tracker& row) { return row.object == sample.object && row.caller_rva == rva; },32)) {
        case bounded_table::Result::evicted: ++m_snapshot.evicted; break;
        case bounded_table::Result::dropped: ++m_snapshot.overflow; break;
        default: break;
    }
}
