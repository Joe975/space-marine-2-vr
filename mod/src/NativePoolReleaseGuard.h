#pragma once
// The level-exit freeze (25 Oct, docs/HEADSET_ALWAYS_SHOWS_2026-10-24.md).
//
// The game's pool release (reference build 0x2079230, its 535 bytes hashed in NativePoolReleaseBytes.h)
// takes (pool, key). On a mode-2 pool (+0xc) it maps the key to a record through the hash at +0x22d0,
// binary-searches that record in the sorted in-use list ([+0x22a0], count +0x22a8) and erases it at the
// index found, or at -1 when it found none: the erase helper then still shifts the whole list down one
// slot and decrements the count. Releasing a key twice is that miss, and the hash entry stays, so it
// keeps happening. The mod's second-eye scene pass (NativeRenderProbe::on_dispatch -> observe_scene ->
// the game's scene code) releases seven per-frame textures the native pass already released: about
// 1200 misses in 90 s of hangar play (cdb trace, 25 Oct 16:58), each dropping a valid in-use entry. At a
// level teardown the count reaches -1 and the next search never ends (gsRenderMng at 0x20796c0-0x20796dd).
//
// The guard skips exactly the miss: mode 2, the key maps to a record, and the record is not in the
// in-use list (or the list is empty or already negative). Everything else, including anything it cannot
// read, goes to the game unchanged. The game without the mod never takes that path.
#include "NativePoolMembership.h"
#include "NativeMemory.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace native_pool_release_guard {
constexpr uintptr_t MODE=0xc, USED=0x22a0, USED_COUNT=0x22a8;
constexpr int32_t RECORDS=4096, MAX_USED=65536;
enum class Decision { call, skip };
inline Decision decide(uintptr_t pool,uintptr_t key) {
    int32_t mode{};
    if(!pool || pool>UINTPTR_MAX-0x22e0 || !native_memory::read(pool+MODE,mode) || mode!=2) return Decision::call;
    const auto entry=native_pool_membership::once(pool,key,RECORDS);
    if(!entry.known || !entry.found || entry.record<0) return Decision::call; // The game's own no-op or unreadable.
    uintptr_t used{};int32_t count{};
    if(!native_memory::read(pool+USED,used) || !native_memory::read(pool+USED_COUNT,count)) return Decision::call;
    if(count<=0) return Decision::skip; // Nothing in use: the erase would take the count below zero.
    if(count>MAX_USED || !used) return Decision::call;
    std::vector<int32_t> list(static_cast<size_t>(count));
    if(!native_memory::read_bytes(used,list.data(),list.size()*sizeof(int32_t))) return Decision::call;
    return std::binary_search(list.begin(),list.end(),entry.record)?Decision::call:Decision::skip;
}

// The invariant the release relies on: 0 <= in-use count <= capacity (+0x22ac) and the list strictly
// increasing. A drift shows here long before a freeze (the 25 Oct freeze had count -1, capacity 0x36).
constexpr uintptr_t USED_CAPACITY=0x22ac;
struct Health { bool known{},sorted{};int32_t count{},capacity{}; };
inline Health health(uintptr_t pool) {
    Health value;uintptr_t used{};
    if(!pool || pool>UINTPTR_MAX-0x22e0 || !native_memory::read(pool+USED,used) || !native_memory::read(pool+USED_COUNT,value.count) ||
        !native_memory::read(pool+USED_CAPACITY,value.capacity)) return value;
    value.known=true;
    if(value.count<0 || value.count>value.capacity || value.capacity>MAX_USED) return value;
    if(!value.count) { value.sorted=true;return value; }
    std::vector<int32_t> list(static_cast<size_t>(value.count));
    if(!used || !native_memory::read_bytes(used,list.data(),list.size()*sizeof(int32_t))) { value.known=false;return value; }
    value.sorted=std::adjacent_find(list.begin(),list.end(),[](int32_t a,int32_t b){return a>=b;})==list.end();
    return value;
}
inline bool healthy(const Health& value) { return value.known && value.sorted && value.count>=0 && value.count<=value.capacity; }
}

class NativePoolReleaseGuard {
public:
    using Release=void(*)(void* pool,uintptr_t key);
    static constexpr size_t TRACKED=4;
    // Skipped releases by the game code that made them (return addresses), so a second path that
    // double-releases shows up by name in the log rather than as another freeze.
    struct Caller { uintptr_t address{};uint64_t skipped{}; };
    struct Counters { uint64_t calls{},skipped{};uintptr_t last_skipped_key{};std::array<Caller,TRACKED> callers{};std::array<uintptr_t,TRACKED> pools{}; };
    static NativePoolReleaseGuard& get();
    // Verifies the release body against its digest for this build, then hooks it.
    bool install_in_game(HMODULE game);
    // Hooks `target` (tests hook a fixture function). `verified`: the caller checked the body's digest.
    bool install(void* target,bool verified=false);
    // The hook rewrites the release's first bytes, so a later digest check of that body fails (25 Oct:
    // NativeOrdinaryTextureCreation refused to arm, and stereo never started). Code that verifies the
    // release body accepts it when the digest matches, or when this guard verified that exact address
    // before hooking it.
    bool verified_at(const void* address) const { return m_installed && m_verified && address && address==m_target; }
    bool release_verified(bool digest_matches,const void* address) const { return digest_matches || verified_at(address); }
    void uninstall();
    bool installed() const { return m_installed; }
    Counters counters() const;
    // The decision and bookkeeping behind the hook; `caller` is the release's return address.
    native_pool_release_guard::Decision observe(uintptr_t pool,uintptr_t key,uintptr_t caller);
private:
    static void hook(void* pool,uintptr_t key);
    std::array<std::atomic<uintptr_t>,TRACKED> m_pools{},m_callers{};
    std::array<std::atomic<uint64_t>,TRACKED> m_caller_skips{};
    Release m_original{};
    void* m_target{};
    std::atomic<bool> m_installed{},m_verified{};
    std::atomic<uint64_t> m_calls{},m_skipped{};
    std::atomic<uintptr_t> m_last_skipped_key{};
};
