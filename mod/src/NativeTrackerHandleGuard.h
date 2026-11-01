#pragma once
#include "GameBuild.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

// Readable-memory test with a lock-free 64 KiB chunk cache, so a hot native hook can
// ask "would this read fault?" without a system call on every call. A chunk is cached
// only when one committed, readable region covers all of it. Stale entries (memory
// released later) fall back to native behaviour, never to a new failure.
class ReadablePages {
public:
    using Query=SIZE_T(WINAPI*)(LPCVOID,PMEMORY_BASIC_INFORMATION,SIZE_T);
    explicit ReadablePages(Query query=&VirtualQuery);
    bool readable(uintptr_t address,size_t size) noexcept;
    uint64_t queries() const noexcept {return m_queries.load(std::memory_order_relaxed);}
    void clear() noexcept;
    static bool readable_protection(const MEMORY_BASIC_INFORMATION&) noexcept;
    static constexpr unsigned CHUNK_SHIFT=16;
    static constexpr size_t SLOTS=size_t{1}<<17;
private:
    static size_t slot(uint64_t chunk) noexcept {return size_t((chunk^(chunk>>17))&(SLOTS-1));}
    std::unique_ptr<std::atomic<uint64_t>[]> m_slots;
    Query m_query;
    std::atomic<uint64_t> m_queries{};
};

// Guards the two native reads that fault when a tracker object's last pointer field
// (+0xd8) has had its high dword overwritten by a stray 4-byte write at +0xdc. That
// corruption appears in the first frames of the second ordinary view and has hit the
// client (Retail.exe) and the in-process server (Retail.dll) trackers alike; see
// docs/validation/tracker-handle-corruption-2026-10-20.md. Both guards change native
// behaviour only when the native read would certainly fault:
// - propGEOM tracker world getter (vtable+0x48): an unreadable packed geometry handle
//   takes the game's own "no geometry" branch (the getter runs on a copy of the
//   tracker whose handle is the native sentinel; nothing native is written);
// - two-handle tracker validity (vtable+0x40): an unreadable weak-handle record makes
//   the tracker report invalid, an answer every native caller already handles.
// Exact build guards per image; a mismatch refuses that image only.
class NativeTrackerHandleGuard {
public:
    enum class Image : uint8_t {executable=0,dll=1};
    enum class Kind : uint8_t {getter=0,pair=1};
    struct Build {
        uint32_t timestamp{};
        uintptr_t getter_rva{},pair_rva{},sentinel_rva{};
        game_build::Code getter_bytes;
        game_build::Code pair_bytes;
    };
    static constexpr Build EXE{0x6ac3e716,0x134c2e0,0x19463c0,0x4d47f00,
        game_build::hashed(0x134c2e0,16,"e53cf2a45234f8bae5153f3c5dc9cf292cd781d3995df310871f01f441c39d0f"),
        game_build::hashed(0x19463c0,32,"07bff3e0da4f5da4df3f06cbb032bbc1fcedb9c7d613e310419be7e6de6916f6")};
    static constexpr Build DLL{0x6ac3eb69,0xd6c170,0x136a0e0,0x3fc7300,
        game_build::hashed(0xd6c170,16,"1ca3206eea77279a252f65c0c2e60696821bbe873b870ee95e74c008cb39a4be",game_build::Image::dll),
        game_build::hashed(0x136a0e0,32,"1049003025ffe7f206b33f576178016f87085c73e8b4171102a55682360f7bc3",game_build::Image::dll)};
    static constexpr size_t OBJECT_SIZE=0xe0,HANDLE_OFFSET=0xd8,OWNER_OFFSET=0xd0;
    static constexpr size_t TRIP_BYTES=0x120,TRIP_SLOTS=4;
    struct Trip {Image image{};Kind kind{};uint32_t thread{};uintptr_t object{},vtable{},value{};bool bytes_valid{};std::array<uint8_t,TRIP_BYTES> bytes{};};
    struct Stats {std::array<bool,2> installed{},refused{};std::array<uint64_t,2> getter_trips{},pair_trips{},fallback_identity{},reapplied{},remapped{};uint64_t queries{};};
    using Getter=void(*)(void*,void*);
    using Pair=uint8_t(*)(void*);
    // Optional post-read filter per image: runs on the calling thread after every native getter
    // read whose handle was intact (not after a guarded trip) and may replace the output matrix.
    // EngineMeleeSweep uses the executable's to show the melee hit check another sword; one
    // detour per function keeps the guard and the filter from patching the same prologue twice.
    using Filter=void(*)(void* tracker,void* output);
    void set_filter(Image image,Filter filter) {m_filters[size_t(image)].store(filter,std::memory_order_release);}

    static NativeTrackerHandleGuard& get();
    static bool matches(const Build&,uint32_t timestamp,std::span<const uint8_t> getter,std::span<const uint8_t> pair);
    // The decision rules, separate from the hooks so fixtures test the exact production logic.
    // True when the native getter's reads of this packed handle cannot fault.
    static bool packed_readable(uint64_t packed,uint64_t sentinel,ReadablePages&) noexcept;
    // True when the native "no geometry" branch's reads through the owner cannot fault.
    static bool owner_readable(uintptr_t object,ReadablePages&) noexcept;
    // True when both weak-handle records of a two-handle tracker are null or readable.
    static bool pair_readable(uintptr_t object,ReadablePages&) noexcept;

    // Fixture-capable install: actual MinHook detours on the supplied targets.
    bool install(Image,void* getter,void* pair,const uint64_t* sentinel,uintptr_t module=0);
    // The server DLL may be unloaded and loaded again. Same mapping: re-patch a target whose
    // code no longer starts with the detour jump. Gone or moved: forget the mapping so the
    // next poll installs on the new one. Returns whether the slot is still installed.
    bool revalidate(Image,HMODULE current);
    // Validates the image's timestamp and both function prologues, then installs.
    bool install_in_module(Image,HMODULE);
    // Present-thread poll: once the late-loaded server DLL exists, installs both images
    // (each decided once). Disabled entirely by SM2VR_TRACKER_GUARD=0.
    void poll();
    // What poll number `tick` (counted on every poll) does: every 16th poll revalidates an installed
    // DLL slot and, while an image is undecided, tries to install. Until 29 Oct the count stopped once
    // both images were decided, so revalidation never ran.
    struct PollSteps {bool revalidate{},install{};};
    static PollSteps poll_steps(uint32_t tick,bool dll_installed,bool both_decided) {
        const bool due=tick%16==0;return {due && dll_installed,due && !both_decided};
    }
    void enable(bool value) {m_enabled.store(value,std::memory_order_release);}
    bool enabled() const {return m_enabled.load(std::memory_order_acquire);}
    void uninstall(); // Quiescent fixture teardown only.
    Stats stats() const;
    // Returns trips captured since the last call (at most TRIP_SLOTS ever kept).
    size_t take_trips(std::array<Trip,TRIP_SLOTS>&);
    static std::string hex(std::span<const uint8_t>);
    ReadablePages& pages() {return m_pages;}

    template<int I> static void getter_hook(void* tracker,void* output);
    template<int I> static uint8_t pair_hook(void* tracker);
private:
    void record(Image,Kind,uintptr_t object,uintptr_t value) noexcept;
    struct Slot {
        std::atomic<Getter> getter{};std::atomic<Pair> pair{};
        std::atomic<const uint64_t*> sentinel{};
        void* getter_target{};void* pair_target{};uintptr_t module{};
        std::atomic<bool> installed{},refused{};
        std::atomic<uint64_t> getter_trips{},pair_trips{},fallback_identity{},reapplied{},remapped{};
    };
    std::array<Slot,2> m_slots{};
    std::array<std::atomic<Filter>,2> m_filters{};
    std::atomic<bool> m_enabled{true};
    ReadablePages m_pages;
    std::array<Trip,TRIP_SLOTS> m_trips{};
    std::array<std::atomic<bool>,TRIP_SLOTS> m_trip_ready{};
    std::atomic<uint32_t> m_trip_count{};
    size_t m_trips_taken{};
    bool m_polled_disabled{},m_disabled{};
    uint32_t m_poll_count{};
};
