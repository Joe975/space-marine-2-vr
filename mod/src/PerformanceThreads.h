#pragma once
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

// Profiling only (docs/PERFORMANCE.md, campaign recipe). Once a second the trace writes how much
// CPU the game's key threads used in that second: the simulation thread (the PCL simulation-end
// marker's thread; simulation starts in jobs on rotating workers), the render thread (the native dispatch's thread) and the Present thread, plus
// the whole process and the whole machine. Windows thread times are tick-accounted, so a one-second
// value is good to about two percent; never subtract per frame. Nothing here suspends or reads
// another thread's state: it is GetThreadTimes on handles opened once.
namespace performance_threads {

enum class Role : uint8_t { simulation, render, present, count };
inline constexpr std::array<const char*, size_t(Role::count)> ROLE_EVENTS{
    "thread_cpu_simulation", "thread_cpu_render", "thread_cpu_present"};

// Thread CPU (kernel+user) in 100 ns units; nullopt when the thread is gone or unreadable.
using ThreadReader = std::function<std::optional<uint64_t>(DWORD)>;
// Process CPU (kernel+user) in 100 ns units.
using ProcessReader = std::function<std::optional<uint64_t>()>;
// Machine idle and total (kernel+user) time in 100 ns units, summed over logical processors.
struct SystemTimes { uint64_t idle{}, total{}; };
using SystemReader = std::function<std::optional<SystemTimes>()>;
using Emit = std::function<void(const char* event, double start, double duration, double value)>;

inline uint64_t ticks(FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

class Monitor {
public:
    Monitor(ThreadReader thread, ProcessReader process, SystemReader system)
        : m_thread(std::move(thread)), m_process(std::move(process)), m_system(std::move(system)) {}
    // Any thread; a relaxed store. The latest thread seen in a role is the one sampled.
    void observe(Role role, DWORD tid) { m_roles[size_t(role)].store(tid, std::memory_order_relaxed); }
    DWORD tid(Role role) const { return m_roles[size_t(role)].load(std::memory_order_relaxed); }
    // Called by one thread at a time (the trace writer holds its lock). The first call, and the
    // first call after a role's thread changes, only sets a baseline: no row without an interval.
    void sample(double now, const Emit& emit) {
        const bool interval = m_last > 0 && now > m_last;
        const double wall = interval ? now - m_last : 0;
        for (size_t i = 0; i < m_roles.size(); ++i) {
            const auto tid = m_roles[i].load(std::memory_order_relaxed);
            auto& slot = m_slots[i];
            std::optional<uint64_t> cpu;
            if (tid) try { cpu = m_thread(tid); } catch (...) {}
            if (tid && cpu && interval && slot.tid == tid && slot.cpu && *cpu >= *slot.cpu)
                emit(ROLE_EVENTS[i], m_last, wall, double(*cpu - *slot.cpu) / 10000.0);
            slot.tid = tid; slot.cpu = cpu;
        }
        std::optional<uint64_t> process;
        try { process = m_process(); } catch (...) {}
        if (process && interval && m_process_cpu && *process >= *m_process_cpu)
            emit("process_cpu", m_last, wall, double(*process - *m_process_cpu) / 10000.0);
        m_process_cpu = process;
        std::optional<SystemTimes> system;
        try { system = m_system(); } catch (...) {}
        if (system && interval && m_system_times && system->total > m_system_times->total && system->idle >= m_system_times->idle) {
            const auto total = system->total - m_system_times->total, idle = system->idle - m_system_times->idle;
            if (idle <= total) emit("system_cpu_busy", m_last, wall, double(total - idle) / double(total));
        }
        m_system_times = system;
        m_last = now;
    }
    // Production readers. read_thread is only called under the trace writer's lock; it keeps a
    // handle per recently sampled thread (one per role) instead of reopening every second.
    static std::optional<uint64_t> read_thread(DWORD tid) {
        struct Cached { DWORD tid{}; HANDLE handle{}; };
        static std::array<Cached, 4> cache{};
        static size_t next{};
        auto found = std::find_if(cache.begin(), cache.end(), [tid](const Cached& c) { return c.tid == tid; });
        if (found == cache.end()) {
            const auto handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (!handle) return std::nullopt;
            found = cache.begin() + next; next = (next + 1) % cache.size();
            if (found->handle) CloseHandle(found->handle);
            *found = {tid, handle};
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(found->handle, &created, &exited, &kernel, &user) || ticks(exited)) {
            CloseHandle(found->handle); *found = {}; return std::nullopt;
        }
        return ticks(kernel) + ticks(user);
    }
    static std::optional<uint64_t> read_process() {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return std::nullopt;
        return ticks(kernel) + ticks(user);
    }
    static std::optional<SystemTimes> read_system() {
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) return std::nullopt;
        return SystemTimes{ticks(idle), ticks(kernel) + ticks(user)}; // Kernel time includes idle.
    }
private:
    struct Slot { DWORD tid{}; std::optional<uint64_t> cpu; };
    ThreadReader m_thread; ProcessReader m_process; SystemReader m_system;
    std::array<std::atomic<DWORD>, size_t(Role::count)> m_roles{};
    std::array<Slot, size_t(Role::count)> m_slots{};
    std::optional<uint64_t> m_process_cpu; std::optional<SystemTimes> m_system_times;
    double m_last{};
};

// QueryThreadCycleTime counts the thread's cycles in time-stamp-counter units on current Windows;
// convert with the TSC rate measured against QPC over the trace's lifetime. 0 until calibrated.
inline double cycles_per_ms(uint64_t tsc_delta, double qpc_delta_ms) {
    return qpc_delta_ms >= 200 && tsc_delta ? double(tsc_delta) / qpc_delta_ms : 0;
}

} // namespace performance_threads
