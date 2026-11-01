#pragma once
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

// Hang evidence (24 Oct evening: returning to the Battle Barge froze the game, no frame after
// 20:35:15, no crash dump). The present hook stamps every frame; a background thread checks once
// a second and, when no frame has been presented for THRESHOLD after at least one was, writes one
// minidump of every thread's stack (no heap) with CREATE_NEW, then never again in this process.
// Read-only: it never suspends, resumes or kills anything itself.
class HangWatchdog {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto THRESHOLD=std::chrono::seconds(10);
    static HangWatchdog& get();
    // Present hook, every frame.
    void present(Clock::time_point now=Clock::now()) {m_last.store(now.time_since_epoch().count(),std::memory_order_relaxed);}
    // The decision, pure: a frame was seen, none for `threshold`, and no dump written yet.
    static bool due(int64_t last_ticks,Clock::time_point now,std::chrono::milliseconds threshold,bool dumped) {
        return last_ticks && !dumped && now.time_since_epoch().count()-last_ticks>=std::chrono::duration_cast<Clock::duration>(threshold).count();
    }
    // Thread stacks, thread info and the module lists; CREATE_NEW, so an existing file is kept.
    static bool write_dump(const std::filesystem::path&) noexcept;
    // Starts the checking thread once. on_dump runs on that thread with the path and the result.
    bool start(std::filesystem::path path,std::chrono::milliseconds threshold=THRESHOLD,std::chrono::milliseconds period=std::chrono::seconds(1),
        std::function<void(const std::filesystem::path&,bool,std::chrono::milliseconds)> on_dump={});
    bool dumped() const {return m_dumped.load();}
private:
    std::atomic<int64_t> m_last{};std::atomic_bool m_dumped{},m_started{};
};
