#include "NativeOrdinaryBloom.h"
#include "NativeOrdinaryTaa.h"
#include "GameBuild.h"
#include "NativeObjectVisibilityProbe.h"
#include "NativeWorkerVisibilityProbe.h"
#include "NativeOrdinaryVolumeFrame.h"
#include "NativeOrdinaryShadingControl.h"
#include "NativeOrdinaryLightVisibility.h"
#include "NativeOrdinaryReflectionSelection.h"
#include "NativeOrdinaryShadowFit.h"
#include "NativeOrdinaryShadowCache.h"
#include "NativeOrdinaryShadowLights.h"
#include "NativeOcclusionHarvestOnce.h"
#include "NativeHzbControl.h"
#include "NativeOrdinaryLightSection.h"
#include "NativeSecondCameraPlainLightTest.h"
#include "NativeOrdinaryCamera.h"
#include "NativeOrdinaryGrading.h"
#include "NativeOrdinaryLateEffect.h"
#include "NativeOrdinarySharpen.h"
#include "NativeTrackedOrdinaryCamera.h"
#include "PerformanceTrace.h"
#include "NativeSceneWaitTiming.h"
#include "NativeDepthReadyTiming.h"
#include "NativeFlareProbe.h"
#include "NativeAdaptationMirror.h"
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace {
using namespace native_texture_contract;
bool shape(const Snapshot& a,const Snapshot& b){return !difference(a,b);}
}
NativeOrdinaryBloom& NativeOrdinaryBloom::get(){static auto* value=new NativeOrdinaryBloom;return *value;}
bool NativeOrdinaryBloom::matches(uint32_t stamp,const std::array<uint8_t,36>& selector,const std::array<uint8_t,16>& colour,const std::array<uint8_t,16>& tone,const std::array<uint8_t,38>& select_call,const std::array<uint8_t,43>& colour_call){
    return game_build::code(stamp,SELECTOR,selector,SELECTOR_BYTES) && game_build::code(stamp,COLOUR,colour,COLOUR_BYTES) && game_build::code(stamp,TONE,tone,TONE_BYTES) &&
        game_build::code(stamp,SELECT_CALL,select_call,SELECT_CALL_BYTES) && game_build::code(stamp,COLOUR_CALL,colour_call,COLOUR_CALL_BYTES);
}
bool NativeOrdinaryBloom::install_in_game(HMODULE module){
    const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    std::array<uint8_t,36> selector{};std::array<uint8_t,16> colour{},tone{};std::array<uint8_t,38> select_call{};std::array<uint8_t,43> colour_call{};
    if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage<0x508cab0 ||
        !game_build::supported(nt.FileHeader.TimeDateStamp) || !game_build::rva(SELECTOR) || !game_build::rva(COLOUR) || !game_build::rva(TONE) || !game_build::rva(SELECT_CALL) || !game_build::rva(COLOUR_CALL) ||
        !read(base+game_build::rva(SELECTOR),selector) || !read(base+game_build::rva(COLOUR),colour) || !read(base+game_build::rva(TONE),tone) || !read(base+game_build::rva(SELECT_CALL),select_call) || !read(base+game_build::rva(COLOUR_CALL),colour_call) || !matches(nt.FileHeader.TimeDateStamp,selector,colour,tone,select_call,colour_call))return false;
    wchar_t grading[8]{};if(GetEnvironmentVariableW(L"SM2VR_ORDINARY_SHARED_GRADING",grading,8)==1 && grading[0]==L'1' && !NativeOrdinaryGrading::get().install_in_game(base))return false;
    wchar_t volume[8]{};if(GetEnvironmentVariableW(L"SM2VR_ORDINARY_SHARED_VOLUME_FRAME",volume,8)==1 && volume[0]==L'1' && !NativeOrdinaryVolumeFrame::get().install_in_game(base))return false;
    wchar_t shading[32768]{};const auto shading_size=GetEnvironmentVariableW(L"SM2VR_ORDINARY_SHADING_CONTROL",shading,32768);
    if(shading_size && (shading_size>=32768 || !NativeOrdinaryShadingControl::get().install_in_game(base,shading)))return false;
    if(shading_size && !NativeOrdinaryLightVisibility::get().install_in_game(base,shading))return false;
    if(shading_size && !NativeOrdinaryLightVisibility::get().install_scene_sites_in_game(base,shading))return false;
    wchar_t reflections[8]{};
    if(!(GetEnvironmentVariableW(L"SM2VR_ORDINARY_STEREO_REFLECTIONS",reflections,8)==1 && reflections[0]==L'0') &&
       !NativeOrdinaryReflectionSelection::get().install_in_game(base,shading_size?std::filesystem::path{shading}:std::filesystem::path{}))return false;
    if(shading_size && !NativeOrdinaryShadowFit::get().install_in_game(base))return false;
    if(shading_size && !NativeOrdinaryShadowCache::get().install_in_game(base,shading))return false;
    // Optional: a refused install leaves each eye's own shadowed lights (OrdinaryShadowLights: installed=false).
    if(shading_size)NativeOrdinaryShadowLights::get().install_in_game(base,shading);
    if(shading_size)NativeOrdinarySpotShadowShare::get().install_in_game(base,shading);
    // Diagnostic (docs/RIGHT_EYE_FLICKER_2026-10-28.md): optional; a refused install logs installed=false.
    std::wstring harvest(32768,0);const auto harvest_size=GetEnvironmentVariableW(L"SM2VR_DIAG_HARVEST_ONCE_CONTROL",harvest.data(),32768);harvest.resize(harvest_size<32768?harvest_size:0);
    if(harvest_size && harvest_size<32768)NativeOcclusionHarvestOnce::get().install_in_game(base,harvest);
    // Occlusion culling for both eyes (docs/HZB_STEREO_2026-11-01.md): on unless -NativeHzb keeps the game's own.
    // Optional: a refused install logs no NativeHzbControl line and leaves the switch cleared.
    std::wstring hzb(32768,0);const auto hzb_size=GetEnvironmentVariableW(L"SM2VR_HZB_CONTROL",hzb.data(),32768);hzb.resize(hzb_size<32768?hzb_size:0);
    wchar_t native_hzb[8]{};const bool keep_native_hzb=GetEnvironmentVariableW(L"SM2VR_NATIVE_HZB",native_hzb,8)==1 && native_hzb[0]==L'1';
    if(!keep_native_hzb)NativeHzbControl::get().install_in_game(base,hzb);
    // The right eye's tiles index its own light section (default slot; light_section.txt beside
    // shading.txt says native or slot). A refused install logs installed=false.
    if(shading_size)NativeOrdinaryLightSection::get().install_in_game(base,std::filesystem::path{shading}.parent_path()/"light_section.txt");
    std::wstring plain_test(32768,0);const auto plain_size=GetEnvironmentVariableW(L"SM2VR_DIAG_SECOND_CAMERA_PLAIN_TEST_CONTROL",plain_test.data(),32768);plain_test.resize(plain_size<32768?plain_size:0);
    if(plain_size && plain_size<32768)NativeSecondCameraPlainLightTest::get().install_in_game(base,nt.FileHeader.TimeDateStamp,plain_test);
    if(PerformanceTrace::get().enabled() && !NativeSceneWaitTiming::get().install_in_game(base))return false;
    if(PerformanceTrace::get().probes() && !NativeDepthReadyTiming::get().install_in_game(base))return false;
    wchar_t dependencies[8]{};
    if(PerformanceTrace::get().probes() && GetEnvironmentVariableW(L"SM2VR_PROFILE_QUERY_DEPENDENCIES",dependencies,8)==1 && dependencies[0]==L'1' && !NativeQueryDependencyProbe::get().install_in_game(base))return false;
    wchar_t visibility[32768]{};const auto visibility_size=GetEnvironmentVariableW(L"SM2VR_ORDINARY_DRAW_VISIBILITY",visibility,32768);
    if(visibility_size && (visibility_size>=32768 || !NativeObjectVisibilityProbe::get().install_in_game(base,visibility)))return false;
    wchar_t workers[8]{};if(GetEnvironmentVariableW(L"SM2VR_ORDINARY_WORKER_VISIBILITY",workers,8)==1 && workers[0]==L'1' &&
        (!visibility_size || !NativeWorkerVisibilityProbe::get().install_in_game(base,visibility)))return false;
    wchar_t late[8]{};if(GetEnvironmentVariableW(L"SM2VR_ORDINARY_SKIP_LATE_EFFECT",late,8)==1 && late[0]==L'1' && !NativeOrdinaryLateEffect::get().install_in_game(base))return false;
    wchar_t early[8]{};if(GetEnvironmentVariableW(L"SM2VR_ORDINARY_DISABLE_EARLY_EFFECT",early,8)==1 && early[0]==L'1' && !NativeOrdinaryLateEffect::get().install_early_in_game(base))return false;
    wchar_t pixels[8]{},shared[8]{};const bool sharpen=(GetEnvironmentVariableW(L"SM2VR_ORDINARY_SDK_PIXELS",pixels,8)==1 && pixels[0]==L'1') || (GetEnvironmentVariableW(L"SM2VR_ORDINARY_SHARED_SHARPEN_EXPOSURE",shared,8)==1 && shared[0]==L'1');if(sharpen && !NativeOrdinarySharpen::get().install_in_game(base))return false;
    if(!install(reinterpret_cast<void*>(base+game_build::rva(SELECTOR)),reinterpret_cast<void*>(base+game_build::rva(COLOUR)),reinterpret_cast<void*>(base+game_build::rva(TONE)),[]()->std::optional<Scope>{
        const auto scene=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();if(!scene || !pair)return {};return Scope{*scene,*pair};
    },[]{return NativeOrdinaryTextureCreation::get().owned_pair();},[](uint64_t dispatch){return NativeOrdinaryPacket::get().receipt(dispatch);},[]{NativeOrdinaryCamera::get().request_stop();NativeTrackedOrdinaryCamera::get().request_stop();},base))return false;
    // Effective only in continuous (headset) mode; the producer stays native mono while suspended.
    recovery([]{NativeTrackedOrdinaryCamera::get().request_suspend();});
    wchar_t flares[8]{},flare_hide[MAX_PATH]{};const bool flare_probe=GetEnvironmentVariableW(L"SM2VR_DIAG_FLARE_PROBE",flares,8)==1 && flares[0]==L'1';
    const auto flare_hide_size=GetEnvironmentVariableW(L"SM2VR_DIAG_FLARE_HIDE_CONTROL",flare_hide,MAX_PATH);const bool hide_control=flare_hide_size && flare_hide_size<MAX_PATH;
    if(hide_control)NativeFlareProbe::get().configure_hide(false,std::filesystem::path{flare_hide});
    if((flare_probe || hide_control) && !NativeFlareProbe::get().install_in_game(base))return false;
    wchar_t mirror[8]{},mirror_control[MAX_PATH]{};const bool mirror_adaptation=GetEnvironmentVariableW(L"SM2VR_ADAPTATION_MIRROR",mirror,8)==1 && mirror[0]==L'1';
    const auto mirror_control_size=GetEnvironmentVariableW(L"SM2VR_ADAPTATION_MIRROR_CONTROL",mirror_control,MAX_PATH);
    if(mirror_adaptation || (mirror_control_size && mirror_control_size<MAX_PATH))NativeAdaptationMirror::get().configure(mirror_adaptation,mirror_control_size<MAX_PATH?std::filesystem::path{mirror_control}:std::filesystem::path{});
    wchar_t tone_control[MAX_PATH]{};if(const auto n=GetEnvironmentVariableW(L"SM2VR_DIAG_TONE_SOURCE_CONTROL",tone_control,MAX_PATH);n && n<MAX_PATH)tone_source_control(tone_control);
    wchar_t inject[16]{};if(GetEnvironmentVariableW(L"SM2VR_DIAG_STEREO_SUSPEND_S",inject,16) && _wtoi(inject)>0)inject_suspensions(std::chrono::seconds(_wtoi(inject)));
    wchar_t move[16]{};if(GetEnvironmentVariableW(L"SM2VR_DIAG_RECORDS_MOVE_S",move,16) && _wtoi(move)>0)inject_record_moves(std::chrono::seconds(_wtoi(move)));
    return true;
}
bool NativeOrdinaryBloom::install(void* selector,void* colour,void* tone,NativeTemporalProbe::Provider provider,std::function<std::optional<Owned>()> owned,std::function<std::optional<NativeOrdinaryPacket::Receipt>(uint64_t)> packet,std::function<void()> stop,uintptr_t base){
    std::lock_guard lock{m_install};if(m_installed || m_selector || !selector || !colour || !tone || !provider || !owned || !packet || !stop)return false;
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED)return false;
    std::array<void*,3> targets{selector,colour,tone},replacements{reinterpret_cast<void*>(&on_selector),reinterpret_cast<void*>(&on_colour),reinterpret_cast<void*>(&on_tone)},originals{};
    size_t created{};for(;created<3;++created)if(MH_CreateHook(targets[created],replacements[created],&originals[created])!=MH_OK)break;
    if(created!=3){while(created)MH_RemoveHook(targets[--created]);return false;}
    m_provider=std::move(provider);m_owned=std::move(owned);m_packet=std::move(packet);m_stop=std::move(stop);m_base=base;m_guards=base!=0;m_records.reserve(4);
    m_selector=reinterpret_cast<Selector>(originals[0]);m_colour=reinterpret_cast<Colour>(originals[1]);m_tone=reinterpret_cast<Tone>(originals[2]);
    size_t enabled{};for(;enabled<3;++enabled)if(MH_EnableHook(targets[enabled])!=MH_OK)break;
    if(enabled!=3){for(auto target:targets){MH_DisableHook(target);MH_RemoveHook(target);}m_selector=nullptr;m_colour=nullptr;m_tone=nullptr;return false;}
    m_installed.store(true,std::memory_order_release);return true;
}
bool NativeOrdinaryBloom::same(const Scope& a,const Scope& b){
    return a.scene.dispatch_sequence==b.scene.dispatch_sequence && a.scene.ordinal==b.scene.ordinal && a.scene.renderer==b.scene.renderer && a.scene.entry==b.scene.entry && a.scene.list==b.scene.list &&
        a.scene.slot==b.scene.slot && a.scene.render_counter_before==b.scene.render_counter_before && a.scene.camera_revision_before==b.scene.camera_revision_before && a.scene.view_flags_before==b.scene.view_flags_before &&
        a.pair.left.render_id==b.pair.left.render_id && a.pair.left.revision==b.pair.left.revision && a.pair.left.token==b.pair.left.token && a.pair.right.entry==b.pair.right.entry;
}
NativeOrdinaryBloom::ScopeEvidence NativeOrdinaryBloom::inspect(const Scope& scope){
    return ordinary_scene_scope::inspect(scope,1); // Preserve the exact right-only selector requirement.
}
std::optional<NativeOrdinaryBloom::Scope> NativeOrdinaryBloom::current(ScopeEvidence* evidence)const{
    const auto scope=m_provider();if(!scope){if(evidence)evidence->mask=256;return {};}
    auto observed=inspect(*scope);const auto again=inspect(*scope);if(observed!=again)observed.mask|=512;
    if(evidence)*evidence=observed;if(observed.mask)return {};return scope;
}
void NativeOrdinaryBloom::fail(Record& record,Refusal reason){if(record.refusal==Refusal::none)record.refusal=reason;record.suspended=false;m_stopped=true;m_recovery.exhaust();m_stop();}
void NativeOrdinaryBloom::recovery(std::function<void()> suspend,OrdinaryStereoRecovery::Settings settings,std::function<Clock::time_point()> now){
    std::lock_guard lock{m_mutex};m_suspend=std::move(suspend);m_recovery_settings=settings;m_recovery=OrdinaryStereoRecovery{settings};m_now=now?std::move(now):std::function<Clock::time_point()>{[]{return Clock::now();}};
}
bool NativeOrdinaryBloom::cold_restart(){
    std::lock_guard lock{m_mutex};if(!installed() || !m_continuous)return false;
    // A pair that exists must be retired first, or the startup bootstrap refuses it (creation).
    // The pool as it is now (read-only, as resume_gate probes it): its layout picks the next pair.
    std::optional<Owned> owned;Snapshot observed;try{owned=m_owned();if(owned)observed=capture(owned->contract.module,{},2,owned->origin.counter);}catch(...){return false;}
    if(owned && !(m_renew && m_renew(observed)))return false;
    if(owned)OrdinaryStereoRecovery::increment(m_renewals);
    m_records.clear();m_last_accepted.reset();m_baseline.reset();m_baseline_origin=0;m_creation_dispatch=0;
    m_suspended=false;m_renewing=false;m_rebuilt_since={};m_probe_difference=0;
    m_recovery=OrdinaryStereoRecovery{m_recovery_settings};m_stopped=false;OrdinaryStereoRecovery::increment(m_cold_restarts);return true;
}
// Caller holds m_mutex. Returns false when this refusal must stay fatal. attempt=false:
// an adjacent job admitted before the suspension reached the producer; it neither
// starts an episode nor spends a granted attempt. transition: a level-transition packet
// refusal; the episode holds on the flat panel until the load settles (OrdinaryStereoRecovery).
bool NativeOrdinaryBloom::recover(Record& record,Refusal reason,bool attempt,bool transition){
    // A renewal has no accepted job to resume from: its bootstrap starts afresh.
    if(!m_continuous || !m_suspend || !m_now || (!m_last_accepted && !m_renewing) || m_stopped)return false;
    if(record.refusal==Refusal::none)record.refusal=reason;
    if(!(m_suspended && !attempt) && !m_recovery.suspend(m_now(),transition))return false; // Exhausted: permanent stop, as before.
    record.suspended=true;m_suspended=true;++m_suspended_records;m_suspend();return true;
}
const NativeOrdinaryBloom::Record* NativeOrdinaryBloom::predecessor(uint64_t dispatch)const{
    for(const auto& r:m_records)if(r.scope.scene.dispatch_sequence==dispatch)return &r;
    if(m_last_accepted && m_last_accepted->scope.scene.dispatch_sequence==dispatch)return &*m_last_accepted;
    return nullptr;
}
std::optional<NativeOrdinaryBloom::Owned> NativeOrdinaryBloom::owned_current()const{
    auto owned=m_owned();if(owned && m_baseline && m_baseline_origin==owned->origin.dispatch)owned->contract=*m_baseline;return owned;
}
bool NativeOrdinaryBloom::resume_gate(){
    std::lock_guard lock{m_mutex};if(m_stopped)return false;if(!m_suspended)return true;
    // Read-only probe of the global pool against the current contract: no wrappers,
    // leases or native calls. The resumed job re-runs the complete contract check.
    bool clear{};Snapshot observed;try{if(const auto owned=owned_current()){
        observed=native_texture_contract::capture(owned->contract.module,{},2,owned->origin.counter);
        m_probe_difference=difference(observed,owned->contract);
        // A moved record table with everything else identical is a legitimate native
        // reallocation: adopt the new address. The attempt still re-checks the whole
        // contract, pool membership and wrappers; any other change stays refused.
        if(m_probe_difference==native_texture_contract::records_moved)if(const auto rebased=native_texture_contract::rebase_records(observed,owned->contract)){
            m_rebased_from=owned->contract.records;m_rebased_to=rebased->records;m_rebased_count_from=owned->contract.record_count;m_rebased_count_to=rebased->record_count;
            m_baseline=*rebased;m_baseline_origin=owned->origin.dispatch;OrdinaryStereoRecovery::increment(m_rebases);m_probe_difference=0;}
        clear=!m_probe_difference;}}catch(...){clear=false;}
    const auto now=m_now?m_now():Clock::now();
    if(m_renewing){
        // Waiting for the fresh bootstrap. A pair already created means its bootstrap was
        // refused; retire that one too (bounded by NativeOrdinaryTextureCreation::GENERATIONS).
        bool owned{};try{owned=m_owned().has_value();}catch(...){owned=true;}
        if(owned && m_renew && m_renew(observed)){OrdinaryStereoRecovery::increment(m_renewals);owned=false;}
        clear=!owned;if(clear)m_probe_difference=0;
    }
    else if(!clear && m_renew && native_texture_contract::rebuilt(m_probe_difference)){
        if(m_rebuilt_since==Clock::time_point{})m_rebuilt_since=now;
        if(now-m_rebuilt_since>=m_renew_after && m_renew(observed)){
            m_renewing=true;OrdinaryStereoRecovery::increment(m_renewals);m_renewed_difference=m_probe_difference;m_rebuilt_since={};
            m_last_accepted.reset();m_baseline.reset();m_probe_difference=0;clear=true;}
    }
    else m_rebuilt_since={};
    bool cutscene{};try{cutscene=m_cutscene && m_cutscene();}catch(...){cutscene=false;}
    return m_recovery.may_attempt(now,clear,cutscene,m_renewing);
}
NativeOrdinaryBloom::RecoveryStatus NativeOrdinaryBloom::recovery_status()const{
    std::lock_guard lock{m_mutex};return {m_recovery.stats(),m_suspended_records,m_last_accepted?m_last_accepted->scope.scene.dispatch_sequence:0,m_probe_difference,m_stopped,
        m_rebases,m_rebased_from,m_rebased_to,m_rebased_count_from,m_rebased_count_to,m_renewals,m_renewed_difference,m_renewing,m_restarts};
}
bool NativeOrdinaryBloom::contract(Record& record,const Owned& owned){
    const auto& s=record.scope.scene;const auto& origin=owned.origin;
    const auto refuse=[&](ContractFailure failure){record.contract_failure=failure;return false;};
    if(s.dispatch_sequence<=origin.dispatch || s.renderer!=origin.renderer || record.thread!=owned.thread || owned.variant!=uint32_t(owned.contract.bank_variant) || owned.row>=owned.contract.targets[0].initialization_words.size())return refuse(ContractFailure::owned);
    std::array<uintptr_t,9> wrappers{};constexpr std::array<size_t,5> inputs{0x20,0x28,0x30,0x78,0x180};
    if(!read(s.renderer+0x50,wrappers[0]) || !read(s.renderer+0x160,wrappers[2]) || !read(s.renderer+0x38,wrappers[3]))return refuse(ContractFailure::renderer);
    for(size_t i=0;i<5;++i)if(!read(s.renderer+inputs[i],wrappers[i+4]))return refuse(ContractFailure::renderer);
    // Left has already acquired the real native target this same dispatch. Read
    // its registered wrapper; never acquire and discard an extra pool reference.
    auto observed=capture(owned.contract.module,wrappers,2,s.render_counter_before);
    // Keep the refused observation in the receipt so the changed field is visible.
    if(const auto changed=difference(observed,owned.contract)){record.contract=observed;record.contract_difference=changed;return refuse(ContractFailure::shape);}
    const auto parity=s.render_counter_before&1;const auto& target=observed.targets[parity];if(owned.row>=target.wrappers.size())return refuse(ContractFailure::row);
    wrappers[1]=target.wrappers[owned.row];observed=capture(owned.contract.module,wrappers,2,s.render_counter_before);record.contract=observed;
    record.contract_difference=difference(observed,owned.contract);
    if(record.contract_difference)return refuse(ContractFailure::shape);
    if(observed.selected_id!=0xc4+parity || observed.wrappers[1].vtable!=owned.contract.wrappers[1].vtable || observed.wrappers[1].methods!=owned.contract.wrappers[1].methods)return refuse(ContractFailure::selected);
    record.source_membership=native_pool_membership::lookup(observed.pool,wrappers[1],observed.record_count);
    if(!record.source_membership.known || !record.source_membership.found || record.source_membership.record!=target.record){record.contract_failure=ContractFailure::pool;fail(record,Refusal::pool);return false;}
    for(size_t i=0;i<2;++i){
        record.owned_membership[i]=native_pool_membership::lookup(observed.pool,owned.native[i],observed.record_count);
        uintptr_t resource{};int32_t refs{};const auto wrapper= native_texture_contract::wrapper(owned.native[i]);
        if(!record.owned_membership[i].known || record.owned_membership[i].found){record.contract_failure=ContractFailure::pool;fail(record,Refusal::pool);return false;}
        if(!wrapper.readable || wrapper.vtable!=observed.wrappers[1].vtable || wrapper.methods!=observed.wrappers[1].methods || wrapper.flags!=(target.definition[0]|(uint64_t(1)<<50)) ||
            !read(owned.native[i]+0x278,resource) || resource!=owned.resource[i] || !resource || !owned.identity[i] || !read(owned.native[i]+0x70,refs) || refs<1)return refuse(ContractFailure::owned_wrapper);
        for(const auto& t:observed.targets)for(auto native:t.wrappers){uintptr_t borrowed{};if(!read(native+0x278,borrowed) || native==owned.native[i] || borrowed==resource)return refuse(ContractFailure::collision);}
    }
    if(owned.native[0]==owned.native[1] || owned.resource[0]==owned.resource[1] || owned.identity[0]==owned.identity[1])return refuse(ContractFailure::distinct);
    const auto again=current();if(!again || !same(record.scope,*again) || capture(owned.contract.module,wrappers,2,s.render_counter_before)!=observed)return refuse(ContractFailure::changed);
    record.creation_dispatch=origin.dispatch;record.destination=owned.native[parity];record.resource=owned.resource[parity];record.identity=owned.identity[parity];return true;
}
uintptr_t NativeOrdinaryBloom::on_selector(uintptr_t renderer,uint32_t even,uint32_t odd){
    PerformanceTrace::Scope timing{"ordinary_bloom_selector_cpu"};
    auto& self=get();const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());std::optional<size_t> index;uintptr_t selected{};
    if(even==NativeOrdinaryTaa::EVEN && odd==NativeOrdinaryTaa::ODD){
        auto& taa=NativeOrdinaryTaa::get();taa.selected();
        try{if(self.installed() && taa.enabled())if(const auto scope=self.current())if(const auto owned=taa.select(*scope,renderer,self.m_base))return owned;}catch(...){}
        return self.m_selector(renderer,even,odd);
    }
    try{if(self.installed() && even==0xc4 && odd==0xc5){
        const auto candidate=self.m_provider();ScopeEvidence evidence;const auto scope=self.current(&evidence);
        if(!scope && candidate && candidate->scene.slot==1){std::lock_guard lock{self.m_mutex};if(!self.m_stopped && self.room()){Record r;r.scope=*candidate;r.thread=GetCurrentThreadId();r.selector_caller=self.m_base?caller-self.m_base:caller;r.scope_evidence=evidence;r.selector_arguments={renderer,even,odd};index=self.m_records.size();self.m_records.push_back(r);self.fail(self.m_records.back(),Refusal::scope);}}
        if(scope){
        std::lock_guard lock{self.m_mutex};if(!self.m_stopped && self.room()){
            Record record;record.scope=*scope;record.scope_evidence=evidence;record.thread=GetCurrentThreadId();record.selector_caller=self.m_base?caller-self.m_base:caller;record.selector_arguments={renderer,even,odd};
            // While suspended only the last ACCEPTED job can be a predecessor, and
            // only through the gap-resume rules below; refused jobs never are.
            const bool suspended=self.m_suspended;
            const auto prior=suspended?self.m_last_accepted:self.m_records.empty()?std::optional<Record>{}:std::optional{self.m_records.back()};
            const auto duplicate=std::ranges::find_if(self.m_records,[&](const auto& r){return r.scope.scene.dispatch_sequence==scope->scene.dispatch_sequence;});
            if(duplicate!=self.m_records.end()){self.fail(*duplicate,Refusal::duplicate);}
            else {index=self.m_records.size();self.m_records.push_back(record);auto& r=self.m_records.back();
                const auto packet=self.m_packet(scope->scene.dispatch_sequence);auto owned=self.owned_current();
                // Diagnostic only: a stale table address in the current contract (see inject_record_moves).
                if(owned && self.m_continuous && self.m_move_period.count() && !suspended && !self.m_recovery.exhausted() && self.m_last_accepted && self.m_now && self.m_now()>=self.m_move_next){
                    self.m_move_next=self.m_now()+self.m_move_period;owned->contract.records+=0x10000;self.m_baseline=owned->contract;self.m_baseline_origin=owned->origin.dispatch;}
                // A tracked packet independently resets BOTH eyes after skipped
                // mono jobs. Admit only matching forward gaps in all three native
                // clocks, and reset bloom sampling; no old image is a predecessor.
                const auto gap=prior && scope->scene.dispatch_sequence>prior->scope.scene.dispatch_sequence?
                    scope->scene.dispatch_sequence-prior->scope.scene.dispatch_sequence:0;
                const bool resumed=self.m_continuous && packet && packet->tracked && packet->left_patched && packet->patched && !packet->continued &&
                    prior && gap>1 && gap<0x80000000ull && scope->pair.left.render_id>prior->scope.pair.left.render_id &&
                    scope->pair.left.render_id-prior->scope.pair.left.render_id==gap &&
                    uint32_t(scope->scene.render_counter_before-prior->scope.scene.render_counter_before)==gap;
                if(renderer!=scope->scene.renderer)self.fail(r,Refusal::scope);
                else if(self.m_base && r.selector_caller!=game_build::rva(0x180a6e1))self.fail(r,Refusal::callsite);
                // A packet refused before any write for a transient camera mismatch (a level
                // transition, 23 Oct) keeps this job native and suspends like a contract refusal.
                else if(packet && packet->recoverable && packet->refusal && !packet->applied && !packet->rollback){if(!self.recover(r,Refusal::packet,true,true))self.fail(r,Refusal::packet);}
                else if(!packet || packet->refusal || !packet->reference || !packet->applied || (!packet->source_unchanged && !(packet->tracked && packet->left_patched)) || packet->renderer!=renderer || packet->render_id!=scope->pair.left.render_id || packet->context.list!=scope->scene.list || packet->context.counter!=scope->scene.render_counter_before)self.fail(r,Refusal::packet);
                else if(!prior){r.bootstrap=true;self.m_creation_dispatch=r.scope.scene.dispatch_sequence;if(owned)self.fail(r,Refusal::creation);}
                else if(!prior->accepted || !prior->sdk || (!packet->continued && !self.m_continuous) || prior->scope.scene.renderer!=renderer ||
                    (!resumed && (suspended || prior->scope.pair.left.render_id+1!=scope->pair.left.render_id || uint32_t(prior->scope.scene.render_counter_before+1)!=scope->scene.render_counter_before || prior->scope.scene.dispatch_sequence+1!=scope->scene.dispatch_sequence))){
                    // A tracked camera cut that cannot chain to the last accepted job (a new
                    // scene: 24 Oct 20:02, the renderer's counters restarted at 11 after a
                    // cutscene) starts a fresh chain instead of stopping for good: a bootstrap
                    // as at startup, native selection, keeping the owned pair. Its acceptance
                    // ends a suspension.
                    const bool cut=packet->tracked && packet->left_patched && packet->patched && !packet->continued;
                    if(self.m_continuous && cut && owned && owned->origin.dispatch==self.m_creation_dispatch){r.bootstrap=r.restart=true;OrdinaryStereoRecovery::increment(self.m_restarts);}
                    // A job admitted before the suspension reached the producer, or a
                    // later non-resumable one: stay native, keep suspended.
                    else if(!suspended || !self.recover(r,Refusal::continuity,!packet->continued))self.fail(r,Refusal::continuity);}
                else if(!owned || owned->origin.dispatch!=self.m_creation_dispatch)self.fail(r,Refusal::creation);
                // The native pool no longer matches the owned textures' creation
                // contract (no owned texture used for this job): suspend owned stereo
                // until a fresh resumed job passes the complete check again.
                else if(!self.contract(r,*owned)){if(r.refusal==Refusal::none && !self.recover(r,Refusal::contract))self.fail(r,Refusal::contract);}
                // Diagnostic only: refuse a valid contract to exercise recovery live.
                else if(self.m_continuous && self.m_inject_period.count() && !suspended && !self.m_recovery.exhausted() && self.m_last_accepted && self.m_now && self.m_now()>=self.m_inject_next){
                    self.m_inject_next=self.m_now()+self.m_inject_period;r.contract_failure=ContractFailure::injected;r.destination=r.resource=r.identity=r.creation_dispatch=0;
                    if(!self.recover(r,Refusal::contract))self.fail(r,Refusal::contract);}
                else {r.previous_dispatch=prior->scope.scene.dispatch_sequence;r.resumed=resumed;r.reset=prior->bootstrap || !packet->continued;r.selected=true;selected=r.destination;}
            }
        }
    }}}catch(...){std::lock_guard lock{self.m_mutex};if(index)self.fail(self.m_records[*index],Refusal::exception);else{self.m_stopped=true;self.m_stop();}}
    if(selected)return selected;
    const auto result=self.m_selector(renderer,even,odd);
    if(index){std::lock_guard lock{self.m_mutex};auto& r=self.m_records[*index];r.destination=result;uintptr_t resource{};if(result && result<=UINTPTR_MAX-0x280 && read(result+0x278,resource))r.resource=resource;}
    return result;
}
uintptr_t NativeOrdinaryBloom::previous(NativeTemporalProbe::Event* event,const std::array<uintptr_t,7>& args){
    if(!installed() || args[6]!=2)return args[3];std::lock_guard lock{m_mutex};
    auto found=std::ranges::find_if(m_records,[&](const auto& r){return r.scope.scene.renderer==args[0] && r.thread==GetCurrentThreadId() && (event?r.scope.scene.dispatch_sequence==event->scope.scene.dispatch_sequence:r.selected && !r.temporal_calls);});
    if(found==m_records.end())return args[3];auto& r=*found;++r.temporal_calls;r.observed_previous=args[3];r.invoked_previous=args[3];
    if(r.selected){if(event)event->controlled=true;r.invoked_previous=r.reset?0:args[3];}
    try{
        const auto scope=current();
        if(r.temporal_calls!=1 || !event || event->refusal!=NativeTemporalProbe::Refusal::none || !scope || !same(r.scope,*scope) || !same(r.scope,event->scope) || args[2]!=r.destination || !event->before || event->before->next!=r.destination || event->before->previous!=args[3])fail(r,Refusal::temporal);
        if(r.selected){
            const auto* prior=predecessor(r.previous_dispatch);
            // A resumed pair samples no previous texture. The native argument
            // remains untouched for native release and is still authenticated
            // by the temporal event/contract above.
            if(!prior || !prior->accepted || (!r.resumed && args[3]!=prior->destination) ||
                (r.resumed && !r.reset) || (!r.reset && (!prior->selected || args[3]==r.destination))){r.invoked_previous=0;fail(r,Refusal::continuity);}
        }
        if(r.refusal!=Refusal::none && r.selected)r.invoked_previous=0;
    }catch(...){fail(r,Refusal::exception);if(r.selected)r.invoked_previous=0;}
    return r.invoked_previous;
}
void NativeOrdinaryBloom::after_temporal(const NativeTemporalProbe::Event& event){
    if(!installed() || event.mode!=2 || event.scope.scene.slot!=1)return;std::lock_guard lock{m_mutex};
    const auto found=std::ranges::find_if(m_records,[&](const auto& r){return r.scope.scene.dispatch_sequence==event.scope.scene.dispatch_sequence;});if(found==m_records.end())return;auto& r=*found;
    try{
        if(r.temporal_calls!=1 || event.refusal!=NativeTemporalProbe::Refusal::none || !event.before || event.before!=event.after || !same(r.scope,event.scope) || event.arguments[2]!=r.destination || event.invoked_arguments[3]!=r.invoked_previous || event.outputs_after[1].resource!=r.resource){fail(r,Refusal::temporal);return;}
        if(r.selected){
            if(!event.controlled || !event.texture_contract_after || !r.contract || !shape(*event.texture_contract_after,*r.contract) || event.invoked_outputs_before[1].resource!=r.resource || event.invoked_outputs_after[1].resource!=r.resource){fail(r,Refusal::temporal);return;}
            const auto owned=m_owned();if(!owned || owned->row>=r.contract->targets[0].initialization_words.size()){fail(r,Refusal::creation);return;}
            const auto& row=r.contract->targets[0].initialization_words[owned->row];
            for(const auto* output:{&event.invoked_outputs_before[1],&event.invoked_outputs_after[1]}){
                if(output->status!=NativeRenderProbe::OutputStatus::observed || output->descriptors.views.empty()){fail(r,Refusal::temporal);return;}
                for(const auto& view:output->descriptors.views)if(!view.known || view.identity!=r.identity || view.resource!=r.resource || view.width!=row[0] || view.height!=row[1] || view.storage!=DXGI_FORMAT_R16G16B16A16_TYPELESS || view.format!=DXGI_FORMAT_R16G16B16A16_FLOAT || view.flags!=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET || view.subresources!=1){fail(r,Refusal::temporal);return;}
            }
            if(r.reset && (r.invoked_previous || event.invoked_outputs_after[2].resource)){fail(r,Refusal::temporal);return;}
        }
        r.temporal=true;
    }catch(...){fail(r,Refusal::exception);}
}
void NativeOrdinaryBloom::on_colour(uintptr_t a,uintptr_t b,float c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h,const void* i){
    PerformanceTrace::Scope timing{"ordinary_colour_total_cpu"};
    auto& self=get();const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    struct ColourScope{std::optional<uint64_t> previous{s_colour};ColourScope(){s_colour.reset();}~ColourScope(){s_colour=previous;}} colour_scope;
    try{if(self.installed())if(const auto scope=self.current()){
        std::lock_guard lock{self.m_mutex};const auto found=std::ranges::find_if(self.m_records,[&](const auto& r){return r.scope.scene.dispatch_sequence==scope->scene.dispatch_sequence;});
        if(found!=self.m_records.end()){auto& r=*found;++r.colour_calls;r.colour_input=g;r.colour_caller=self.m_base?caller-self.m_base:caller;uintptr_t next{};
            if(r.colour_calls!=1 || !same(r.scope,*scope) || d!=scope->scene.entry || g!=r.destination || !read(scope->scene.renderer+0x1b8,next) || next!=g || !r.temporal || (self.m_base && (a!=self.m_base+game_build::rva(0x4faf840) || r.colour_caller!=game_build::rva(0x180ae76))))self.fail(r,Refusal::colour);
            else {r.colour=true;s_colour=scope->scene.dispatch_sequence;}
        }
    }}catch(...){std::lock_guard lock{self.m_mutex};self.m_stopped=true;self.m_stop();}
    try{if(self.colour_input)if(const auto scope=self.m_provider())if(!ordinary_scene_scope::inspect(*scope).mask && d==scope->scene.entry)self.colour_input(*scope,f);}catch(...){}
    {PerformanceTrace::Scope native_timing{"ordinary_colour_native_cpu"};self.m_colour(a,b,c,d,e,f,g,h,i);}
}
void NativeOrdinaryBloom::on_tone(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uint32_t f,int32_t g,const void* h,uintptr_t i){
    auto& self=get();try{if(s_colour){std::lock_guard lock{self.m_mutex};const auto found=std::ranges::find_if(self.m_records,[&](const auto& r){return r.scope.scene.dispatch_sequence==*s_colour;});
        if(found!=self.m_records.end()){auto& r=*found;++r.tone_calls;r.tone_input=d;const auto scope=self.current();
            if(r.tone_calls!=1 || !scope || !same(r.scope,*scope) || !r.colour || d!=r.destination || (self.m_base && a!=self.m_base+game_build::rva(0x4f9d010)))self.fail(r,Refusal::colour);else r.tone=true;}}
    }catch(...){std::lock_guard lock{self.m_mutex};self.m_stopped=true;self.m_stop();}
    std::optional<NativeOrdinaryGrading::Input> input;
    if(self.m_base && a==self.m_base+game_build::rva(0x4f9d010))if(const auto scope=self.m_provider())input=NativeOrdinaryGrading::read_input(*scope,a,g,h,i);
    auto& grading=NativeOrdinaryGrading::get();const auto fixed=grading.descriptor(input);
    OrdinaryToneSource::Inputs inputs{c,d,e};
    if(input)try{inputs=self.tone_source(input->scope.scene.dispatch_sequence,input->scope.scene.slot,inputs);}catch(...){}
    if(input && self.tone_inputs)try{self.tone_inputs(input->scope,inputs);}catch(...){}
    grading.invoke(input,[&]{self.m_tone(a,b,inputs[0],inputs[1],inputs[2],f,g,fixed?fixed->data():h,i);});
    // a is the HDR core: the right eye's view index 1 takes index 0's eye adaptation (NativeAdaptationMirror.h).
    if(input)try{NativeAdaptationMirror::get().mirror(a);}catch(...){}
    if(input && self.tone_output)try{self.tone_output(input->scope,b);}catch(...){}
}
void NativeOrdinaryBloom::complete(const NativeRenderProbe::Snapshot& snapshot){
    if(!installed())return;std::lock_guard lock{m_mutex};const auto found=std::ranges::find_if(m_records,[&](const auto& r){return r.scope.scene.dispatch_sequence==snapshot.sequence;});if(found==m_records.end())return;auto& r=*found;
    try{
        const auto packet=m_packet(snapshot.sequence);
        // Suspended on a recoverable packet refusal: the packet wrote nothing and was never accepted.
        if(r.suspended && r.refusal==Refusal::packet && packet && packet->recoverable && !packet->applied)return;
        if(!packet || packet->refusal || !packet->accepted || !packet->sdk || snapshot.scene_calls!=2 || snapshot.scene_refused || snapshot.scene_dropped || snapshot.scenes.size()!=2 || !snapshot.ordinary_pair || !NativeCameraEntries::get().current(*snapshot.ordinary_pair)){fail(r,Refusal::packet);return;}
        r.sdk=packet->sdk;
        // Suspended: native selection ran; never accepted, never a predecessor.
        if(r.suspended)return;
        if(r.previous_dispatch){const auto* prior=predecessor(r.previous_dispatch);
            const auto step=r.resumed?r.scope.scene.dispatch_sequence-r.previous_dispatch:1;
            if(!prior || !prior->accepted || !prior->sdk || r.sdk->epoch!=prior->sdk->epoch ||
                r.sdk->ordinal<=prior->sdk->ordinal || r.sdk->ordinal-prior->sdk->ordinal!=step || uint32_t(r.sdk->token-prior->sdk->token)!=step){fail(r,Refusal::continuity);return;}}
        if(r.refusal!=Refusal::none || !r.temporal || !r.colour || !r.tone || r.temporal_calls!=1 || r.colour_calls!=1 || r.tone_calls!=1){fail(r,Refusal::colour);return;}
        for(size_t slot=0;slot<2;++slot){const auto& s=snapshot.scenes[slot];
            if(!s.valid() || s.slot!=slot || s.renderer!=r.scope.scene.renderer || s.list!=r.scope.scene.list || s.render_counter_before!=r.scope.scene.render_counter_before || !s.temporal_before || !s.temporal_after || !s.persistent_before || !s.persistent_after || s.persistent_before->address!=s.renderer+0x200+slot*0x58 || s.persistent_after->address!=s.persistent_before->address || s.persistent_before->words!=s.temporal_before->previous || s.persistent_after->words!=s.temporal_after->next){fail(r,Refusal::export_bank);return;}
        }
        const auto& right=snapshot.scenes[1];
        if(right.temporal_after->next[2]!=r.destination || right.persistent_after->words[2]!=r.destination || (r.selected && snapshot.scenes[0].persistent_after->words[2]==r.destination)){fail(r,Refusal::export_bank);return;}
        r.accepted=true;
        // A renewal's fresh bootstrap (new owned pair) ends the suspension like a resumed job.
        if(m_suspended && (r.resumed || (r.bootstrap && (m_renewing || r.restart)))){m_suspended=false;m_renewing=false;r.recovered=true;m_recovery.recovered();}
        m_last_accepted=r;
    }catch(...){fail(r,Refusal::exception);}
}
std::optional<NativeOrdinaryBloom::Record> NativeOrdinaryBloom::record(uint64_t dispatch)const{std::lock_guard lock{m_mutex};for(const auto& r:m_records)if(r.scope.scene.dispatch_sequence==dispatch)return r;return {};}
void NativeOrdinaryBloom::write(std::ostream& out,uint64_t dispatch)const{
    const auto r=record(dispatch);out<<"{\"model\":\"ordinary_owned_bloom_use_v1\",\"installed\":"<<(installed()?"true":"false")<<",\"instruction_guards\":"<<(m_guards?"true":"false")<<",\"dispatch\":"<<dispatch<<",\"native_gpu_histories_verified\":false,\"gpu_pair_verified\":false,\"runtime_submission\":false,\"retirement\":\"process_lifetime\",\"record\":";
    if(!r){out<<"null}";return;}const auto& s=r->scope.scene;const auto& p=r->scope.pair.left;
    const auto boolean=[&](const char* name,bool value){out<<",\""<<name<<"\":"<<(value?"true":"false");};
    out<<"{\"render_id\":"<<p.render_id<<",\"revision\":"<<p.revision<<",\"token\":"<<p.token<<",\"renderer\":"<<s.renderer<<",\"list\":"<<s.list<<",\"entry\":"<<s.entry<<",\"slot\":"<<s.slot<<",\"scene_ordinal\":"<<s.ordinal<<",\"counter\":"<<s.render_counter_before<<",\"thread\":"<<r->thread<<",\"refusal\":"<<uint32_t(r->refusal)<<",\"selector_caller\":"<<r->selector_caller<<",\"selector_arguments\":["<<r->selector_arguments[0]<<','<<r->selector_arguments[1]<<','<<r->selector_arguments[2]<<']';
    boolean("bootstrap",r->bootstrap);boolean("selected",r->selected);boolean("reset",r->reset);boolean("resumed",r->resumed);boolean("temporal",r->temporal);boolean("colour",r->colour);boolean("tone",r->tone);boolean("accepted",r->accepted);boolean("suspended",r->suspended);boolean("recovered",r->recovered);boolean("restart",r->restart);
    out<<",\"contract_failure\":\""<<name(r->contract_failure)<<"\",\"contract_difference\":"<<r->contract_difference;
    out<<",\"scope_evidence\":{\"model\":\"ordinary_scene_view_inventory_v1\",\"mask\":"<<r->scope_evidence.mask<<",\"reads\":"<<r->scope_evidence.reads<<",\"counter\":"<<r->scope_evidence.counter<<",\"count\":"<<r->scope_evidence.count<<",\"active\":"<<r->scope_evidence.active<<",\"flags\":"<<r->scope_evidence.flags<<",\"view_flags\":[";for(size_t i=0;i<r->scope_evidence.view_flags.size();++i){if(i)out<<',';out<<r->scope_evidence.view_flags[i];}out<<"]}";
    out<<",\"destination\":"<<r->destination<<",\"resource\":"<<r->resource<<",\"identity\":"<<r->identity<<",\"observed_previous\":"<<r->observed_previous<<",\"invoked_previous\":"<<r->invoked_previous<<",\"colour_input\":"<<r->colour_input<<",\"tone_input\":"<<r->tone_input<<",\"colour_caller\":"<<r->colour_caller<<",\"temporal_calls\":"<<r->temporal_calls<<",\"colour_calls\":"<<r->colour_calls<<",\"tone_calls\":"<<r->tone_calls<<",\"previous_dispatch\":"<<r->previous_dispatch<<",\"creation_dispatch\":"<<r->creation_dispatch;
    out<<",\"sdk\":";if(r->sdk)out<<"{\"epoch\":"<<r->sdk->epoch<<",\"ordinal\":"<<r->sdk->ordinal<<",\"token\":"<<r->sdk->token<<'}';else out<<"null";
    out<<",\"contract\":";if(r->contract)native_texture_contract::write(out,*r->contract);else out<<"null";
    out<<",\"source_membership\":";native_pool_membership::write(out,r->source_membership);out<<",\"owned_membership\":[";
    for(size_t i=0;i<2;++i){if(i)out<<',';native_pool_membership::write(out,r->owned_membership[i]);}out<<"],\"grading\":";NativeOrdinaryGrading::get().write(out,dispatch);out<<",\"late_effect\":";NativeOrdinaryLateEffect::get().write(out,dispatch);out<<",\"sharpen\":";NativeOrdinarySharpen::get().write(out,dispatch);out<<"}}";
}
OrdinaryToneSource::Inputs NativeOrdinaryBloom::tone_source(uint64_t dispatch,uint32_t slot,OrdinaryToneSource::Inputs inputs){
    std::lock_guard lock{m_tone_mutex};if(m_tone_control.empty())return inputs;
    if(const auto now=std::chrono::steady_clock::now();now>=m_tone_poll){m_tone_poll=now+std::chrono::seconds(1);
        std::ifstream in{m_tone_control};std::stringstream text;if(in){text<<in.rdbuf();if(const auto mask=OrdinaryToneSource::parse(text.str()))m_tone_source.mask(*mask);}}
    return m_tone_source.apply(dispatch,slot,inputs);
}
OrdinaryToneSource::Stats ordinary_tone_source_stats(){return NativeOrdinaryBloom::get().tone_source_stats();}
std::string ordinary_adaptation_mirror_line(){auto& m=NativeAdaptationMirror::get();return m.configured()?m.line():std::string{};}
std::string ordinary_adaptation_share_line(){auto& g=NativeOrdinaryGrading::get();return g.installed()?g.adaptation_line():std::string{};}
std::string ordinary_flare_probe_line(){auto& probe=NativeFlareProbe::get();return probe.installed()?probe.line():std::string{};}
