#pragma once
#include "GameBuild.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <vector>

// Screen-movie layout boundary. Separate from world-marker camera helpers.
class EngineScreenUi {
public:
    static constexpr uintptr_t LAYOUT_RVA=0xb85260;
    static constexpr game_build::Code PROLOGUE=game_build::hashed(0xb85260,16,"8484824d792787d73d2837023824f1311c13ddabfad4428096e42cb9b703921b");
    struct Sample {
        uintptr_t object{},caller{},renderer{},viewport_function{};
        std::array<int,2> size{},origin{};
        std::array<uint8_t,4> flags{};
        uint64_t calls{};
        std::chrono::steady_clock::time_point received{}; // BoundedTable.h: a full table evicts the least recently seen movie

    };
    struct Snapshot { uint64_t calls{},overflow{}; std::vector<Sample> movies; };
    static EngineScreenUi& get();
    static bool matches_build(uint32_t,std::span<const uint8_t>);
    bool install_in_game(HMODULE);
    bool install(void*,uintptr_t);
    bool installed() const { return m_installed; }
    Snapshot snapshot() const;
private:
    using Layout=void(void*,const int*,const int*);
    static void on_layout(void*,const int*,const int*);
    std::atomic<Layout*> m_original{};
    std::atomic<bool> m_installed{};
    uintptr_t m_module{};
    mutable std::mutex m_mutex;
    std::mutex m_install_mutex;
    Snapshot m_snapshot;
};
