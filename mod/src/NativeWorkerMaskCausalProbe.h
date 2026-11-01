#pragma once
#include "NativeGeometryIdentity.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <charconv>
#include <cmath>

// Build-specific causal input experiment, never a production visibility policy.
// Verified native consumers read this 24-byte mask only during the worker call.
template<class Source> class NativeWorkerMaskCausalProbe {
public:
    enum class Mode {native,stereo_union,left_hide};
    using Clock=std::chrono::steady_clock;using Now=std::function<Clock::time_point()>;
    using Reader=std::function<std::optional<Source>(uintptr_t)>;using Same=std::function<bool(const Source&,const Source&)>;
    struct Pending {alignas(8) std::array<uint8_t,24> original{},input{};Source source;uint64_t generation{},handle{},before{},effective{},ordinary{};uintptr_t instance{},handle_pointer{},mask_pointer{},caller{};std::string id;bool applied{};};
    bool configure(const std::filesystem::path& directory,uintptr_t caller,Reader reader,Same same,Now now=Clock::now){
        std::lock_guard lock{m_mutex};if(m_caller || !caller || !directory.is_absolute() || !std::filesystem::is_directory(directory) || !reader || !same || !now)return false;
        m_directory=directory;m_caller=caller;m_reader=std::move(reader);m_same=std::move(same);m_now=std::move(now);return true;
    }
    bool active()const{return m_active.load();}
    static bool wanted(const native_geometry_identity::Evidence& e){return !e.mask && e.class_chain_stable && e.weak_roundtrip &&
        (e.class_name=="test_scene_mod__usable_button_interplay__x__floor_placement__actor" || e.class_name=="test_scene_mod__usable_button_interplay__x__button__actor");}
    void poll(){
        if(!m_caller)return;std::lock_guard lock{m_mutex};const auto now=m_now();
        if(m_active && now>=m_until){m_active=false;save(m_inflight?"expiring_inflight":"expired");}
        if(m_active || m_inflight || m_faulted || now<m_next || m_seen.size()>=8)return;m_next=now+std::chrono::milliseconds(250);
        std::ifstream file{m_directory/"console_mask_causal_request.txt",std::ios::binary};std::array<char,129> raw{};file.read(raw.data(),raw.size());if(file.gcount()>128)return;
        std::istringstream in{std::string{raw.data(),size_t(file.gcount())}};std::string id,pid_text,duration_text,mode,extra;uint64_t pid{},duration{};
        if(!(in>>id>>pid_text>>duration_text>>mode) || in>>extra || !safe(id) || !number(pid_text,pid) || pid!=GetCurrentProcessId() ||
           !number(duration_text,duration) || !duration || duration>5000 || (mode!="native" && mode!="union" && mode!="left_hide") || std::ranges::find(m_seen,id)!=m_seen.end())return;
        if(std::filesystem::exists(m_directory/(id+".console_mask_causal.json")))return;
        m_seen.push_back(id);m_id=id;m_mode=mode=="union"?Mode::stereo_union:mode=="left_hide"?Mode::left_hide:Mode::native;++m_generation;m_until=now+std::chrono::milliseconds(duration);
        m_cache.clear();m_records.clear();m_lookups=m_attempts=m_exhausted=m_changes=m_rejected=m_contract_failures=m_source_failures=0;m_active=true;save("accepted");
    }
    std::optional<Pending> prepare(uintptr_t list,uintptr_t handle,uintptr_t mask,uintptr_t caller){
        if(!m_active || caller!=m_caller)return {};uint64_t generation{};
        {std::lock_guard lock{m_mutex};if(!live())return {};generation=m_generation;}
        uint64_t encoded{};if(!ordinary_scene_scope::read(handle,encoded))return rejected(generation);
        native_geometry_identity::Evidence hint;const auto key=uintptr_t(encoded&0xffffffffffffULL);bool cached{};
        {std::lock_guard lock{m_mutex};if(!current(generation))return {};const auto found=std::ranges::find(m_cache,key,&Cached::key);
            if(found!=m_cache.end()){hint=found->identity;cached=true;}else {if(m_lookups>=64){++m_rejected;return {};}++m_lookups;}}
        if(!cached){hint=native_geometry_identity::capture_handle(handle);std::lock_guard lock{m_mutex};if(!current(generation))return {};if(m_cache.size()<64)m_cache.push_back({key,hint});}
        if(!wanted(hint))return {};
        {std::lock_guard lock{m_mutex};if(!current(generation))return {};if(m_attempts>=4096){++m_exhausted;return {};}++m_attempts;}
        // A cached name never authorizes the intervention.
        const auto identity=native_geometry_identity::capture_handle(handle);if(!wanted(identity) || identity.handle!=encoded)return rejected(generation);
        const auto owner=m_reader(list);if(!owner)return rejected(generation);
        const auto& pair=owner->pair;const auto left=pair.left.slot,right=pair.right.slot;
        if(left<0 || left>=64 || right<0 || right>=64 || left==right || pair.left.add_list!=list || pair.right.add_list!=list ||
           NativeCameraEntries::address(list,left)!=pair.left.entry || NativeCameraEntries::address(list,right)!=pair.right.entry)return rejected(generation);
        Pending p;p.source=*owner;p.generation=generation;p.handle=encoded;p.instance=identity.instance;p.handle_pointer=handle;p.mask_pointer=mask;p.caller=caller;
        if(!ordinary_scene_scope::read(mask,p.original))return rejected(generation);p.input=p.original;std::memcpy(&p.before,p.original.data(),8);
        p.ordinary=(uint64_t(1)<<left)|(uint64_t(1)<<right);p.effective=p.before;
        {std::lock_guard lock{m_mutex};if(!current(generation))return {};p.id=m_id;
            if(m_mode==Mode::stereo_union && (p.before&p.ordinary))p.effective|=p.ordinary;
            else if(m_mode==Mode::left_hide)p.effective&=~(uint64_t(1)<<left);}
        const uint64_t added=p.effective&~p.before;uint32_t distances{};uintptr_t pointer{};std::memcpy(&distances,p.original.data()+12,4);std::memcpy(&pointer,p.original.data()+16,8);
        if(added && distances){for(const auto slot:{left,right})if(added&(uint64_t(1)<<slot)){float value{};
            if(!pointer || pointer>UINTPTR_MAX-size_t(slot)*4 || !ordinary_scene_scope::read(pointer+size_t(slot)*4,value) || !std::isfinite(value) || value<0)return rejected(generation);}}
        std::memcpy(p.input.data(),&p.effective,8);return p;
    }
    bool apply(Pending& p){
        {std::lock_guard lock{m_mutex};if(!current(p.generation))return false;}
        const auto owner=m_reader(p.source.pair.left.add_list);uint64_t handle{};std::array<uint8_t,24> mask{};
        const bool valid=owner && m_same(p.source,*owner) && ordinary_scene_scope::read(p.handle_pointer,handle) &&
            handle==p.handle && ordinary_scene_scope::read(p.mask_pointer,mask) && mask==p.original;
        // Full identity is rechecked after preparation immediately before use.
        const auto identity=valid?native_geometry_identity::capture_handle(p.handle_pointer):native_geometry_identity::Evidence{};
        std::lock_guard lock{m_mutex};if(!current(p.generation))return false;
        if(!valid || !wanted(identity) || identity.handle!=handle || identity.instance!=p.instance){++m_rejected;return false;}p.applied=true;++m_inflight;return true;
    }
    void finish(Pending& p){
        std::array<uint8_t,24> original{};uint64_t input_after{};std::memcpy(&input_after,p.input.data(),8);
        const bool readable=ordinary_scene_scope::read(p.mask_pointer,original);const auto owner=m_reader(p.source.pair.left.add_list);
        const bool source_stable=owner && m_same(p.source,*owner),contract=readable && original==p.original &&
            (p.effective==p.before || input_after==p.effective) && std::equal(p.input.begin()+8,p.input.end(),p.original.begin()+8);
        std::lock_guard lock{m_mutex};if(m_generation!=p.generation || !p.applied)return;
        if(m_inflight)--m_inflight;if(!contract){++m_contract_failures;m_active=false;m_faulted=true;}if(!source_stable){++m_source_failures;m_active=false;}
        if(p.effective!=p.before)++m_changes;
        if(m_records.size()<4096)m_records.push_back({p.source,p.handle,p.instance,p.caller,p.before,p.effective,input_after,p.ordinary,source_stable,contract});
        if(!contract)save("native_mask_contract_failed");else if(!source_stable)save("source_changed");else if(!m_active || m_now()>=m_until){m_active=false;save(m_inflight?"expiring_inflight":"expired");}
    }
private:
    struct Cached {uintptr_t key{};native_geometry_identity::Evidence identity;};
    struct Record {Source source;uint64_t handle{};uintptr_t instance{},caller{};uint64_t before{},effective{},after{},ordinary{};bool stable{},contract{};};
    static bool safe(const std::string& s){return !s.empty() && s.size()<=40 && std::ranges::all_of(s,[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';});}
    static bool number(const std::string& s,uint64_t& v){const auto r=std::from_chars(s.data(),s.data()+s.size(),v);return r.ec==std::errc{} && r.ptr==s.data()+s.size();}
    bool live()const{return m_active && m_now()<m_until;}
    bool current(uint64_t generation)const{return live() && generation==m_generation;}
    std::optional<Pending> rejected(uint64_t generation){std::lock_guard lock{m_mutex};if(current(generation))++m_rejected;return {};}
    void save(const char* outcome){
        std::ofstream out{m_directory/(m_id+".console_mask_causal.json")};out<<"{\"model\":\"console_worker_mask_causal_v1\",\"pid\":"<<GetCurrentProcessId()<<",\"id\":\""<<m_id<<"\",\"mode\":\""<<(m_mode==Mode::stereo_union?"union":m_mode==Mode::left_hide?"left_hide":"native")<<"\",\"outcome\":\""<<outcome<<"\",\"changes\":"<<m_changes<<",\"attempts\":"<<m_attempts<<",\"exhausted\":"<<m_exhausted<<",\"identity_lookups\":"<<m_lookups<<",\"rejected\":"<<m_rejected<<",\"contract_failures\":"<<m_contract_failures<<",\"source_failures\":"<<m_source_failures<<",\"inflight\":"<<m_inflight<<",\"records\":[";
        for(size_t i=0;i<m_records.size();++i){if(i)out<<',';const auto& r=m_records[i];const auto& p=r.source.pair;out<<"{\"handle\":"<<r.handle<<",\"instance\":"<<r.instance<<",\"caller\":"<<r.caller<<",\"producer_list\":"<<p.producer_list<<",\"producer_revision\":"<<p.producer_revision<<",\"observed_add_list\":"<<p.left.add_list<<",\"observed_revision\":"<<p.left.revision<<",\"render_id\":"<<p.left.render_id<<",\"token\":"<<p.left.token<<",\"ordinary_bits\":"<<r.ordinary<<",\"before\":"<<r.before<<",\"effective\":"<<r.effective<<",\"input_after\":"<<r.after<<",\"source_stable\":"<<(r.stable?"true":"false")<<",\"mask_contract\":"<<(r.contract?"true":"false")<<'}';}out<<"]}\n";
    }
    uintptr_t m_caller{};std::filesystem::path m_directory;Reader m_reader;Same m_same;Now m_now;mutable std::mutex m_mutex;std::atomic_bool m_active{};
    Clock::time_point m_until{},m_next{};uint64_t m_generation{};std::string m_id;Mode m_mode{Mode::native};bool m_faulted{};unsigned m_lookups{},m_attempts{},m_exhausted{},m_changes{},m_rejected{},m_contract_failures{},m_source_failures{},m_inflight{};
    std::vector<std::string> m_seen;std::vector<Cached> m_cache;std::vector<Record> m_records;
};
