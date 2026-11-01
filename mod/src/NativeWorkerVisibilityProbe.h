#pragma once
#include "NativeGeometryIdentity.h"
#include "GameBuild.h"
#include "NativeWorkerMaskCausalProbe.h"
#include <MinHook.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <intrin.h>

// Optional passive observation BEFORE per-camera descriptor generation. Worker
// index/Scene TLS are never used as eye/frame identity. A producer pair observed
// before+after the native call must join forward to a later owned Scene pair.
class NativeWorkerVisibilityProbe {
public:
    using Native=void(*)(uintptr_t,int32_t,uintptr_t,uintptr_t,uintptr_t);
    using Builder=void(*)(uintptr_t,uintptr_t,uintptr_t,int32_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,int32_t,uintptr_t);
    using Clock=std::chrono::steady_clock;using Now=std::function<Clock::time_point()>;
    struct PreserveError {DWORD value{GetLastError()};~PreserveError(){SetLastError(value);}};
    struct Source {NativeCameraEntries::OrdinaryPair pair;std::array<uint64_t,2> flags{};std::array<vr_camera::Matrix,2> worlds{};};
    static NativeWorkerVisibilityProbe& get(){static auto* value=new NativeWorkerVisibilityProbe;return *value;}
    static bool enabled(){return get().m_native!=nullptr;}
    static std::optional<Source> source(uintptr_t add_list){
        if(add_list<0x160 || add_list>UINTPTR_MAX-0x29b18)return {};
        const auto render=add_list-0x160;auto& entries=NativeCameraEntries::get();const auto revision=entries.revision(render);if(!revision)return {};
        int32_t count{};if(!ordinary_scene_scope::read(add_list+0x29808,count) || count<2 || count>64)return {};
        Source value;std::array<NativeCameraEntries::Entry,2> actual{};
        for(size_t i=0;i<2;++i){const auto address=add_list+8+i*0xa60;uint8_t id{};
            if(!ordinary_scene_scope::read(address,value.flags[i]) || (value.flags[i]&0x50000044) || !ordinary_scene_scope::read(address+0x33,id) || id!=i ||
               !ordinary_scene_scope::read(address+0x34,value.worlds[i]))return {};
            actual[i]={address,int32_t(i),NativeCameraEntries::Role::main,value.worlds[i]};
        }
        const auto pair=entries.ordinary_pair(render,actual,*revision);if(!pair || !entries.current(*pair))return {};value.pair=*pair;return value;
    }
    bool install(void* target,const std::filesystem::path& directory,Now now=Clock::now,void* builder=nullptr){
        if(m_native || !target || !directory.is_absolute() || !std::filesystem::is_directory(directory) || !now)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        m_directory=directory;m_now=std::move(now);
        if(builder && MH_CreateHook(builder,reinterpret_cast<void*>(&builder_hook),reinterpret_cast<void**>(&m_builder))!=MH_OK){MH_RemoveHook(target);m_native=nullptr;return false;}
        if(MH_EnableHook(target)==MH_OK && (!builder || MH_EnableHook(builder)==MH_OK))return true;
        MH_DisableHook(target);MH_RemoveHook(target);if(builder){MH_DisableHook(builder);MH_RemoveHook(builder);}m_native=nullptr;m_builder=nullptr;return false;
    }
    bool install_in_game(uintptr_t base,const std::filesystem::path& directory){
        constexpr game_build::Code expected=game_build::hashed(0x1f7a8e0,44,"2537f76a2bf43d673c608adf07e5db4181c94bedf1ce62b666dfd6e831fcb38b");
        std::array<uint8_t,44> bytes{};if(!ordinary_scene_scope::read(base+game_build::rva(0x1f7a8e0),bytes) || !game_build::code(0x1f7a8e0,bytes,expected))return false;
        constexpr game_build::Code builder_expected=game_build::hashed(0x1f7bf10,34,"f856c6cba1e92c814870d8ed258be97a65e7b8d9807ed319c1b65f6373397c89");
        std::array<uint8_t,34> builder_bytes{};if(!ordinary_scene_scope::read(base+game_build::rva(0x1f7bf10),builder_bytes) || !game_build::code(0x1f7bf10,builder_bytes,builder_expected))return false;
        constexpr game_build::Code caller_expected=game_build::hashed(0x1e7928f,5,"438dfa6d2af503bc80a52eeac13287263fda9024e3e48624d859866c7510542e");std::array<uint8_t,5> caller_bytes{};
        if(!ordinary_scene_scope::read(base+game_build::rva(0x1e7928f),caller_bytes) || !game_build::code(0x1e7928f,caller_bytes,caller_expected))return false;
        m_base=base;if(!install(reinterpret_cast<void*>(base+game_build::rva(0x1f7a8e0)),directory,Clock::now,reinterpret_cast<void*>(base+game_build::rva(0x1f7bf10))))return false;
        return m_causal.configure(directory,base+game_build::rva(0x1e79294),source,same);
    }
    class Scope {
    public:
        Scope(const std::optional<NativeRenderProbe::Scene>& scene,const std::optional<NativeCameraEntries::OrdinaryPair>& pair){
            auto& self=get();if(!self.m_native || !scene || !pair)return;PreserveError error;
            try{ordinary_scene_scope::Scope owned{*scene,*pair};if(ordinary_scene_scope::inspect(owned).mask&~uint32_t(16))return;
                if(scene->slot==0)self.m_causal.poll();
                std::lock_guard lock{self.m_mutex};if(scene->slot==0)self.poll();
                if(self.m_active && self.m_source && matches(self.m_source->pair,*pair)){m_owned=owned;m_generation=self.m_generation;}
            }catch(...){}
        }
        void finish(){
            if(!m_owned)return;PreserveError error;auto& self=get();try{std::lock_guard lock{self.m_mutex};
                if(!self.m_active || self.m_generation!=m_generation)return;
                if(self.m_now()>=self.m_until){self.m_active=false;self.save({},"expired_unjoined");return;}
                if(!self.m_source || !matches(self.m_source->pair,m_owned->pair) || ordinary_scene_scope::inspect(*m_owned).mask)return;
                if(m_owned->scene.slot==0)self.m_dispatch=m_owned->scene.dispatch_sequence;
                else if(self.m_dispatch==m_owned->scene.dispatch_sequence){self.m_active=false;self.save(m_owned,"joined");}
            }catch(...){}
        }
    private:std::optional<ordinary_scene_scope::Scope> m_owned;uint64_t m_generation{};
    };
private:
    friend struct ObjectVisibilityProbeTests;
    struct Build {uintptr_t category{};int32_t slot{};uintptr_t material{},entry{},scratch{},output{},caller{},object{};uint64_t handle{};uint32_t packed190{};uint16_t interim_flags194{};};
    struct Row {uint64_t handle{},mask_before{},mask_after{};uintptr_t caller{};DWORD thread{};int32_t worker{};native_geometry_identity::Evidence identity;std::vector<Build> builds;uint64_t mask_effective{};std::string causal_id;};
    struct Pending {Source source;Row row;uintptr_t handle_pointer{},mask_pointer{};uint64_t generation{};Clock::time_point until{};};
    struct BuildPending {Pending* worker{};Build row;uintptr_t context{};};
    inline static thread_local Pending* s_worker{};
    struct WorkerScope {Pending* previous{s_worker};explicit WorkerScope(Pending* value){s_worker=value;}~WorkerScope(){s_worker=previous;}};
    template<class Note> static void passive_note(Note&& note)noexcept{try{note();}catch(...) {}}
    struct Cached {uintptr_t base{};native_geometry_identity::Evidence identity;};
    struct Unknown {uint64_t handle{};uint32_t mask{};uint8_t kind{};};
    struct Rejection {uint64_t handle{},before{},after{};uint32_t identity_mask{};bool inputs_read{},source_stable{},handle_stable{};};
    static bool wanted(const native_geometry_identity::Evidence& e){return !e.mask && e.class_chain_stable && e.weak_roundtrip &&
        (e.class_name=="test_scene_mod__usable_button_interplay__x__floor_placement__actor" || e.class_name=="test_scene_mod__usable_button_interplay__x__button__actor");}
    static bool matches(const NativeCameraEntries::OrdinaryPair& a,const NativeCameraEntries::OrdinaryPair& b){return a.producer_list==b.producer_list && a.producer_revision==b.producer_revision && a.left.render_id==b.left.render_id && a.left.token==b.left.token;}
    static bool same(const Source& a,const Source& b){return matches(a.pair,b.pair) && a.pair.left.add_list==b.pair.left.add_list && a.pair.left.revision==b.pair.left.revision && a.flags==b.flags && a.worlds==b.worlds;}
    static bool safe_name(const std::string& name){return !name.empty() && name.size()<=40 && std::ranges::all_of(name,[](unsigned char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';});}
    void poll(){ // Only called under the mutex, outside worker callbacks.
        const auto now=m_now();if(m_active && now>=m_until){m_active=false;save({},"expired_unjoined");}
        if(m_active || now<m_next || m_requests>=8)return;m_next=now+std::chrono::seconds(1);
        std::ifstream file{m_directory/"draw_visibility_request.txt",std::ios::binary};std::array<char,43> raw{};file.read(raw.data(),raw.size());std::string name{raw.data(),size_t(file.gcount())};
        if(!name.empty() && name.back()=='\n')name.pop_back();if(!name.empty() && name.back()=='\r')name.pop_back();
        if(!safe_name(name) || name==m_last || std::filesystem::exists(m_directory/(name+".worker_visibility.json")))return;
        m_name=name;m_last=name;++m_requests;++m_generation;m_rows.clear();m_cache.clear();m_unknown.clear();m_failures.clear();m_source.reset();m_dispatch=0;m_lookups=m_dropped=m_rejected=m_after_validations=m_after_exhausted=m_builder_validations=m_builder_exhausted=m_builder_rejected=0;
        m_until=now+std::chrono::seconds(5);m_active=true;
    }
    std::optional<Pending> before(uintptr_t list,int32_t worker,uintptr_t handle,uintptr_t mask,uintptr_t caller){
        uint64_t generation{};Clock::time_point until{};
        {std::lock_guard lock{m_mutex};if(!m_active || m_now()>=m_until)return {};generation=m_generation;until=m_until;}
        const auto current=[&]{return m_active && m_generation==generation && m_until==until && m_now()<until;};
        uint64_t encoded{},views{};if(!ordinary_scene_scope::read(handle,encoded) || !ordinary_scene_scope::read(mask,views)){std::lock_guard lock{m_mutex};if(current())++m_rejected;return {};}
        native_geometry_identity::Evidence identity;const uintptr_t key=uintptr_t(encoded&0xffffffffffffULL);bool cached{};
        {std::lock_guard lock{m_mutex};if(!current())return {};
            const auto found=std::ranges::find(m_cache,key,&Cached::base);if(found!=m_cache.end()){identity=found->identity;cached=true;}
            else {if(m_lookups>=64){++m_dropped;return {};}++m_lookups;}
        }
        if(!cached){identity=native_geometry_identity::capture_handle(handle);std::lock_guard lock{m_mutex};if(!current())return {};if(m_cache.size()<64)m_cache.push_back({key,identity});if(identity.mask && m_unknown.size()<8)m_unknown.push_back({encoded,identity.mask,identity.kind_flags});}
        // Cached names are only cheap filtering hints. Every retained event gets
        // a fresh full identity after native work; cached addresses never prove it.
        if(!wanted(identity))return {};const auto owner=source(list);if(!owner){std::lock_guard lock{m_mutex};if(current())++m_rejected;return {};}
        {std::lock_guard lock{m_mutex};if(!current())return {};if((m_source && !matches(m_source->pair,owner->pair)) || m_rows.size()>=128){++m_dropped;return {};}}
        return Pending{*owner,{encoded,views,0,caller,GetCurrentThreadId(),worker,{}},handle,mask,generation,until};
    }
    void after(Pending& event){
        const auto current=[&]{return m_active && m_generation==event.generation && m_until==event.until && m_now()<event.until;};
        {std::lock_guard lock{m_mutex};if(!current())return;
            // Reserve before any full post-native reads. Failed validations
            // consume work too; the mutex also bounds concurrent callbacks.
            if(m_after_validations>=128){++m_after_exhausted;return;}++m_after_validations;
        }
        auto identity=native_geometry_identity::capture_handle(event.handle_pointer);uint64_t encoded{},views{};const auto owner=source(event.source.pair.left.add_list);
        const bool inputs_read=ordinary_scene_scope::read(event.handle_pointer,encoded) && ordinary_scene_scope::read(event.mask_pointer,views);
        const bool source_stable=owner && same(event.source,*owner),handle_stable=inputs_read && encoded==event.row.handle && identity.handle==encoded;
        const bool valid=inputs_read && handle_stable && wanted(identity) && source_stable;
        std::lock_guard lock{m_mutex};if(!current())return;
        if(!valid || views!=event.row.mask_before){++m_rejected;if(m_failures.size()<8)m_failures.push_back({event.row.handle,event.row.mask_before,views,identity.mask,inputs_read,source_stable,handle_stable});if(identity.mask && m_unknown.size()<8)m_unknown.push_back({event.row.handle,identity.mask,identity.kind_flags});return;}
        if(m_source && !matches(m_source->pair,event.source.pair)){++m_dropped;return;}if(m_rows.size()>=128){++m_dropped;return;}
        if(!m_source)m_source=event.source;event.row.mask_after=views;event.row.identity=std::move(identity);m_rows.push_back(std::move(event.row));
    }
    bool current(const Pending& e)const{return m_active && m_generation==e.generation && m_until==e.until && m_now()<e.until;}
    std::optional<BuildPending> before_build(uintptr_t category,uintptr_t material,uintptr_t entry,int32_t slot,uintptr_t scratch,uintptr_t context,uintptr_t output,uintptr_t caller){
        auto* worker=s_worker;if(!worker)return {};
        {std::lock_guard lock{m_mutex};if(!current(*worker))return {};if(m_builder_validations>=128){++m_builder_exhausted;return {};}++m_builder_validations;}
        uint64_t handle{},scratch_handle{};const auto owner=source(worker->source.pair.left.add_list);
        const bool valid=slot>=0 && slot<2 && entry==worker->source.pair.left.add_list+8+size_t(slot)*0xa60 &&
            owner && same(worker->source,*owner) && ordinary_scene_scope::read(context,handle) && handle==worker->row.handle &&
            scratch<=UINTPTR_MAX-0x100 && ordinary_scene_scope::read(scratch+0x100,scratch_handle) && scratch_handle==handle;
        if(!valid){std::lock_guard lock{m_mutex};if(current(*worker))++m_builder_rejected;return {};}
        return BuildPending{worker,{category,slot,material,entry,scratch,output,caller},context};
    }
    void after_build(BuildPending& event){
        auto& worker=*event.worker;{std::lock_guard lock{m_mutex};if(!current(worker))return;}
        uint64_t handle{},scratch_handle{};auto& row=event.row;const auto owner=source(worker.source.pair.left.add_list);
        const bool valid=owner && same(worker.source,*owner) && ordinary_scene_scope::read(event.context,handle) && handle==worker.row.handle &&
            ordinary_scene_scope::read(row.scratch+0x100,scratch_handle) && scratch_handle==handle && row.output<=UINTPTR_MAX-0x196 &&
            ordinary_scene_scope::read(row.output+0x100,row.handle) && row.handle==handle && ordinary_scene_scope::read(row.output+0xf8,row.object) &&
            ordinary_scene_scope::read(row.output+0x190,row.packed190) && ordinary_scene_scope::read(row.output+0x194,row.interim_flags194);
        std::lock_guard lock{m_mutex};if(!current(worker))return;
        if(!valid){++m_builder_rejected;return;}worker.row.builds.push_back(row);
    }
    static void builder_hook(uintptr_t category,uintptr_t material,uintptr_t entry,int32_t slot,uintptr_t scratch,uintptr_t a6,uintptr_t a7,uintptr_t context,uintptr_t output,int32_t a10,uintptr_t a11){
        auto& self=get();const auto incoming=GetLastError();std::optional<BuildPending> event;
        if(s_worker)try{event=self.before_build(category,material,entry,slot,scratch,context,output,reinterpret_cast<uintptr_t>(_ReturnAddress()));}catch(...){}
        SetLastError(incoming);self.m_builder(category,material,entry,slot,scratch,a6,a7,context,output,a10,a11);PreserveError error;
        if(event)try{self.after_build(*event);}catch(...){}
    }
    static void hook(uintptr_t list,int32_t worker,uintptr_t handle,uintptr_t mask,uintptr_t options){
        auto& self=get();if(!self.m_active && !self.m_causal.active()){self.m_native(list,worker,handle,mask,options);return;}
        const auto incoming=GetLastError();std::optional<Pending> event;
        try{event=self.before(list,worker,handle,mask,reinterpret_cast<uintptr_t>(_ReturnAddress()));}catch(...){}
        std::optional<NativeWorkerMaskCausalProbe<Source>::Pending> causal;
        try{causal=self.m_causal.prepare(list,handle,mask,reinterpret_cast<uintptr_t>(_ReturnAddress()));}catch(...){}
        bool applied{};if(causal)try{applied=self.m_causal.apply(*causal);}catch(...){}
        const auto native_mask=applied && causal->effective!=causal->before?reinterpret_cast<uintptr_t>(causal->input.data()):mask;
        passive_note([&]{if(event){event->row.mask_effective=applied?causal->effective:event->row.mask_before;if(applied)event->row.causal_id=causal->id;}});
        WorkerScope context{event?&*event:nullptr};SetLastError(incoming);self.m_native(list,worker,handle,native_mask,options);PreserveError error;
        if(applied)try{self.m_causal.finish(*causal);}catch(...){}
        if(event)try{self.after(*event);}catch(...){}
    }
    void save(const std::optional<ordinary_scene_scope::Scope>& owned,const char* outcome){
        std::ofstream out{m_directory/(m_name+".worker_visibility.json")};out<<"{\"model\":\"ordinary_worker_visibility_v1\",\"outcome\":\""<<outcome<<"\",\"worker_census_complete\":false,\"gpu_execution_verified\":false,\"dispatch\":"<<(owned?owned->scene.dispatch_sequence:0)
            <<",\"descriptor_builder_enabled\":"<<(m_builder?"true":"false")<<",\"descriptor_builder_census_complete\":false"
            <<",\"identity_lookups\":"<<m_lookups<<",\"after_validations\":"<<m_after_validations<<",\"after_validation_exhausted\":"<<m_after_exhausted<<",\"builder_validations\":"<<m_builder_validations<<",\"builder_validation_exhausted\":"<<m_builder_exhausted<<",\"builder_rejected\":"<<m_builder_rejected<<",\"dropped\":"<<m_dropped<<",\"rejected\":"<<m_rejected<<",\"producer\":";
        if(m_source){const auto& s=*m_source;out<<"{\"observed_add_list\":"<<s.pair.left.add_list<<",\"observed_revision\":"<<s.pair.left.revision<<",\"list\":"<<s.pair.producer_list<<",\"revision\":"<<s.pair.producer_revision<<",\"render_id\":"<<s.pair.left.render_id<<",\"token\":"<<s.pair.left.token<<",\"camera_flags\":["<<s.flags[0]<<','<<s.flags[1]<<"],\"camera_world\":[";
            for(size_t i=0;i<2;++i){if(i)out<<',';out<<'[';for(size_t n=0;n<16;++n){if(n)out<<',';out<<s.worlds[i][n];}out<<']';}out<<"]}";
        }else out<<"null";
        out<<",\"events\":[";for(size_t i=0;i<m_rows.size();++i){if(i)out<<',';const auto& r=m_rows[i];out<<"{\"handle\":"<<r.handle<<",\"mask_before\":"<<r.mask_before<<",\"mask_after\":"<<r.mask_after<<",\"mask_effective\":"<<r.mask_effective<<",\"causal_id\":\""<<r.causal_id<<"\",\"caller\":"<<r.caller<<",\"thread\":"<<r.thread<<",\"worker_index\":"<<r.worker<<",\"identity\":";native_geometry_identity::write(out,r.identity);
            out<<",\"descriptor_builds\":[";for(size_t j=0;j<r.builds.size();++j){if(j)out<<',';const auto& b=r.builds[j];out<<"{\"category\":"<<b.category<<",\"slot\":"<<b.slot<<",\"material\":"<<b.material<<",\"camera_entry\":"<<b.entry<<",\"scratch\":"<<b.scratch<<",\"output\":"<<b.output<<",\"caller\":"<<b.caller<<",\"handle\":"<<b.handle<<",\"object\":"<<b.object<<",\"packed190\":"<<b.packed190<<",\"interim_flags194\":"<<b.interim_flags194<<'}';}out<<"]}";}
        out<<"],\"unknowns_unjoined\":[";for(size_t i=0;i<m_unknown.size();++i){if(i)out<<',';const auto& u=m_unknown[i];out<<"{\"handle\":"<<u.handle<<",\"mask\":"<<u.mask<<",\"kind_flags\":"<<unsigned(u.kind)<<'}';}
        out<<"],\"rejections_unjoined\":[";for(size_t i=0;i<m_failures.size();++i){if(i)out<<',';const auto& f=m_failures[i];out<<"{\"handle\":"<<f.handle<<",\"mask_before\":"<<f.before<<",\"mask_after\":"<<f.after<<",\"identity_mask\":"<<f.identity_mask<<",\"inputs_read\":"<<(f.inputs_read?"true":"false")<<",\"source_stable\":"<<(f.source_stable?"true":"false")<<",\"handle_stable\":"<<(f.handle_stable?"true":"false")<<'}';}out<<"]}\n";
    }
    Native m_native{};Builder m_builder{};uintptr_t m_base{};std::filesystem::path m_directory;Now m_now;std::atomic_bool m_active{};std::mutex m_mutex;
    NativeWorkerMaskCausalProbe<Source> m_causal;
    Clock::time_point m_until{},m_next{};std::string m_name,m_last;unsigned m_requests{},m_lookups{},m_dropped{},m_rejected{},m_after_validations{},m_after_exhausted{},m_builder_validations{},m_builder_exhausted{},m_builder_rejected{};uint64_t m_dispatch{},m_generation{};
    std::vector<Row> m_rows;std::vector<Cached> m_cache;std::vector<Unknown> m_unknown;std::vector<Rejection> m_failures;std::optional<Source> m_source;
};
