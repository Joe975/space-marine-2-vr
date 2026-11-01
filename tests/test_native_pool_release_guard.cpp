// The level-exit freeze guard (NativePoolReleaseGuard.h): a release whose record is not in the
// in-use list is skipped, everything else reaches the game's release unchanged.
#include "NativePoolReleaseGuard.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool yes,const char* why) { if(!yes) throw std::runtime_error(why); }
template<class T> void put(uintptr_t base,uintptr_t offset,const T& value) { std::memcpy(reinterpret_cast<void*>(base+offset),&value,sizeof(T)); }
using native_pool_release_guard::Decision;
using native_pool_release_guard::decide;

// One pool laid out like the game's: mode, the in-use list and the key hash (one bucket, one node).
struct Pool {
    std::array<uint8_t,0x22e0> bytes{};
    std::array<uintptr_t,3> node{0,0x1234,5}; // next, key, record (+0x10 as int32)
    uintptr_t head{};
    std::array<int32_t,8> used{};
    uintptr_t base() { return reinterpret_cast<uintptr_t>(bytes.data()); }
    Pool(int32_t mode,std::initializer_list<int32_t> in_use) {
        head=reinterpret_cast<uintptr_t>(node.data());
        put(base(),native_pool_release_guard::MODE,mode);
        put(base(),0x22d0,native_pool_membership::Header{1,1,reinterpret_cast<uintptr_t>(&head)});
        std::copy(in_use.begin(),in_use.end(),used.begin());
        put(base(),native_pool_release_guard::USED,reinterpret_cast<uintptr_t>(used.data()));
        put(base(),native_pool_release_guard::USED_COUNT,int32_t(in_use.size()));
    }
};

std::atomic<int> released{};
__declspec(noinline) void fixture_release(void*,uintptr_t) { released.fetch_add(1); }
}

int main() { try {
    Pool in_use(2,{1,5,9});require(decide(in_use.base(),0x1234)==Decision::call,"a record in the in-use list is released by the game");
    Pool missing(2,{1,9});require(decide(missing.base(),0x1234)==Decision::skip,"a record not in the in-use list (a second release) is skipped");
    Pool empty(2,{});require(decide(empty.base(),0x1234)==Decision::skip,"an empty in-use list is never decremented below zero");
    Pool broken(2,{});put(broken.base(),native_pool_release_guard::USED_COUNT,int32_t(-1));
    require(decide(broken.base(),0x1234)==Decision::skip,"an already negative count is never searched (the freeze loop)");
    Pool other_mode(1,{1,9});require(decide(other_mode.base(),0x1234)==Decision::call,"other pool modes go to the game unchanged");
    require(decide(missing.base(),0x5678)==Decision::call,"a key the hash does not know is the game's own no-op");
    require(decide(0,0x1234)==Decision::call && decide(0x10,0x1234)==Decision::call,"unreadable pools go to the game unchanged");
    Pool unreadable_list(2,{1});put(unreadable_list.base(),native_pool_release_guard::USED,uintptr_t(0x10));
    require(decide(unreadable_list.base(),0x1234)==Decision::call,"an unreadable in-use list goes to the game unchanged");

    // The invariant check.
    using native_pool_release_guard::health;using native_pool_release_guard::healthy;
    auto sorted_pool=Pool(2,{1,5,9});put(sorted_pool.base(),native_pool_release_guard::USED_CAPACITY,int32_t(8));
    require(healthy(health(sorted_pool.base())),"a sorted list within its capacity is healthy");
    auto unsorted=Pool(2,{5,1,9});put(unsorted.base(),native_pool_release_guard::USED_CAPACITY,int32_t(8));
    require(health(unsorted.base()).known && !healthy(health(unsorted.base())),"an unsorted list is reported");
    auto duplicate=Pool(2,{1,5,5});put(duplicate.base(),native_pool_release_guard::USED_CAPACITY,int32_t(8));
    require(!healthy(health(duplicate.base())),"a duplicated record is reported");
    put(broken.base(),native_pool_release_guard::USED_CAPACITY,int32_t(0x36));
    require(health(broken.base()).known && health(broken.base()).count==-1 && !healthy(health(broken.base())),"the 25 Oct freeze state (count -1) is reported");
    auto over=Pool(2,{1,5,9});put(over.base(),native_pool_release_guard::USED_CAPACITY,int32_t(2));
    require(!healthy(health(over.base())),"a count above capacity is reported");
    require(!health(0).known,"no pool, nothing known");

    // Bookkeeping: skipped releases are attributed to their caller and pool.
    auto& books=*new NativePoolReleaseGuard;
    require(books.observe(missing.base(),0x1234,0xaaa)==Decision::skip && books.observe(missing.base(),0x1234,0xaaa)==Decision::skip &&
        books.observe(empty.base(),0x1234,0xbbb)==Decision::skip && books.observe(in_use.base(),0x1234,0xccc)==Decision::call,"observe decides like decide");
    const auto books_counters=books.counters();
    require(books_counters.calls==4 && books_counters.skipped==3 && books_counters.callers[0].address==0xaaa && books_counters.callers[0].skipped==2 &&
        books_counters.callers[1].address==0xbbb && books_counters.callers[1].skipped==1 && !books_counters.callers[2].address,"skips counted per caller; calls that go through are not callers");
    require(books_counters.pools[0]==missing.base() && books_counters.pools[1]==empty.base() && books_counters.pools[2]==in_use.base() && !books_counters.pools[3],
        "every mode-2 pool is tracked, in first-seen order, once");
    require(books.observe(other_mode.base(),0x1234,0xddd)==Decision::call && !books.counters().pools[3],"other modes are not tracked");

    // The real detour on a fixture function.
    auto& guard=NativePoolReleaseGuard::get();require(guard.install(reinterpret_cast<void*>(&fixture_release)),"guard hooks the release");
    void(*volatile release)(void*,uintptr_t)=&fixture_release;
    release(reinterpret_cast<void*>(in_use.base()),0x1234);require(released==1,"a valid release still reaches the original");
    release(reinterpret_cast<void*>(missing.base()),0x1234);require(released==1,"a second release does not reach it");
    release(nullptr,0x1234);require(released==2,"unknown input reaches it");
    const auto counters=guard.counters();require(counters.calls==3 && counters.skipped==1 && counters.last_skipped_key==0x1234,"calls and skips are counted");
    require(!guard.verified_at(reinterpret_cast<void*>(&fixture_release)) && !guard.release_verified(false,reinterpret_cast<void*>(&fixture_release)),
        "a hook nobody digest-checked does not vouch for the body");
    guard.uninstall();release(reinterpret_cast<void*>(missing.base()),0x1234);require(released==3 && !guard.installed(),"uninstall restores the original");

    // 25 Oct 17:21: the hook's jump made NativeOrdinaryTextureCreation's digest check of the same body
    // fail, so it never armed and stereo never started. A verified hook vouches for exactly its address.
    int other{};
    require(guard.install(reinterpret_cast<void*>(&fixture_release),true),"guard hooks a digest-checked body");
    require(guard.release_verified(false,reinterpret_cast<void*>(&fixture_release)),"the hooked body still counts as verified");
    require(!guard.release_verified(false,&other) && guard.release_verified(true,&other),"another address needs its own digest");
    guard.uninstall();require(!guard.release_verified(false,reinterpret_cast<void*>(&fixture_release)),"after uninstall only the digest counts");
    std::cout<<"native pool release guard: in-use records released, second releases and empty lists skipped, the rest unchanged PASS\n";return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; } }
