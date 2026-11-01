// Deterministic tests for the per-build address table (GameBuild.h, GameBuildTable.inc):
// build identification, reference -> ported RVA translation (a fixed old/new table checked
// against the 2026-10-21 patch), SHA-256, digest comparison on the reference and ported builds
// failing closed, and the module matchers that consult the table (on synthetic code).
#include "synthetic_code.h"
#include "GameBuild.h"
#include "Sha256.h"
#include "EngineCinematicState.h"
#include "NativeArmPoseInvalidation.h"
#include "NativeHzbCulling.h"
#include "NativePipExperiment.h"
#include "NativePoolMembership.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace gb = game_build;
void require(bool value, const char* message) { if (!value) throw std::runtime_error{message}; }

constexpr uint32_t NEW_EXE = 0x6ad10fcb;  // CLIENT 15.2.PATCH.1454884/1455141, installed 2026-10-21
constexpr uint32_t NEW_DLL = 0x6ad110a6;

std::vector<uint8_t> ported(uintptr_t reference, gb::Image image = gb::Image::exe)
{
    auto out = gb::testing::ported(image == gb::Image::dll ? NEW_DLL : NEW_EXE, reference, image);
    require(!out.empty(), "reference RVA is in the ported table");
    return out;
}

std::string hex(const sha256::Digest& d)
{
    std::string s;
    for (auto b : d) { s += "0123456789abcdef"[b >> 4]; s += "0123456789abcdef"[b & 15]; }
    return s;
}
std::span<const uint8_t> text(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

void digests()
{
    // FIPS 180-4 examples.
    require(hex(sha256::hash({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of the empty message");
    require(hex(sha256::hash(text("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    require(hex(sha256::hash(text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 of the 448-bit message");
    const std::vector<uint8_t> million(1000000, 'a');
    require(hex(sha256::hash(million)) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 of a million a");
    constexpr auto parsed = gb::hashed(0x1234, 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    require(parsed.digest == sha256::hash(text("abc")) && parsed.size() == 3 && parsed.reference == 0x1234, "compile-time digest parse");
}

void digest_comparison()
{
    // Real mode (what the game runs): digests decide, on the reference and on a ported build.
    gb::testing::synthetic(false);
    const std::array<uint8_t, 6> actual{0x8b, 0x05, 0x11, 0x22, 0x33, 0x44};
    const gb::Code want{EngineCinematicState::GETTER_RVA, 6, sha256::hash(actual)};
    require(gb::code(gb::REFERENCE_EXE, EngineCinematicState::GETTER_RVA, actual, want), "matching digest accepted on the reference build");
    auto changed = actual;
    changed[5] ^= 1;
    require(!gb::code(gb::REFERENCE_EXE, EngineCinematicState::GETTER_RVA, changed, want), "a changed byte is refused");
    require(!gb::code(gb::REFERENCE_EXE, EngineCinematicState::GETTER_RVA, std::span{actual}.first(5), want), "too few bytes refused");
    require(!gb::code(gb::REFERENCE_EXE, EngineCinematicState::GETTER_RVA + 1, actual, want), "only at the code's own RVA");
    require(!gb::code(gb::REFERENCE_EXE, EngineCinematicState::GETTER_RVA, actual, want, gb::Image::dll), "only in the code's own image");
    // On the ported build the bytes read, with the table's delta undone, must hash to the digest.
    // The getter's rip displacement moved, so the reference bytes read there are refused.
    const auto* e = gb::entry(*gb::ported(NEW_EXE), EngineCinematicState::GETTER_RVA);
    require(e && e->verified >= 6 && !e->delta.empty() && e->delta[0].offset < 6, "the getter's verified prefix has a moved displacement");
    auto at_new_build = actual;
    for (const auto& d : e->delta)
        if (d.offset < at_new_build.size()) at_new_build[d.offset] ^= d.mask;
    require(gb::code(NEW_EXE, EngineCinematicState::GETTER_RVA, at_new_build, want), "the verified port is accepted on the ported build");
    require(!gb::code(NEW_EXE, EngineCinematicState::GETTER_RVA, actual, want), "the reference bytes are refused on the ported build");
    const gb::Code longer{EngineCinematicState::GETTER_RVA, uint16_t(e->verified + 1), sha256::hash(actual)};
    const std::vector<uint8_t> window(e->verified + 1, 0xcc);
    require(!gb::code(NEW_EXE, EngineCinematicState::GETTER_RVA, window, longer), "a window past the verified prefix is refused");
    require(!gb::code(NEW_EXE + 1, EngineCinematicState::GETTER_RVA, at_new_build, want), "unknown build refused");
    const gb::Code outside{0x1d76b51, 6, sha256::hash(actual)};
    require(!gb::code(NEW_EXE, 0x1d76b51, at_new_build, outside), "address outside the table refused");
    gb::testing::synthetic(true);
}

void identification()
{
    require(gb::known(gb::REFERENCE_EXE) && gb::known(NEW_EXE), "reference and ported exe builds are known");
    require(gb::known(gb::REFERENCE_DLL, gb::Image::dll) && gb::known(NEW_DLL, gb::Image::dll), "reference and ported dll builds are known");
    require(!gb::known(NEW_EXE + 1) && !gb::known(NEW_DLL) && !gb::known(NEW_EXE, gb::Image::dll), "other timestamps and images are unknown");
    require(std::string_view{gb::name(NEW_EXE)} == "2026-10-21", "build name");
    for (const auto& b : gb::PORTED_BUILDS) {
        for (size_t i = 1; i < b.entries.size(); ++i) require(b.entries[i - 1].reference < b.entries[i].reference, "entries sorted and unique");
        require(b.size_of_image > 0x4000000, "image size recorded");
    }
}

void translation()
{
    // Reference build: identity.
    require(gb::rva(gb::REFERENCE_EXE, 0x1d76b50) == 0x1d76b50, "reference exe is the identity");
    // Old -> new addresses, one per kind of evidence (see docs/GAME_PATCH_2026-10-21.md).
    struct Row { uintptr_t old_rva, new_rva; gb::Image image; const char* what; };
    constexpr Row rows[]{
        {0x1d76b50, 0x1d76e90, gb::Image::exe, "camera list add (moved function)"},
        {0x18033bf, 0x180389f, gb::Image::exe, "main view return address (moved function)"},
        {0x1805cab, 0x180618b, gb::Image::exe, "volume frame call site inside the changed producer function"},
        {0x134c2e0, 0x134c0b0, gb::Image::exe, "tracker world getter (leaf without unwind data)"},
        {0x4f7c6c8, 0x4f7c688, gb::Image::exe, "camera list global (referenced data)"},
        {0x4f17744, 0x4f17738, gb::Image::exe, "cinematic flags global"},
        {0x4faff8c, 0x4faff4c, gb::Image::exe, "field inside a referenced data object"},
        {0xd6c170, 0xd6c3d0, gb::Image::dll, "server tracker getter"},
        {0x136a0e0, 0x136a620, gb::Image::dll, "server two-handle validity"},
        {0x3fc7300, 0x3fc7300, gb::Image::dll, "server sentinel global"},
    };
    for (const auto& r : rows) {
        const auto stamp = r.image == gb::Image::dll ? NEW_DLL : NEW_EXE;
        if (gb::rva(stamp, r.old_rva, r.image) != r.new_rva) { std::cerr << r.what << '\n'; require(false, "ported RVA"); }
    }
    require(gb::rva(NEW_EXE, 0x1d76b51) == 0, "an address outside the table is never guessed");
    require(gb::rva(NEW_EXE + 1, 0x1d76b50) == 0, "an unknown build translates nothing");
    require(gb::rva(NEW_EXE, 0xd6c170, gb::Image::dll) == 0, "the exe timestamp does not translate dll addresses");
}

void code_comparison()
{
    // Synthetic mode, as the module suites use it: the reference build accepts the synthetic code,
    // the ported build its port (delta applied); the reference bytes, whose rip displacement
    // differs, a changed byte and an unknown build fail.
    const auto& want = EngineCinematicState::GETTER;
    const auto getter = SYNTH(EngineCinematicState::GETTER);
    require(gb::code(gb::REFERENCE_EXE, EngineCinematicState::GETTER_RVA, getter, want), "synthetic code on the reference build");
    const auto bytes = ported(EngineCinematicState::GETTER_RVA);
    require(bytes.size() >= getter.size(), "getter verified far enough");
    require(gb::code(NEW_EXE, EngineCinematicState::GETTER_RVA, bytes, want), "ported getter bytes accepted");
    require(!gb::code(NEW_EXE, EngineCinematicState::GETTER_RVA, getter, want), "reference bytes refused on the ported build");
    std::vector<uint8_t> changed(bytes.begin(), bytes.end());
    changed[1] ^= 1;
    require(!gb::code(NEW_EXE, EngineCinematicState::GETTER_RVA, changed, want), "a changed byte is refused");
    require(!gb::code(NEW_EXE + 1, EngineCinematicState::GETTER_RVA, bytes, want), "unknown build refused");
}

void module_matchers()
{
    require(EngineCinematicState::matches_build(NEW_EXE, ported(EngineCinematicState::GETTER_RVA)), "cinematic getter on the ported build");
    using P = NativePipExperiment;
    require(P::matches(NEW_EXE, ported(P::GET_RVA), ported(P::REGISTER_RVA), ported(P::RELEASE_RVA)), "PIP experiment functions on the ported build");
    require(!P::matches(NEW_EXE, SYNTH(P::GET_BYTES), SYNTH(P::REGISTER_BYTES), SYNTH(P::RELEASE_BYTES)), "PIP get's call/rip bytes differ on the ported build");
    require(P::matches(P::BUILD_TIMESTAMP, SYNTH(P::GET_BYTES), SYNTH(P::REGISTER_BYTES), SYNTH(P::RELEASE_BYTES)), "reference build unchanged");
    const auto release = ported(native_pool_membership::RELEASE_RVA);
    require(release.size() >= native_pool_membership::RELEASE_BYTES.size(), "the whole 535-byte release function is verified");
    std::array<uint8_t, native_pool_membership::RELEASE_BYTES.size()> copy{};
    std::memcpy(copy.data(), release.data(), copy.size());
    require(native_pool_membership::matches(NEW_EXE, copy), "pool release on the ported build");
    require(vr_arm_cache::matches(NEW_EXE, 0x556f000, ported(vr_arm_cache::RVA)), "arm pose invalidation on the ported build");
    // HZB switch: refused on an unknown build without reading; accepted on a supported one.
    bool read_called = false;
    const auto unknown = native_hzb_culling::apply(NEW_EXE + 1, false, [&](uint8_t& v) { read_called = true; v = 1; return true; }, [](uint8_t) { return true; });
    require(unknown == native_hzb_culling::Result::unsupported_build && !read_called, "HZB refuses an unknown build before reading");
}

void active_build()
{
    require(gb::active() == gb::REFERENCE_EXE, "a test process acts as the reference build");
    require(gb::rva(0x1d76b50) == 0x1d76b50 && gb::supported(gb::REFERENCE_EXE) && !gb::supported(NEW_EXE), "reference active");
    gb::override_active_for_testing(NEW_EXE);
    require(gb::rva(0x1d76b50) == 0x1d76e90 && gb::supported(NEW_EXE) && !gb::supported(gb::REFERENCE_EXE), "ported build active");
    require(gb::code(EngineCinematicState::GETTER_RVA, ported(EngineCinematicState::GETTER_RVA), EngineCinematicState::GETTER), "active-build code check");
    gb::override_active_for_testing(NEW_EXE + 1);
    require(gb::rva(0x1d76b50) == 0 && !gb::supported(NEW_EXE + 1), "unknown active build translates nothing");
    gb::override_active_for_testing(0);
    require(gb::active() == gb::REFERENCE_EXE, "override cleared");
}

}

int main()
{
    try {
        identification();
        translation();
        digests();
        digest_comparison();
        code_comparison();
        module_matchers();
        active_build();
    } catch (const std::exception& error) {
        std::cerr << "game_build test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "game_build tests passed\n";
    return 0;
}
