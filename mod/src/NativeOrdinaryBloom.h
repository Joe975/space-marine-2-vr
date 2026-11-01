#pragma once
#include "GameBuild.h"
#include "NativeTemporalProbe.h"
#include "NativeOrdinaryTextureCreation.h"
#include "NativeOrdinaryPacket.h"
#include "OrdinaryStereoRecovery.h"
#include "OrdinaryToneSource.h"
#include <filesystem>

// Tone-source diagnostic stats (OrdinaryToneSource); defined in NativeOrdinaryBloom.cpp.
OrdinaryToneSource::Stats ordinary_tone_source_stats();
// Flare probe window (NativeFlareProbe::line), empty when not installed; defined in NativeOrdinaryBloom.cpp.
std::string ordinary_flare_probe_line();
// Adaptation mirror window (NativeAdaptationMirror::line), empty when not configured; defined in NativeOrdinaryBloom.cpp.
std::string ordinary_adaptation_mirror_line();
// Adaptation share totals (NativeOrdinaryGrading::adaptation_line), empty when grading is not hooked; defined in NativeOrdinaryBloom.cpp.
std::string ordinary_adaptation_share_line();

// Four-job desktop experiment: unchanged bootstrap, then three owned right-bloom
// destinations. Never edits pool records or previous banks; all leases are pinned.
class NativeOrdinaryBloom {
public:
    using Scope=NativeTemporalProbe::Scope;
    using Owned=NativeOrdinaryTextureCreation::OwnedPair;
    using Selector=uintptr_t(*)(uintptr_t,uint32_t,uint32_t);
    using Colour=void(*)(uintptr_t,uintptr_t,float,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,const void*);
    using Tone=void(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t,int32_t,const void*,uintptr_t);
    static constexpr uintptr_t SELECTOR=0x1f95590,COLOUR=0x1fac000,TONE=0x1e90410;
    inline static constexpr game_build::Code SELECTOR_BYTES=game_build::hashed(0x1f95590,36,"ad5b3ac9188ca349f3b2efe7bb2c8d7730bcfb53c219c05f0e5abb2f3608832b");
    inline static constexpr game_build::Code COLOUR_BYTES=game_build::hashed(0x1fac000,16,"442a19659fe9096de3a84e01a789d0005ab6710232eaa0a6a807ab732a813780");
    inline static constexpr game_build::Code TONE_BYTES=game_build::hashed(0x1e90410,16,"d01c01fd8935a72f21602837bef319409eb8a281658a18b55eabbb5e514cbe8e");
    static constexpr uintptr_t SELECT_CALL=0x180a6d0,COLOUR_CALL=0x180ae4b;
    inline static constexpr game_build::Code SELECT_CALL_BYTES=game_build::hashed(0x180a6d0,38,"0fa5b507d7b168861d11937b71c816c802e5a44f25e71104ad162d46f8ec9118");
    inline static constexpr game_build::Code COLOUR_CALL_BYTES=game_build::hashed(0x180ae4b,43,"5a800f4c19b3c4b4c1376ac0f5b6d693881882dbebab29923b5e2c4a399638e0");
    enum class Refusal:uint32_t {none,scope,callsite,duplicate,creation,continuity,contract,pool,changed,temporal,colour,export_bank,packet,exception};
    // First failed predicate of an owned-texture contract check (receipt/log only).
    enum class ContractFailure:uint32_t {none,owned,renderer,shape,row,selected,pool,owned_wrapper,collision,distinct,changed,injected};
    static const char* name(ContractFailure value){switch(value){case ContractFailure::none:return "none";case ContractFailure::owned:return "owned";case ContractFailure::renderer:return "renderer";
        case ContractFailure::shape:return "shape";case ContractFailure::row:return "row";case ContractFailure::selected:return "selected";case ContractFailure::pool:return "pool";
        case ContractFailure::owned_wrapper:return "owned_wrapper";case ContractFailure::collision:return "collision";case ContractFailure::distinct:return "distinct";case ContractFailure::changed:return "changed";case ContractFailure::injected:return "injected";}return "unknown";}
    using ScopeEvidence=ordinary_scene_scope::Evidence;
    // Read-only native inventory: precisely two ordinary slots, and every other
    // bounded native slot explicitly excluded from ordinary/PIP rendering.
    static ScopeEvidence inspect(const Scope&);
    struct Record {
        Scope scope;uint32_t thread{};uintptr_t selector_caller{};std::array<uintptr_t,3> selector_arguments{};
        ScopeEvidence scope_evidence;
        bool bootstrap{},selected{},reset{},resumed{},temporal{},colour{},tone{},accepted{};
        // suspended: refused recoverably; native selection ran and the pair must not be shown.
        // recovered: this accepted resumed job ended a suspension.
        // restart: a bootstrap that starts a fresh chain after a scene change, keeping the owned pair.
        bool suspended{},recovered{},restart{};ContractFailure contract_failure{};uint32_t contract_difference{};
        Refusal refusal{Refusal::none};uintptr_t destination{},resource{},identity{},observed_previous{},invoked_previous{},colour_input{},tone_input{};
        uintptr_t colour_caller{};uint32_t temporal_calls{},colour_calls{},tone_calls{};
        uint64_t previous_dispatch{},creation_dispatch{};std::optional<NativeOrdinaryPacket::Sdk> sdk;
        std::optional<native_texture_contract::Snapshot> contract;
        native_pool_membership::Result source_membership;std::array<native_pool_membership::Result,2> owned_membership;
    };
    static NativeOrdinaryBloom& get();
    void continuous(bool value){m_continuous=value;}
    // Continuous mode only: a recoverable refusal (changed texture contract, a packet
    // refused before any write for a transient camera mismatch (NativeOrdinaryPacket::
    // transient), or a non-resumable job while suspended) suspends instead of stopping. The callback
    // asks the producer for native mono; resume_gate() admits a later fully checked
    // resumed job. Without this, every refusal stays a permanent stop.
    using Clock=OrdinaryStereoRecovery::Clock;
    void recovery(std::function<void()> suspend,OrdinaryStereoRecovery::Settings settings={},std::function<Clock::time_point()> now={});
    bool resume_gate();
    // The game's cutscene flag, read by resume_gate on every job while suspended (a cutscene
    // cut settles faster than a level load, OrdinaryStereoRecovery). Empty: never a cutscene.
    void cutscene_source(std::function<bool()> source){std::lock_guard lock{m_mutex};m_cutscene=std::move(source);}
    // A rebuilt native pool (native_texture_contract::rebuilt) that persists for `after`
    // while suspended: renew(observed) retires the owned pair (NativeOrdinaryTextureCreation::renew,
    // given the gate's read-only capture of the rebuilt pool), and the next admitted job is a fresh
    // bootstrap that takes the pair for the pool's layout, as at startup: a retired pair made for it
    // again, or a new one. Its acceptance ends the suspension. Empty: a rebuilt pool keeps stereo suspended.
    using Renew=std::function<bool(const native_texture_contract::Snapshot&)>;
    void renewal(Renew renew,std::chrono::milliseconds after=std::chrono::milliseconds(3000)){std::lock_guard lock{m_mutex};m_renew=std::move(renew);m_renew_after=after;m_rebuilt_since={};}
    // Live diagnostic (SM2VR_DIAG_STEREO_SUSPEND_S): refuse one otherwise valid job's
    // contract at the first eligible job, then every period, through the same recovery.
    void inject_suspensions(std::chrono::milliseconds period){std::lock_guard lock{m_mutex};m_inject_period=period;m_inject_next={};}
    // Live diagnostic (SM2VR_DIAG_RECORDS_MOVE_S): make the current contract name a stale
    // record-table address, so the next check sees exactly the 21 Oct table move (0x100)
    // and the probe must re-base onto the real address. First eligible job, then every period.
    void inject_record_moves(std::chrono::milliseconds period){std::lock_guard lock{m_mutex};m_move_period=period;m_move_next={};}
    bool suspended()const{std::lock_guard lock{m_mutex};return m_suspended;}
    bool stopped()const{std::lock_guard lock{m_mutex};return m_stopped;}
    // Cold restart (OrdinaryColdRestart, continuous mode only): back to the state before the first
    // job. The owned pair is retired (renewal() with a read-only capture of the pool, process-lifetime
    // leases kept) so the next job is a bootstrap that takes the pair for the pool's layout, exactly
    // as at startup (usually the one just retired, re-checked); the chain, contract baseline and
    // recovery policy start over. False when no pair can be renewed (generation limit, creation
    // refused) or the mode is not continuous: the stop then stands.
    bool cold_restart();
    uint64_t cold_restarts()const{std::lock_guard lock{m_mutex};return m_cold_restarts;}
    struct RecoveryStatus {OrdinaryStereoRecovery::Stats policy;uint64_t suspended_records{},last_accepted{};uint32_t probe_difference{};bool stopped{};
        // Contract re-based onto a moved native record table (native_texture_contract::rebase_records).
        uint64_t rebases{};uintptr_t rebased_from{},rebased_to{};int32_t rebased_count_from{},rebased_count_to{};
        // Owned pairs retired after a rebuilt pool; renewing: waiting for the fresh bootstrap.
        uint64_t renewals{};uint32_t renewed_difference{};bool renewing{};
        // Fresh chains started after a camera cut that could not continue (scene changes).
        uint64_t restarts{};};
    RecoveryStatus recovery_status()const;
    std::function<void(const Scope&,uintptr_t)> tone_output; // Synchronous, after native tone pass.
    std::function<void(const Scope&,std::array<uintptr_t,3>)> tone_inputs;
    std::function<void(const Scope&,uintptr_t)> colour_input;
    // Diagnostic (SM2VR_DIAG_TONE_SOURCE_CONTROL): a file polled once a second whose contents
    // (OrdinaryToneSource::parse) choose which tone inputs the right eye takes from the left eye.
    void tone_source_control(std::filesystem::path path){std::lock_guard lock{m_tone_mutex};m_tone_control=std::move(path);m_tone_poll={};}
    OrdinaryToneSource::Stats tone_source_stats()const{std::lock_guard lock{m_tone_mutex};return m_tone_source.stats();}
    OrdinaryToneSource::Inputs tone_source(uint64_t dispatch,uint32_t slot,OrdinaryToneSource::Inputs inputs);
    static bool matches(uint32_t,const std::array<uint8_t,36>&,const std::array<uint8_t,16>&,const std::array<uint8_t,16>&,const std::array<uint8_t,38>&,const std::array<uint8_t,43>&);
    bool install_in_game(HMODULE);
    // Real detours and memory reads in tests. Typed providers do not claim leases
    // or shipping instruction guards; production providers use accepted receipts.
    bool install(void*,void*,void*,NativeTemporalProbe::Provider,std::function<std::optional<Owned>()>,std::function<std::optional<NativeOrdinaryPacket::Receipt>(uint64_t)>,std::function<void()>,uintptr_t base=0);
    bool installed()const{return m_installed.load(std::memory_order_acquire);}
    uintptr_t previous(NativeTemporalProbe::Event*,const std::array<uintptr_t,7>&);
    void after_temporal(const NativeTemporalProbe::Event&);
    void complete(const NativeRenderProbe::Snapshot&);
    std::optional<Record> record(uint64_t)const;
    void write(std::ostream&,uint64_t)const;
private:
    static uintptr_t on_selector(uintptr_t,uint32_t,uint32_t);
    static void on_colour(uintptr_t,uintptr_t,float,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,const void*);
    static void on_tone(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t,int32_t,const void*,uintptr_t);
    std::optional<Scope> current(ScopeEvidence* evidence=nullptr)const;
    static bool same(const Scope&,const Scope&);
    void fail(Record&,Refusal);
    bool contract(Record&,const Owned&);
    bool recover(Record&,Refusal,bool attempt=true,bool transition=false);
    const Record* predecessor(uint64_t dispatch)const;
    bool room(){if(m_records.size()<4)return true;if(!m_continuous)return false;m_records.erase(m_records.begin());return true;}
    std::atomic_bool m_installed{};bool m_guards{},m_stopped{};uintptr_t m_base{};
    Selector m_selector{};Colour m_colour{};Tone m_tone{};
    NativeTemporalProbe::Provider m_provider;std::function<std::optional<Owned>()> m_owned;
    std::function<std::optional<NativeOrdinaryPacket::Receipt>(uint64_t)> m_packet;std::function<void()> m_stop;
    mutable std::mutex m_mutex;std::mutex m_install;std::vector<Record> m_records;
    bool m_continuous{};uint64_t m_creation_dispatch{};
    std::function<void()> m_suspend;std::function<Clock::time_point()> m_now;std::function<bool()> m_cutscene;Renew m_renew;std::chrono::milliseconds m_renew_after{3000};Clock::time_point m_rebuilt_since{};bool m_renewing{};uint64_t m_renewals{},m_restarts{},m_cold_restarts{};uint32_t m_renewed_difference{};OrdinaryStereoRecovery m_recovery;OrdinaryStereoRecovery::Settings m_recovery_settings;
    std::chrono::milliseconds m_inject_period{},m_move_period{};Clock::time_point m_inject_next{},m_move_next{};
    bool m_suspended{};std::optional<Record> m_last_accepted;uint64_t m_suspended_records{};uint32_t m_probe_difference{};
    // Current contract for the owned pair created at m_baseline_origin; replaces the creation contract after a rebase.
    std::optional<native_texture_contract::Snapshot> m_baseline;uint64_t m_baseline_origin{},m_rebases{};uintptr_t m_rebased_from{},m_rebased_to{};int32_t m_rebased_count_from{},m_rebased_count_to{};
    std::optional<Owned> owned_current()const; // m_owned() with the re-based contract applied. Caller holds m_mutex.
    inline static thread_local std::optional<uint64_t> s_colour;
    mutable std::mutex m_tone_mutex;OrdinaryToneSource m_tone_source;std::filesystem::path m_tone_control;std::chrono::steady_clock::time_point m_tone_poll{};
};
