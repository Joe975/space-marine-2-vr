#pragma once
#include "OrdinaryTrackedCamera.h"
#include "OrdinaryCameraCulling.h"
#include "EngineProjectionHook.h"
#include "NativeCameraEntries.h"
#include "OwnedStereoReadiness.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <stdexcept>

// Tracked native-add adapter. Inert until explicitly armed; continuous use
// requires an explicit arm option. Native histories/packets/GPU consumers need their
// own acceptance. Does not start a runtime or lease native resource pointers.
class NativeTrackedOrdinaryCamera {
public:
    static constexpr size_t ADD_PROJECTION_OFFSET=0x45c;
    using Prepared=ordinary_tracked_camera::Pair;
    using Source=ordinary_tracked_camera::Source;
    using Args=std::array<uint64_t,9>;
    using Add=std::function<int64_t(void*,void*,const Args&)>;
    using Provider=std::function<std::optional<Prepared>(std::span<const uint8_t>,const FrameSyncClock::Frame&)>;
    using Renderer=std::function<uintptr_t()>;
    using Current=std::function<std::optional<FrameSyncClock::Frame>()>;
    using RetryMissing=std::function<bool()>;
    using Now=std::function<std::chrono::steady_clock::time_point()>;
    using Observer=std::function<void(const ordinary_tracked_camera::Eye&,int64_t,bool)>;
    using Readiness=std::function<OwnedStereoReadiness::Observation()>;
    enum class Refusal:uint32_t {none,source,job,provider,bank,ticket,native_slot,projection,changed,exception,stopped,culling,initializing,suspended,foreign_view,no_dlss};
    struct Receipt {
        FrameSyncClock::Frame job;uintptr_t list{},renderer{},source{};uint32_t thread{};
        std::array<int64_t,2> results{};std::array<uint64_t,2> flags{};
        std::array<bool,2> applied{};unsigned native_calls{};
        bool accepted{},source_unchanged{},aborted{};Refusal refusal{};
        // Pure provider expiry before any owned eye was added, at any stream age.
        bool provider_returned{},expired_pre_add_retry{};double provider_ms{},tracking_age_ms{};
        std::optional<NativeCameraEntries::OrdinaryPair> pair;
        std::optional<Prepared> prepared;
    };
    static NativeTrackedOrdinaryCamera& get(){static auto* value=new NativeTrackedOrdinaryCamera;return *value;}
    ~NativeTrackedOrdinaryCamera(){request_stop();}
    bool arm(Renderer renderer,Current current,Provider provider,unsigned jobs=4,bool continuous=false,RetryMissing retry_missing={},Now now={},Readiness readiness={}){
        std::lock_guard lock{m_arm_mutex};
        if(m_armed || !renderer || !current || !provider || !jobs || jobs>4 || EngineProjectionHook::get().state()!=EngineProjectionHook::State::installed)return false;
        m_renderer=std::move(renderer);m_current=std::move(current);m_provider=std::move(provider);m_retry_missing=std::move(retry_missing);m_limit=jobs;m_continuous=continuous;
        m_now=now?std::move(now):Now{[]{return std::chrono::steady_clock::now();}};
        m_readiness=std::move(readiness);
        NativeCameraEntries::get().enable();m_armed.store(true,std::memory_order_release);return true;
    }
    bool armed()const{return m_armed.load(std::memory_order_acquire);}
    // Explicit diagnostic: camera-id byte for the added slot-1 camera (-1 = inherit native).
    void set_second_camera_id(int id){m_second_camera_id.store(id>=0&&id<=255?id:-1,std::memory_order_relaxed);}
    int second_camera_id()const{return m_second_camera_id.load(std::memory_order_relaxed);}
    int native_camera_id_arg()const{return m_native_id_arg.load(std::memory_order_relaxed);}
    bool stopped()const{return m_stopped.load(std::memory_order_acquire);}
    void warmup(unsigned jobs){m_warmup=jobs;}
    // Stereo onset diagnostic (SM2VR_STEREO_WARMUP_JOBS): eligible ordinary jobs before the first
    // second view. Default and minimum 241; at most 100000. Anything unparsable keeps the default.
    static constexpr unsigned DEFAULT_WARMUP=241,MAX_WARMUP=100000;
    static unsigned warmup_from(const char* value){
        if(!value || !*value)return DEFAULT_WARMUP;unsigned long long n=0;
        for(const char* c=value;*c;++c){if(*c<'0' || *c>'9')return DEFAULT_WARMUP;n=n*10+unsigned(*c-'0');if(n>MAX_WARMUP)return MAX_WARMUP;}
        return n<DEFAULT_WARMUP?DEFAULT_WARMUP:unsigned(n);}
    void request_stop()noexcept{m_stopped.store(true,std::memory_order_release);reset(m_live_list.load(std::memory_order_acquire));}
    // Lifts a stop requested by the consumer for a recoverable reason (a swap-chain reset: the
    // producer's inputs are untouched, only the GPU-side consumer had to drop its leases). An
    // armed producer starts over as at first arming: the warmup and the owned readiness
    // settling apply again, the renderer owner and render-id ordering are kept, and a pending
    // suspension or hold is cleared. Refuses while a native job is in flight (retry later) and
    // on a producer that was never armed. Stops caused by the producer's own refusals are
    // lifted too: the next job is fully re-checked, so nothing is trusted from before. The busy
    // flag is only read, never taken: a job that starts meanwhile must not see it held and stop.
    bool restart()noexcept{
        if(!armed())return false;
        if(m_busy.test(std::memory_order_acquire))return false;
        m_eligible=0;m_warmup_last=0;
        if(m_readiness){std::lock_guard lock{m_readiness_mutex};m_readiness_state.gap();}
        m_suspended.store(false,std::memory_order_release);m_hold.store(false,std::memory_order_release);
        m_stopped.store(false,std::memory_order_release);return true;
    }
    // Recoverable consumer refusal (continuous mode only; otherwise a stop). Jobs
    // already added finish normally; later jobs stay native mono, without any
    // tracking read, until the resume gate admits one. Then the usual readiness
    // settling and every pair check apply again. A stop always wins.
    void request_suspend()noexcept{
        if(!m_continuous){request_stop();return;}
        if(!m_suspended.exchange(true,std::memory_order_acq_rel))m_suspensions.fetch_add(1,std::memory_order_relaxed);
    }
    void set_resume_gate(std::function<bool()> gate){std::lock_guard lock{m_gate_mutex};m_resume_gate=std::move(gate);}
    // A game PIP view is showing (ForeignViewHold): add no pair, whatever path would resume
    // stereo (a suspension's gate, a camera cut, a renewal, a cold restart, readiness), since a
    // PIP behind the pair gets an HDR index past the game's table (docs/LOAD_CRASH_2026-10-26.md).
    // Native mono continues; nothing stops or suspends, and readiness settles again afterwards.
    void set_foreign_gate(std::function<bool()> held){std::lock_guard lock{m_gate_mutex};m_foreign_gate=std::move(held);}
    uint64_t foreign_jobs()const{return m_foreign_jobs.load(std::memory_order_relaxed);}
    // The game is running neither DLSS nor TAA (OrdinaryDlssPresence): no pair could be accepted, and each one
    // tried stops the stack. The same hold as a PIP: native mono, nothing stops or suspends.
    void set_dlss_gate(std::function<bool()> held){std::lock_guard lock{m_gate_mutex};m_dlss_gate=std::move(held);}
    uint64_t dlss_jobs()const{return m_dlss_jobs.load(std::memory_order_relaxed);}
    // The list the last pair was added to (0 before the first): a PIP added there after the pair sits behind it.
    uintptr_t live_list()const{return m_live_list.load(std::memory_order_acquire);}
    // Profiling A/B (StereoHoldControl): hold native mono through the suspension path above, so the
    // headset shows the flat panel and no held job is shown. Release lets the resume gate admit the
    // next job with every pair check. Continuous mode only; bounded experiments ignore it.
    void hold(bool value)noexcept{if(!m_continuous)return;m_hold.store(value,std::memory_order_release);if(value)request_suspend();}
    bool held()const{return m_hold.load(std::memory_order_acquire);}
    bool suspended()const{return m_suspended.load(std::memory_order_acquire);}
    struct Suspension {uint64_t suspensions{},suspended_jobs{},resumes{};};
    Suspension suspension()const{return {m_suspensions.load(),m_suspended_jobs.load(),m_resumes.load()};}
    std::vector<Receipt> receipts()const{std::lock_guard lock{m_mutex};return {m_receipts.begin(),m_receipts.begin()+m_count};}
    std::optional<Receipt> receipt(uint64_t render_id)const{std::lock_guard lock{m_mutex};for(size_t i=0;i<m_count;++i)if(m_receipts[i].job.render_id==render_id)return m_receipts[i];return {};}
    uint64_t dropped()const{return m_dropped.load();}
    uint64_t clock_skips()const{return m_clock_skips.load();}
    OwnedStereoReadiness::Snapshot readiness()const{std::lock_guard lock{m_readiness_mutex};return m_readiness_state.snapshot();}
    // How long after its last ready job an unchanged owner skips the three-job settle (OwnedStereoReadiness::resume).
    void readiness_resume(std::chrono::milliseconds value){std::lock_guard lock{m_readiness_mutex};m_readiness_state.resume(value);}
    std::chrono::milliseconds readiness_resume()const{std::lock_guard lock{m_readiness_mutex};return m_readiness_state.resume();}
    int64_t on_main(void* list,void* source,const FrameSyncClock::Frame* job,const Args& args,const Add& add,const Observer& observer={}){
        if(!add)throw std::invalid_argument("native add callback required");
        const auto native_list=reinterpret_cast<uintptr_t>(list);
        const auto fallback=[&]{reset(native_list);EngineProjectionHook::Scope scope{std::nullopt};return add(list,source,args);};
        if(!armed())return fallback();
        if(m_busy.test_and_set(std::memory_order_acquire)){request_stop();return fallback();}
        struct Busy{std::atomic_flag& value;~Busy(){value.clear(std::memory_order_release);}} busy{m_busy};
        if(m_stopped || (!m_continuous && m_completed>=m_limit))return fallback();
        if(m_eligible<m_warmup){Source value{};float fov{};int32_t count{};
            const bool eligible=source && job && !job->invalid && job->render_id && !(args[0]&0x50000044) && read(reinterpret_cast<uintptr_t>(source),value) && read_count(native_list,count) && count==0;
            if(eligible)std::memcpy(&fov,value.data()+0x2d0,4);
            if(!eligible || !std::isfinite(fov) || fov<40 || fov>150){m_eligible=0;return fallback();}
            if(job->render_id!=m_warmup_last){m_warmup_last=job->render_id;++m_eligible;}return fallback();}
        m_live_list.store(native_list,std::memory_order_release);
        Receipt r;r.list=native_list;r.source=reinterpret_cast<uintptr_t>(source);r.thread=GetCurrentThreadId();if(job)r.job=*job;
        const auto refuse=[&](Refusal reason){r.refusal=reason;request_stop();reset(native_list);store(r);};
        Source original{};int32_t count{};
        if(!source || !native_list || !read(r.source,original) || !read_count(native_list,count) || count!=0 || (args[0]&0x50000044)){
            refuse(Refusal::source);return fallback();}
        // The passive marker clock can have zero or multiple open intervals.
        // No stereo work has begun: discard this camera's provenance and let
        // native mono rendering proceed. A later independently verified job
        // may resume; never borrow a previous job or relax the pair checks.
        if(!job && m_continuous){
            // Says nothing about the player: an owner proven just before resumes at once
            // (OwnedStereoReadiness resume window, docs/STEREO_PAUSES_2026-10-26.md).
            if(m_readiness){std::lock_guard lock{m_readiness_mutex};m_readiness_state.gap(true);}
            ++m_clock_skips;r.refusal=Refusal::job;reset(native_list);store(r);return fallback();
        }
        if(!job || job->invalid || !job->render_id || job->render_id<=m_last_render || !current(*job)){
            refuse(Refusal::job);return fallback();}
        m_last_render=job->render_id;
        if(foreign_held()){
            if(m_readiness){std::lock_guard lock{m_readiness_mutex};m_readiness_state.gap();}
            m_foreign_jobs.fetch_add(1,std::memory_order_relaxed);r.refusal=Refusal::foreign_view;reset(native_list);store(r);return fallback();}
        if(dlss_held()){
            if(m_readiness){std::lock_guard lock{m_readiness_mutex};m_readiness_state.gap();}
            m_dlss_jobs.fetch_add(1,std::memory_order_relaxed);r.refusal=Refusal::no_dlss;reset(native_list);store(r);return fallback();}
        if(m_suspended.load(std::memory_order_acquire)){
            if(!resume_allowed()){
                if(m_readiness){std::lock_guard lock{m_readiness_mutex};m_readiness_state.gap();}
                m_suspended_jobs.fetch_add(1,std::memory_order_relaxed);r.refusal=Refusal::suspended;reset(native_list);store(r);return fallback();}
            m_suspended.store(false,std::memory_order_release);m_resumes.fetch_add(1,std::memory_order_relaxed);
        }
        // Native mono remains available throughout loading. No VR acquisition,
        // eye preparation or owned add occurs until actual native ownership exists.
        if(!owned_ready(job->render_id)){r.refusal=Refusal::initializing;reset(native_list);store(r);return fallback();}
        std::optional<Prepared> prepared;
        const auto provider_start=std::chrono::steady_clock::now();
        try{r.renderer=m_renderer();prepared=m_provider(original,*job);}catch(...){refuse(Refusal::provider);return fallback();}
        r.provider_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-provider_start).count();r.provider_returned=prepared.has_value();
        if(prepared)r.tracking_age_ms=std::chrono::duration<double,std::milli>(m_now()-prepared->eyes[0].tracking.captured).count();
        // A hitch may age an otherwise valid provider result before any
        // stereo add. Discard that job, keeping the exact 100ms freshness rule.
        // Rebuild/validate at its original capture time to prove age is the ONLY
        // failure. Future/mismatched/corrupt data and any half-added pair retain
        // terminal refusal behavior. This is safe after startup too: no owned
        // eye from this job exists and the next job reacquires fresh tracking.
        const auto retry_provider_age=[&]{
            if(!prepared || !m_continuous || r.native_calls || m_stopped || !current(*job) || !r.renderer ||
               (m_owner && m_owner!=r.renderer) || !same_renderer(r.renderer) || !read_count(native_list,count) || count!=0)return false;
            const auto captured=prepared->eyes[0].tracking.captured,now=m_now();Source unchanged{};
            if(captured!=prepared->eyes[1].tracking.captured || now<captured || now-captured<=std::chrono::milliseconds(100) ||
               !ordinary_tracked_camera::matches(*prepared,original,*job,captured) || !read(r.source,unchanged) || unchanged!=original)return false;
            r.refusal=Refusal::provider;r.expired_pre_add_retry=true;reset(native_list);store(r);return true;
        };
        // No eye was added for this job. A specifically identified tracking
        // absence can recover on a new job even after an earlier stereo stream.
        // Corrupt prepared data, exceptions and changed renderer/job still stop.
        if(!prepared && m_continuous && (!m_completed || retry_missing()) && current(*job) && r.renderer && (!m_owner || m_owner==r.renderer) && same_renderer(r.renderer)){
            r.refusal=Refusal::provider;reset(native_list);store(r);return fallback();
        }
        if(!prepared || !ordinary_tracked_camera::matches(*prepared,original,*job,m_now()) || !current(*job)){
            if(retry_provider_age())return fallback();
            refuse(Refusal::provider);return fallback();}
        // Acquisition can hitch. Recheck current ownership before the first add;
        // the same job cannot advance the three-job readiness requirement.
        if(!owned_ready(job->render_id)){r.refusal=Refusal::initializing;reset(native_list);store(r);return fallback();}
        std::array<uint64_t,11> bank{};
        if(!r.renderer || r.renderer>UINTPTR_MAX-0x258 || !read(r.renderer+0x258,bank) ||
           (!m_owner && std::ranges::any_of(bank,[](auto value){return value!=0;})) || (m_owner && m_owner!=r.renderer)){
            refuse(Refusal::bank);return fallback();}
        std::array<NativeCameraEntries::Entry,2> actual{};
        std::array<Source,2> owned_sources{prepared->eyes[0].source,prepared->eyes[1].source};
        for(size_t slot=0;slot<2;++slot){
            const auto refuse_after_left=[&](Refusal reason){refuse(reason);return r.results[0];};
            if(m_stopped || !current(*job) || !same_renderer(r.renderer) || !read_count(native_list,count) || count!=int32_t(slot) ||
               !ordinary_tracked_camera::matches(*prepared,original,*job,m_now())){
                if(!slot && retry_provider_age())return fallback();
                if(slot)return refuse_after_left(Refusal::changed);refuse(Refusal::changed);return fallback();}
            std::optional<NativeCameraEntries::Ticket> ticket;
            try{ticket=NativeCameraEntries::get().begin(native_list,slot?NativeCameraEntries::Role::ordinary_right:NativeCameraEntries::Role::main,*job);}catch(...){}
            if(!ticket){if(slot)return refuse_after_left(Refusal::ticket);refuse(Refusal::ticket);return fallback();}
            auto& owned=owned_sources[slot];
            EngineProjectionHook::Scope scope{EngineProjectionHook::Binding{*prepared->eyes[slot].tracking.fov,int(slot),job->render_id}};
            ++r.native_calls;
            // Diagnostic only: native add stores (byte)args[1] as the entry's camera id
            // (+0x24); both slots otherwise inherit slot 0's id.
            auto slot_args=args;m_native_id_arg.store(uint8_t(args[1]),std::memory_order_relaxed);
            if(const auto id=m_second_camera_id.load(std::memory_order_relaxed);slot && id>=0)slot_args[1]=(args[1]&~uint64_t{0xff})|uint64_t(id);
            try{r.results[slot]=add(list,owned.data(),slot_args);}catch(...){
                finish(*ticket,-1,std::nullopt);r.aborted=true;refuse(Refusal::exception);throw;
            }
            r.applied[slot]=scope.applied();
            const auto address=NativeCameraEntries::address(native_list,int32_t(slot));
            vr_camera::Matrix world{},projection{};
            const bool observed=int32_t(r.results[slot])==int32_t(slot) && read_count(native_list,count) && count==int32_t(slot+1) && address &&
                read(*address,r.flags[slot]) && !(r.flags[slot]&0x50000044) && *address<=UINTPTR_MAX-ADD_PROJECTION_OFFSET &&
                read(*address+0x34,world) && world==prepared->eyes[slot].world && read(*address+ADD_PROJECTION_OFFSET,projection);
            const bool finished=finish(*ticket,observed?int32_t(slot):-1,observed?std::optional{world}:std::nullopt);
            if(!observed || !finished || (slot && r.flags[slot]!=r.flags[0]))return refuse_after_left(Refusal::native_slot);
            if(!scope.applied() || scope.output()!=std::optional{prepared->eyes[slot].projection} || projection!=prepared->eyes[slot].projection)
                return refuse_after_left(Refusal::projection);
            if(!repair_culling(*address,prepared->eyes[slot]))return refuse_after_left(Refusal::culling);
            Source after{};
            r.source_unchanged=read(r.source,after) && after==original;
            if(m_stopped || owned!=prepared->eyes[slot].source || !r.source_unchanged || !current(*job) || !same_renderer(r.renderer))return refuse_after_left(Refusal::changed);
            actual[slot]={*address,int32_t(slot),NativeCameraEntries::Role::main,world};
            if(observer)try{observer(prepared->eyes[slot],r.results[slot],scope.applied());}catch(...){++m_dropped;return refuse_after_left(Refusal::changed);}
        }
        try{const auto revision=NativeCameraEntries::get().revision(native_list-0x160);
            if(revision)r.pair=NativeCameraEntries::get().ordinary_pair(native_list-0x160,actual,*revision);
            r.accepted=!m_stopped && r.pair && NativeCameraEntries::get().current(*r.pair) && current(*job) && same_renderer(r.renderer);
        }catch(...){}
        if(!r.accepted){refuse(Refusal::ticket);return r.results[0];}
        r.prepared=*prepared;m_owner=r.renderer;++m_completed;store(r);return r.results[0];
    }
private:
    std::atomic<int> m_second_camera_id{-1},m_native_id_arg{-1};
    static bool repair_culling(uintptr_t entry,const ordinary_tracked_camera::Eye& eye){
        if(entry>UINTPTR_MAX-0x34-sizeof(Source))return false;
        const auto camera=entry+0x34;Source before{},again{};
        if(!read(camera,before) || !read(camera,again) || before!=again)return false;
        const auto corrected=ordinary_camera_culling::correct(before,eye);if(!corrected)return false;
        SIZE_T n{};const auto write=[&](const Source& value){return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(camera),value.data(),value.size(),&n) && n==value.size();};
        if(!write(*corrected) || !read(camera,again) || again!=*corrected){write(before);return false;}
        return true;
    }
    template<class T>static bool read(uintptr_t p,T& out){SIZE_T n{};return p && p<=UINTPTR_MAX-sizeof(T) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),&out,sizeof(out),&n) && n==sizeof(out);}
    static bool read_count(uintptr_t list,int32_t& count){return list && list<=UINTPTR_MAX-0x2980c && read(list+0x29808,count);}
    static void reset(uintptr_t list)noexcept{try{NativeCameraEntries::get().reset(list);}catch(...) {}}
    static bool finish(const NativeCameraEntries::Ticket& ticket,int32_t slot,std::optional<vr_camera::Matrix> world)noexcept{
        try{NativeCameraEntries::get().finish(ticket,slot,world);return true;}catch(...){reset(ticket.list);return false;}}
    bool current(const FrameSyncClock::Frame& job)const noexcept{try{const auto value=m_current();return value && !value->invalid && value->render_id==job.render_id && value->token==job.token;}catch(...){return false;}}
    bool same_renderer(uintptr_t renderer)const noexcept{try{return m_renderer()==renderer;}catch(...){return false;}}
    bool foreign_held()noexcept{try{std::lock_guard lock{m_gate_mutex};return m_foreign_gate && m_foreign_gate();}catch(...){return true;}}
    bool dlss_held()noexcept{try{std::lock_guard lock{m_gate_mutex};return m_dlss_gate && m_dlss_gate();}catch(...){return true;}}
    bool resume_allowed()noexcept{if(m_hold.load(std::memory_order_acquire))return false;try{std::lock_guard lock{m_gate_mutex};return m_resume_gate && m_resume_gate();}catch(...){return false;}}
    bool retry_missing()const noexcept{try{return m_retry_missing && m_retry_missing();}catch(...){return false;}}
    bool owned_ready(uint64_t job){
        if(!m_readiness)return true;
        OwnedStereoReadiness::Observation observation;
        try{observation=m_readiness();}catch(...){}
        // Publication can advance while a source lock is acquired. Assess time
        // after reading the sources, never against a timestamp predating them.
        const auto now=m_now();
        std::lock_guard lock{m_readiness_mutex};return m_readiness_state.observe(job,observation,now);
    }
    void store(const Receipt& receipt)noexcept{try{std::lock_guard lock{m_mutex};if(m_count<m_receipts.size())m_receipts[m_count++]=receipt;else if(m_continuous){std::move(m_receipts.begin()+1,m_receipts.end(),m_receipts.begin());m_receipts.back()=receipt;}else{++m_dropped;request_stop();}}catch(...){++m_dropped;request_stop();}}
    Renderer m_renderer;Current m_current;Provider m_provider;RetryMissing m_retry_missing;Now m_now;unsigned m_limit{},m_completed{},m_count{};
    Readiness m_readiness;OwnedStereoReadiness m_readiness_state;mutable std::mutex m_readiness_mutex;
    bool m_continuous{};
    unsigned m_warmup{},m_eligible{};uint64_t m_warmup_last{};
    uintptr_t m_owner{};uint64_t m_last_render{};std::array<Receipt,4> m_receipts{};
    std::function<bool()> m_resume_gate,m_foreign_gate,m_dlss_gate;std::mutex m_gate_mutex;std::atomic_uint64_t m_foreign_jobs{},m_dlss_jobs{};std::atomic_bool m_suspended{},m_hold{};std::atomic_uint64_t m_suspensions{},m_suspended_jobs{},m_resumes{};
    std::atomic_bool m_armed{},m_stopped{};std::atomic_flag m_busy=ATOMIC_FLAG_INIT;std::atomic_uint64_t m_dropped{},m_clock_skips{};std::atomic_uintptr_t m_live_list{};
    mutable std::mutex m_mutex;std::mutex m_arm_mutex;
};
