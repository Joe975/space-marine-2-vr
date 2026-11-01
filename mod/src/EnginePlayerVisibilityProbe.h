#pragma once
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <span>

// Passive observation of the owned geometry's native view-mask filtering. Its only culling write is
// the optional mask filter (NativeHzbControl), which runs for every object.
class EnginePlayerVisibilityProbe {
public:
    using Clock=std::chrono::steady_clock;
    using MaskFilter=void(*)(void* views,void* instance,uint64_t* mask);
    static void set_mask_filter(MaskFilter filter){s_mask_filter.store(filter);}
    static constexpr uintptr_t FILTER_RVA=0x1e798d0;
    struct Snapshot {
        uint64_t owned_calls{},rejected{},mask_removed{},last_before{},last_after{};
        uintptr_t instance{}; float native_time{}; bool installed{};
    };
    static EnginePlayerVisibilityProbe& get();
    static bool matches_build(uint32_t,std::span<const uint8_t>);
    bool install_in_game(HMODULE);
    bool install(void*,std::function<uintptr_t(Clock::time_point)>);
    void refresh_owner(); // Candidate only; native calls revalidate fresh ownership.
    Snapshot snapshot() const;
    void uninstall(); // Quiescent fixture teardown.
private:
    inline static std::atomic<MaskFilter> s_mask_filter{};
    using Filter=uint64_t(*)(void*,uint32_t,float,void*,uint64_t*,uint8_t);
    static uint64_t hook(void*,uint32_t,float,void*,uint64_t*,uint8_t);
    bool owned(void*) const;
    mutable std::mutex m_mutex;
    Snapshot m_state;
    std::atomic<uintptr_t> m_candidate{};
    std::function<uintptr_t(Clock::time_point)> m_actor;
    Filter m_original{}; void* m_target{};
};
