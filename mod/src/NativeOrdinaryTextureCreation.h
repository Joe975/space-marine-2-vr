#pragma once
#include "GameBuild.h"
#include "NativeTextureContract.h"
#include "NativeRenderProbe.h"
#include "GpuAllocationProbe.h"
#include "NativePoolMembership.h"
#include <functional>
#include <string>
#include <mutex>
#include <vector>

// Bounded owned creation. No native pool edits. Use is separately opt-in and
// authenticated by NativeOrdinaryBloom; this class reports creation only.
// Fresh factory results receive a native reference before backing initialization;
// only those owned results can grant the additional COM/resource lease.
class NativeOrdinaryTextureCreation {
public:
    struct Name {const char* text{};uint32_t length{},padding{};};
    struct Api {
        std::function<uintptr_t(uintptr_t,const Name*,uint64_t,uint32_t,uintptr_t)> create;
        std::function<int32_t(uintptr_t)> retain;
        std::function<void(uintptr_t,uintptr_t,uintptr_t)> preinitialize;
        std::function<void(uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t)> initialize;
        uintptr_t vtable{},factory_method{},preinitialize_method{},initialize_method{};
        bool instruction_guards{};
        // Diagnostic only (SM2VR_DIAG_NO_NATIVE_REFERENCE=1, 25 Oct level-exit freeze): skip the extra
        // native reference, leaving the fresh textures at the factory's count (0). Not safe for play.
        bool skip_reference{};
    };
    struct Origin {uint64_t dispatch{},render_id{},revision{};uint32_t token{},counter{},slot{};uintptr_t entry{},list{},renderer{};};
    struct Texture {
        std::string name;uintptr_t native{},identity{};int32_t refs_before{},refs_after{};
        bool native_reference{},initialized{};
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        std::optional<GpuAllocationProbe::Record> allocation;
        native_pool_membership::Result pool_membership;
    };
    struct OwnedPair {
        Origin origin;uint32_t thread{},variant{},row{};
        native_texture_contract::Snapshot contract;
        std::array<uintptr_t,2> native{},resource{},identity{};
    };
    // generations: no retired pair of this layout and GENERATIONS pairs exist; not attempted, a later job may still reuse one.
    enum class Refusal:uint32_t {none,scope,contract,factory,existing,reference,initialize,resource,allocation,changed,exception,pool,generations};
    // What a pair is made for: the pool targets it mirrors, their definition word, the views' flags and a name.
    struct Spec {std::array<uint32_t,2> ids{0xc4,0xc5};uint64_t definition{0x20000b0};D3D12_RESOURCE_FLAGS flags{D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};const char* name{"SM2VR_ORDINARY_RIGHT_BLOOM_"};};
    NativeOrdinaryTextureCreation()=default;
    explicit NativeOrdinaryTextureCreation(Spec spec):m_spec(spec){}
    static NativeOrdinaryTextureCreation& get(); // The right eye's bloom pair.
    static NativeOrdinaryTextureCreation& taa(); // The right eye's TAA pair (NativeOrdinaryTaa.h).
    bool arm_in_game(HMODULE,ID3D12Device*);
    // The same checked factory and device as an armed instance. Arming twice would read the pool
    // release's digest back through the guard's jump (NativePoolReleaseGuard).
    bool arm_like(const NativeOrdinaryTextureCreation&);
    // Exact typed API seam for real-path deterministic fixture tests.
    bool configure(ID3D12Device*,uintptr_t,Api);
    void consider(const Origin&,const native_texture_contract::Snapshot&,const std::array<NativeRenderProbe::Output,9>&,const std::function<bool()>& current);
    void write(std::ostream&,uint64_t dispatch)const;
    bool armed()const{return m_armed;}
    bool accepted()const{return m_attempted && m_refusal==Refusal::none;}
    Refusal refusal()const{return m_refusal;}
    const std::array<Texture,2>& textures()const{return m_textures;}
    // POD receipt only; native references and COM leases stay pinned here.
    std::optional<OwnedPair> owned_pair()const;
    // The native pool was rebuilt (24 Oct: leaving the armoury) and the pair no longer
    // matches it: retire the pair and allow one bootstrap pair, as at startup. Retired
    // textures keep their native references and COM leases for the process lifetime, as
    // before; nothing is released. `observed` is the rebuilt pool (a read-only capture).
    // The next consider() makes a retired pair of the bootstrap's layout
    // (native_texture_contract::same_layout) current again instead of calling the factory:
    // 26 Oct, every class change flips the pool between two layouts (one bank or two, 0x80)
    // and loads only move it (0x120), so the 14:46 run's 8 renewals needed 2 pairs, not 9.
    // At most GENERATIONS pairs exist per process (created, not renewed). False when there is no
    // accepted pair to retire, a creation is in progress, or the new layout has no pair and the
    // limit is reached (the log says so: OrdinaryBloomRenewed: refused).
    // 26 Oct 11:55: at the old cap of 4 the headset stayed flat 78 s until the pool happened to
    // return to the last pair's layout. 12 pairs (about 33 MB each at 1920x1080).
    static constexpr uint32_t GENERATIONS=12;
    bool renew(const native_texture_contract::Snapshot& observed);
    // Pairs created (each counts against GENERATIONS); a reuse adds none.
    uint32_t generation()const{std::lock_guard lock{m_mutex};return m_generation;}
    size_t retired()const{std::lock_guard lock{m_mutex};return m_retired.size();}
    // Retired pairs made current again.
    uint64_t reuses()const{std::lock_guard lock{m_mutex};return m_reuses;}
    // The last renewal, for its log line: whether its pair is (or, before the bootstrap, is expected
    // to be) a retired one made current again, and the pairs created once it is taken.
    struct Renewal {bool reused{};uint32_t generation{};};
    Renewal renewal()const{std::lock_guard lock{m_mutex};if(m_attempted)return {m_reused,m_generation};return {m_renewal_reuses,m_generation+(m_renewal_reuses?0u:1u)};}
    void record_native_use(uint64_t dispatch){std::lock_guard lock{m_mutex};if(m_attempted && m_refusal==Refusal::none && dispatch>m_origin.dispatch && !m_first_use)m_first_use=dispatch;}
    static bool matches(uint32_t,const std::array<std::array<uint8_t,16>,4>&,const std::array<uint8_t,12>&);
    inline static constexpr std::array<uintptr_t,4> RVAS{0x2653c20,0x2113cc0,0x264b7b0,0x2119040};
    inline static constexpr std::array<game_build::Code,4> BYTES{
        game_build::hashed(0x2653c20,16,"d3e234ca80b6666f2420340814b60fc340a98f3317147cba38492a96adc6aac4"),
        game_build::hashed(0x2113cc0,16,"36582cc2f38870fa802e61b80595760e27014b4077f5a107e424d8baccc4b55f"),
        game_build::hashed(0x264b7b0,16,"33980a6fc601b079b5cd78cc93c9f27f9e1fed5020992bfe41266983364847a5"),
        game_build::hashed(0x2119040,16,"8d5b7ca73a197e7a6d1074a4f736c4bb5e390ce84775555f5bec2cf5933a6db6")};
    inline static constexpr game_build::Code RETAIN_BYTES=game_build::hashed(0x26e29d0,12,"cfc263c4ad605a1088a89ead82f9d69dd7f8d9322306c6e60f34f15eaeabd1b0");
private:
    // A retired pair and the contract it was created (or last made current) against. spent: it failed
    // a reuse check once and is never made current again (its leases stay pinned).
    struct Retired {std::array<Texture,2> textures;native_texture_contract::Snapshot contract;bool spent{};};
    bool contract(const Origin&,const native_texture_contract::Snapshot&,const std::array<NativeRenderProbe::Output,9>&)const;
    // What a usable retired pair takes on: its contract re-based onto this pool, and the pool lookups that passed.
    struct Adoption {native_texture_contract::Snapshot contract;native_pool_membership::Result source;std::array<native_pool_membership::Result,2> owned;};
    enum class Reuse {usable,unusable,changed};
    Reuse reusable(const Retired&,const Origin&,const native_texture_contract::Snapshot&,const std::array<NativeRenderProbe::Output,9>&,const std::function<bool()>& current,size_t row,Adoption&)const;
    Spec m_spec;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;uintptr_t m_device_identity{},m_base{};Api m_api;
    GpuAllocationProbe m_allocations;
    bool m_armed{},m_attempted{},m_reused{},m_renewal_reuses{};Refusal m_refusal{Refusal::none};Origin m_origin;uint32_t m_thread{},m_variant{},m_row{};
    std::array<Texture,2> m_textures;std::vector<Retired> m_retired;uint32_t m_generation{};uint64_t m_reuses{};
    native_texture_contract::Snapshot m_contract;
    uint64_t m_first_use{};
    native_pool_membership::Result m_pool_source;
    mutable std::mutex m_mutex;
};
