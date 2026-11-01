#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <algorithm>
#include "GameBuild.h"

// Read-only native game-state masks. Sequence start/resume sets requested bit 0x10;
// pause/stop clears it. Never call those setters or retain native sequence objects.
class EngineCinematicState {
public:
    static constexpr uint32_t BUILD_TIMESTAMP=0x6ac3e716;
    static constexpr uintptr_t GETTER_RVA=0x17353b0,FLAGS_RVA=0x4f17744;
    inline static constexpr game_build::Code GETTER=game_build::hashed(0x17353b0,9,"3a0074d1b89e00fbb66a2f0ea48fa42479712849c579e6cd2c4da081c8902736");
    // LOADING: the game's loading screen (25 Oct: 0x4010, 0x4410, 0x5410, 0x4c10 loading into a mission).
    static constexpr uint32_t CINEMATIC=0x10,PAUSED=0x400,LOADING=0x4000;
    struct Snapshot {bool available{},scripted{},paused{};uint32_t published{},requested{};uint64_t samples{},failures{},transitions{};};
    static EngineCinematicState& get() {static auto* instance=new EngineCinematicState;return *instance;}
    static bool matches_build(uint32_t stamp,std::span<const uint8_t> getter) {
        return game_build::code(stamp,GETTER_RVA,getter,GETTER);
    }
    // The same reader binds a guarded native image or fixture-owned flag storage.
    bool bind(uintptr_t flags,uint32_t stamp,std::span<const uint8_t> getter) {
        std::scoped_lock lock{m_mutex};m_flags=0;m_state={};
        if(!flags || flags>UINTPTR_MAX-8 || !matches_build(stamp,getter)) return false;
        m_flags=flags;return true;
    }
    bool bind_in_game(HMODULE module) {
        const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
        std::array<uint8_t,GETTER.size()> getter{};
        if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 ||
            !read(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.SizeOfImage<FLAGS_RVA+8) return bind(0,0,{});
        const auto stamp=nt.FileHeader.TimeDateStamp;const auto flags=game_build::rva(stamp,FLAGS_RVA),getter_rva=game_build::rva(stamp,GETTER_RVA);
        if(!flags || !getter_rva || !read(base+getter_rva,getter)) return bind(0,0,{});
        return bind(base+flags,stamp,getter);
    }
    Snapshot sample() {
        std::scoped_lock lock{m_mutex};std::array<uint32_t,2> flags{};++m_state.samples;
        if(!read(m_flags,flags)) {m_state.available=false;++m_state.failures;return m_state;}
        const auto combined=flags[0]|flags[1];const bool paused=(combined&PAUSED)!=0;
        // Native pause may clear CINEMATIC while its shot still owns the view. Preserve
        // an observed cinematic across pause; normal paused gameplay never starts one.
        const bool scripted=(combined&CINEMATIC)!=0 || (paused && m_state.scripted);
        if(scripted!=m_state.scripted) ++m_state.transitions;
        m_state.available=true;m_state.scripted=scripted;m_state.paused=paused;
        m_state.published=flags[0];m_state.requested=flags[1];return m_state;
    }
    std::optional<bool> authority() {const auto state=sample();return state.available?std::optional{state.scripted}:std::nullopt;}
    Snapshot snapshot() const {std::scoped_lock lock{m_mutex};return m_state;}
private:
    template<class T> static bool read(uintptr_t address,T& value) {
        SIZE_T count{};return address && address<=UINTPTR_MAX-sizeof(T) &&
            ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(T),&count) && count==sizeof(T);
    }
    mutable std::mutex m_mutex;uintptr_t m_flags{};Snapshot m_state;
};
