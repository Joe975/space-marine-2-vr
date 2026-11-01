#include "NativePoolReleaseGuard.h"
#include "NativeCallCensus.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <cstring>
#include <intrin.h>

NativePoolReleaseGuard& NativePoolReleaseGuard::get() { static auto* instance=new NativePoolReleaseGuard; return *instance; }

bool NativePoolReleaseGuard::install_in_game(HMODULE game) {
    if(installed()) return true;
    const auto base=reinterpret_cast<uint8_t*>(game);
    if(!base) return false;
    const auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;
    const auto release=game_build::rva(stamp,native_pool_membership::RELEASE_RVA);
    if(!release || nt->OptionalHeader.SizeOfImage<release+native_pool_membership::RELEASE_BYTES.size()) return false;
    std::array<uint8_t,native_pool_membership::RELEASE_BYTES.size()> bytes{};std::memcpy(bytes.data(),base+release,bytes.size());
    if(!native_pool_membership::matches(stamp,bytes)) return false; // A changed release body: leave it alone.
    return install(base+release,true);
}

bool NativePoolReleaseGuard::install(void* target,bool verified) {
    if(installed()) return true;
    if(!target) return false;
    m_verified=verified;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&m_original))!=MH_OK) return false;
    m_target=target;
    if(MH_EnableHook(target)!=MH_OK) { uninstall(); return false; }
    m_installed=true;return true;
}

void NativePoolReleaseGuard::uninstall() {
    if(m_target) { MH_DisableHook(m_target);MH_RemoveHook(m_target); }
    m_target=nullptr;m_installed=false;m_verified=false;
}

namespace {
// Index of `value` in a small lock-free set, adding it if there is room; TRACKED when full.
template<size_t N> size_t slot(std::array<std::atomic<uintptr_t>,N>& set,uintptr_t value) {
    for(size_t i=0;i<N;++i) {
        auto current=set[i].load(std::memory_order_relaxed);
        if(current==value) return i;
        if(!current && set[i].compare_exchange_strong(current,value)) return i;
        if(current==value) return i;
    }
    return N;
}
}

native_pool_release_guard::Decision NativePoolReleaseGuard::observe(uintptr_t pool,uintptr_t key,uintptr_t caller) {
    m_calls.fetch_add(1,std::memory_order_relaxed);
    // Every mode-2 pool is watched, not only those with skips: a count that only climbs is the
    // mirror-image bug (a release the second pass skips: a leak). The mode is read only while the
    // pool is new and there is room, so a tracked pool costs no extra read.
    if(pool && !m_pools[TRACKED-1].load(std::memory_order_relaxed) &&
        std::none_of(m_pools.begin(),m_pools.end(),[pool](const auto& p){return p.load(std::memory_order_relaxed)==pool;})) {
        int32_t mode{};if(native_memory::read(pool+native_pool_release_guard::MODE,mode) && mode==2) slot(m_pools,pool);
    }
    const auto decision=native_pool_release_guard::decide(pool,key);
    if(decision==native_pool_release_guard::Decision::skip) {
        m_skipped.fetch_add(1,std::memory_order_relaxed);m_last_skipped_key.store(key,std::memory_order_relaxed);
        if(const auto i=slot(m_callers,caller);i<TRACKED) m_caller_skips[i].fetch_add(1,std::memory_order_relaxed);
    }
    return decision;
}

NativePoolReleaseGuard::Counters NativePoolReleaseGuard::counters() const {
    Counters value{m_calls.load(),m_skipped.load(),m_last_skipped_key.load()};
    for(size_t i=0;i<TRACKED;++i) { value.callers[i]={m_callers[i].load(),m_caller_skips[i].load()};value.pools[i]=m_pools[i].load(); }
    return value;
}

void NativePoolReleaseGuard::hook(void* pool,uintptr_t key) {
    NativeCallCensus::get().count(NativeCallCensus::Hook::pool_release);
    auto& self=get();
    if(self.observe(reinterpret_cast<uintptr_t>(pool),key,reinterpret_cast<uintptr_t>(_ReturnAddress()))==native_pool_release_guard::Decision::skip) return;
    self.m_original(pool,key);
}
