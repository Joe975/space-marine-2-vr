#pragma once
#include "GameBuild.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>

// First-chance evidence for the 23 Oct level-load crash (PLAYTEST_2026-10-23_1948): the game's
// reference-count increment (lock xadd [rcx]) faulting on a game-wide table entry whose memory
// now holds a string, inside the scene render. Read-only and once per process, like
// NativeFaultEvidence: never handles or suppresses the exception, never changes the context,
// calls nothing in the game; the handler costs a compare on every other exception.
//
// The faulting helper is shared by much of the game, so the chain is recorded, not required:
// [rsp] is the holder's return address and [rsp+0x30] the scene caller's, and only then are the
// caller's table (RDI, its [this+0x648]), object ([rsp+0x28], the holder's saved RBX), index
// ([object+0x188]) and entry (table+(index+1)*24) read.
class NativeSceneFaultEvidence {
public:
    // Reference build (2026-10-05) RVAs; the running build's come from game_build::rva.
    static constexpr uintptr_t HELPER=0x26e29d0,HOLDER=0x2650de0,HOLDER_RETURN=0x2650dfa,CALLER=0x1fab630,CALLER_RETURN=0x1fab68e;
    static constexpr uintptr_t FAULT_OFFSET=5,OBJECT_SLOT=0x28,CALLER_SLOT=0x30,INDEX_OFFSET=0x188,ENTRY_STRIDE=24;
    static constexpr uint32_t MAGIC=0x534d3253,VERSION=1; // "S2MS"
    // mov eax,1; lock xadd [rcx],eax; inc eax; ret
    inline static constexpr game_build::Code HELPER_BYTES=game_build::hashed(0x26e29d0,12,"cfc263c4ad605a1088a89ead82f9d69dd7f8d9322306c6e60f34f15eaeabd1b0");
    // The holder: [rcx]=rdx, then the reference at [rdx+0x70].
    inline static constexpr game_build::Code HOLDER_BYTES=game_build::hashed(0x2650de0,35,"0733bbf541f9e8d1f0d45da1b9519853d43d449f1c888b514833c7608ac9959c");
    // The scene caller up to its call of the holder: rdi=[rcx+0x648], index [rdx+0x188], entry rdi+(index+1)*24.
    inline static constexpr game_build::Code CALLER_BYTES=game_build::hashed(0x1fab630,94,"258c89df3996b1b67647ca1b193e2ae862c0598a71258f754464426eade34c39");
    // The scene call on this thread (NativeRenderProbe::observe_scene), set around the pass-through.
    struct SceneCall {uintptr_t renderer{};uint64_t dispatch{};uint32_t flags{},slot{};uint32_t active{};};
    struct Scope {
        SceneCall previous{s_scene};
        Scope(uintptr_t renderer,uint32_t flags,uint32_t slot,uint64_t dispatch){s_scene={renderer,dispatch,flags,slot,1};}
        ~Scope(){s_scene=previous;}
    };
    enum Chain:uint32_t {holder_return=1,caller_return=2,index_read=4};
#pragma pack(push,1)
    struct Region {uint64_t address{},allocation{},base{},region_size{};uint32_t state{},protect{},type{},size{};std::array<uint8_t,512> bytes{};};
    struct Evidence {
        uint32_t magic{MAGIC},version{VERSION},size{},pid{},thread{},timestamp{},code{},first_chance{1},disposition{EXCEPTION_CONTINUE_SEARCH},chain{};
        int32_t index{};uint32_t scene_flags{},scene_slot{},scene_active{};
        uint64_t filetime{},module{},rip{},parameters[2]{},scene_renderer{},scene_dispatch{},object{},table{},entry{},entry_value{};
        // rax rcx rdx rbx rsp rbp rsi rdi r8-r15, as in CONTEXT.
        std::array<uint64_t,16> registers{};
        // stack, object, table, entry (from 0x30 below it), the entry's pointer target, scene renderer.
        std::array<Region,6> regions{};
    };
#pragma pack(pop)
    static_assert(sizeof(Region)==560 && sizeof(Evidence)==3632);
    static NativeSceneFaultEvidence& get();
    static bool matches(uint32_t stamp,std::span<const uint8_t> helper,std::span<const uint8_t> holder,std::span<const uint8_t> caller);
    // rvas: the running build's fault, holder return and caller return RVAs.
    struct Rvas {uintptr_t fault{},holder_return{},caller_return{};};
    static bool collect(const EXCEPTION_POINTERS*,uintptr_t module,const Rvas&,uint32_t stamp,Evidence&) noexcept;
    static bool write_new(const wchar_t*,const Evidence&) noexcept;
    // Verifies the running executable's three functions, then registers the handler. Once.
    bool arm(HMODULE executable,const std::filesystem::path&);
    bool armed()const{return m_handler!=nullptr;}
    uint32_t timestamp()const{return m_stamp;}
    static LONG WINAPI observe(EXCEPTION_POINTERS*) noexcept;
private:
    static Region region(uintptr_t) noexcept;
    inline static thread_local SceneCall s_scene{};
    uintptr_t m_module{};Rvas m_rvas{};uint32_t m_stamp{};std::wstring m_path;void* m_handler{};bool m_arm_attempted{};
    std::atomic_flag m_busy=ATOMIC_FLAG_INIT;std::atomic_bool m_attempted{};
    inline static std::atomic<NativeSceneFaultEvidence*> s_owner{};
};
