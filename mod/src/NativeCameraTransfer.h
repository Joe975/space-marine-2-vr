#pragma once
#include "GameBuild.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <span>

class NativeCameraTransfer {
public:
    static constexpr uint32_t TIMESTAMP=0x6ac3e716;
    static constexpr uintptr_t RESET_RVA=0x1d77410,COPY_RVA=0x1d774a0;
    static constexpr size_t BYTES=0x29b18;
    inline static constexpr game_build::Code RESET_BYTES=game_build::hashed(0x1d77410,17,"7acfa24e7db36944b970753434e5a4e5ad4ada7b371c1a498f23d5c95f129c37");
    inline static constexpr game_build::Code COPY_BYTES=game_build::hashed(0x1d774a0,11,"73261ccfeacf1bfdfbba20b58a6e05e7790440f708bd8c387e2184162c0bbb2e");
    struct Status {uint64_t resets{},copies{},verified{},refused{};};
    static NativeCameraTransfer& get();
    static bool matches(uint32_t,std::span<const uint8_t>,std::span<const uint8_t>);
    bool install_in_game(HMODULE);
    bool install(void*,void*);
    Status status() const {return {m_resets.load(),m_copies.load(),m_verified.load(),m_refused.load()};}
private:
    using Reset=void(*)(void*);using Copy=void(*)(void*,void*);
    static void on_reset(void*);static void on_copy(void*,void*);
    std::mutex m_install;std::atomic<Reset> m_reset{};std::atomic<Copy> m_copy{};
    std::atomic<uint64_t> m_resets{},m_copies{},m_verified{},m_refused{};
};
