#pragma once
#include "GameBuild.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>

// Opt-in read-only first-chance evidence for the observed exact game-DLL / EXE duplicate transform
// fault. Never handles/suppresses an exception, changes context, follows COM
// pointers or establishes an object/resource lifetime. No work on normal frames.
class NativeFaultEvidence {
public:
    static constexpr uintptr_t RVA=0xd6c19c,FUNCTION_RVA=0xd6c170;
    static constexpr uint32_t TIMESTAMP=0x6ac3eb69,MAGIC=0x534d3246;
    enum class Image { dll, executable };
    static constexpr uintptr_t EXE_RVA=0x134c30c,EXE_FUNCTION_RVA=0x134c2e0;
    static constexpr uint32_t EXE_TIMESTAMP=0x6ac3e716;
    inline static constexpr game_build::Code EXE_FUNCTION_BYTES=game_build::hashed(0x134c2e0,16,"e53cf2a45234f8bae5153f3c5dc9cf292cd781d3995df310871f01f441c39d0f");
    inline static constexpr game_build::Code FUNCTION_BYTES=game_build::hashed(0xd6c170,16,"1ca3206eea77279a252f65c0c2e60696821bbe873b870ee95e74c008cb39a4be",game_build::Image::dll);
    inline static constexpr game_build::Code FAULT_BYTES=game_build::hashed(0xd6c19c,7,"5859d817333d612cce42a8d702002cb7adf0c914b9d84c43d3bb63a790fb9954",game_build::Image::dll);
    inline static constexpr game_build::Code EXE_FAULT_BYTES=game_build::hashed(0x134c30c,7,"5859d817333d612cce42a8d702002cb7adf0c914b9d84c43d3bb63a790fb9954");
#pragma pack(push,1)
    struct Region {uint64_t address{},allocation{},base{},region_size{};uint32_t state{},protect{},type{},size{};std::array<uint8_t,512> bytes{};};
    struct Evidence {
        uint32_t magic{MAGIC},version{1},size{},pid{},thread{},timestamp{TIMESTAMP},code{},first_chance{1},disposition{EXCEPTION_CONTINUE_SEARCH};
        uint64_t filetime{},module{},rip{},fault_address{},operation{},rbx{},rcx{},r8{},rsp{};
        // RBX object, stack, R8 table, object vtable, masked object+0xd8 table.
        std::array<Region,5> regions{};
    };
#pragma pack(pop)
    static_assert(sizeof(Region)==560 && sizeof(Evidence)==2908);
    static NativeFaultEvidence& get();
    static bool matches(uint32_t,const std::array<uint8_t,16>&,const std::array<uint8_t,7>&,Image=Image::dll);
    static bool collect(const EXCEPTION_POINTERS*,uintptr_t module,Evidence&,Image=Image::dll) noexcept;
    static bool write_new(const wchar_t*,const Evidence&) noexcept;
    bool arm(HMODULE,const std::filesystem::path&,HMODULE executable=nullptr);
    enum class ArmResult {waiting,armed,refused};
    // Missing late-loaded game DLL is a wait. Once a module appears, attempt
    // its complete guard exactly once; a real refusal never retries.
    ArmResult poll(HMODULE,const std::filesystem::path&,HMODULE executable=nullptr);
    // The same callback registered with Windows; exposed for deterministic tests
    // of filtering, once-only capture and unconditional continue-search behavior.
    static LONG WINAPI observe(EXCEPTION_POINTERS*) noexcept;
private:
    static Region region(uintptr_t) noexcept;
    // RVAs and timestamps of the armed images in the running build (0 = reference constants).
    inline static uintptr_t s_dll_rva{},s_exe_rva{};inline static uint32_t s_dll_stamp{},s_exe_stamp{};
    uintptr_t m_module{},m_executable{};std::wstring m_path;void* m_handler{};bool m_arm_attempted{};std::atomic_flag m_busy=ATOMIC_FLAG_INIT;std::atomic_bool m_attempted{};
    inline static std::atomic<NativeFaultEvidence*> s_owner{};
};
