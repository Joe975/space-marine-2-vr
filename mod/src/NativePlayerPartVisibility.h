#pragma once
#include "EngineArmProbe.h"
#include "GameBuild.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>

// Hides the player's head parts in first person (Sm2Probe's set_policy, every -Controls launch) and the
// -PlayerPartVisibilityCandidate diagnostic. Mutates authenticated named runtime flags, never poses.
class NativePlayerPartVisibility {
public:
    using Clock=EngineArmProbe::Clock;
    enum class Mode { Native,LeftShoulder,ShouldersHead,RightHand,Head };
    // Only shadow-only is durable ownership. Native 0x20bbb50 rebuilds mesh
    // properties then sets CACHE_READY; both shadow setters invalidate it.
    static constexpr uint32_t MASK=0x800,SHADOW=0x800,CACHE_READY=0x100;
    // Native HideObj (0x1d99150) sets these bits at record+0x1c (DAT_143609dac, 0x140000) on parts
    // the outfit leaves off. Read only, to count hidden outfit variants inside the head selection.
    static constexpr uint32_t OUTFIT_HIDDEN=0x140000;
    // Runtime physical indices and part IDs are int16: a class Marine's outfit template carries
    // 3,000-4,500 skinned parts (every helmet, head and armour variant), Titus 114.
    static constexpr int32_t MAX_RECORDS=0x7fff;
    // Head policy on outfit templates (no head_01/helmet_01): every group root, name ending "_grp",
    // with one of these "_"-separated words. Their subtrees hold the leaf, glass, lenses and LODs.
    static bool head_group(std::string_view name) {
        if(!name.ends_with("_grp"))return false;name.remove_suffix(4);
        while(!name.empty()){const auto cut=name.find('_');const auto word=name.substr(0,cut);
            if(word=="head"||word=="helmet"||word=="hood"||word=="lenses")return true;
            if(cut==std::string_view::npos)break;name.remove_prefix(cut+1);}
        return false;
    }
    static const char* name(Mode m) { switch(m){case Mode::Native:return "native";case Mode::LeftShoulder:return "left_shoulder";case Mode::ShouldersHead:return "shoulders_head";case Mode::RightHand:return "right_hand";case Mode::Head:return "head";}return "unknown"; }
    struct Control { std::optional<Mode> mode;std::string status{"invalid"}; };
    static Control parse(std::string value) {
        if(value.size()>63)return {{},"oversized"};
        if(value.ends_with("\r\n"))value.resize(value.size()-2);else if(value.ends_with('\n'))value.pop_back();
        for(auto m:{Mode::Native,Mode::LeftShoulder,Mode::ShouldersHead,Mode::RightHand,Mode::Head})if(value==name(m))return {m,"valid"};
        if(value=="reset")return {Mode::Native,"valid"};
        return {{},"invalid"};
    }
    static Control read_control(const std::filesystem::path& path) {
        std::ifstream in{path,std::ios::binary};if(!in)return {{},"unavailable"};
        std::array<char,64> bytes{};in.read(bytes.data(),bytes.size());const auto n=in.gcount();
        if(n==64)return {{},"oversized"};if(in.bad())return {{},"unavailable"};
        return parse(std::string{bytes.data(),static_cast<size_t>(n)});
    }
    struct Api {
        std::function<bool(uintptr_t,void*,size_t)> read;
        std::function<bool(uintptr_t,uint32_t)> write;
        std::function<bool(const EngineArmProbe::Pose&)> current; // BEFORE native CS only.
        std::function<void(uintptr_t)> enter,leave;
        std::function<Clock::time_point()> now{[]{return Clock::now();}};
        std::function<Control(const std::filesystem::path&)> control{read_control};
    };
    struct Change { uint16_t id{};uint32_t before{},after{}; };
    struct Snapshot {
        bool configured{};Mode requested{Mode::Native};std::optional<Mode> applied{Mode::Native};
        std::string status{"disabled"},control_status{"disabled"};uintptr_t actor{},instance{},resource{};
        uint64_t revision{},polls{},invalid{},superseded{},updates{},writes{},restored{},refused{},lost_lease{},conflicts{},failures{},rollback_failures{},abandoned{};
        // Last layout read (also on refusal) and the last selection: named parts in the resource,
        // runtime records, selected group roots, and selected records the outfit hides.
        int32_t name_count{},record_count{};size_t groups{},outfit_hidden{};
        std::vector<Change> changes;
    };
    static NativePlayerPartVisibility& get(){static auto* p=new NativePlayerPartVisibility;return *p;}
    void configure(Api api){std::lock_guard lock{m_mutex};m_api=std::move(api);m_bound=m_api.current&&m_api.read&&m_api.write;}
    bool control(const std::filesystem::path& path) {
        if(!path.empty()&&!path.is_absolute())return false;
        std::lock_guard lock{m_mutex};m_path=path;++m_generation;m_next={};
        m_state.configured=!path.empty();m_state.control_status=path.empty()?"disabled":"pending";
        if(path.empty())m_state.requested=m_policy;
        m_enabled.store(!path.empty()||m_lease.has_value()||m_policy!=Mode::Native);return true;
    }
    // Production view policy (first-person head hiding). A diagnostic control
    // file, when configured, takes precedence. Ignored until native API is bound.
    void set_policy(Mode mode) {
        std::lock_guard lock{m_mutex};if(!m_bound||m_policy==mode)return;m_policy=mode;
        if(m_path.empty()&&m_state.requested!=mode){m_state.requested=mode;++m_state.revision;}
        if(mode!=Mode::Native)m_enabled.store(true);
    }
    Snapshot snapshot()const{std::lock_guard lock{m_mutex};return m_state;}
    bool enabled()const{return m_enabled.load(std::memory_order_relaxed);}
    void apply(const EngineArmProbe::Pose& pose) {
        if(!enabled())return;
        poll();
        // Body/ArmProbe freshness ownership can take their mutexes. Never do this
        // while holding the native instance critical section or our state mutex.
        Api api;{std::lock_guard lock{m_mutex};api=m_api;if(m_poisoned){fail("rollback_unrecoverable_restart_required");return;}if(!m_lease&&m_state.requested==Mode::Native){m_state.applied=Mode::Native;m_state.status="native";return;}}
        if(!api.current||!api.current(pose)){refuse("publication_unavailable");return;}
        std::lock_guard lock{m_mutex};
        if(m_poisoned){fail("rollback_unrecoverable_restart_required");return;}
        if(!m_lease&&m_state.requested==Mode::Native){m_state.applied=Mode::Native;m_state.status="native";return;}
        if(!api.read||!api.write||!api.enter||!api.leave||!api.now){fail("api_unavailable");return;}
        if(!fresh(pose,api.now())){fail("publication_expired");return;}
        api.enter(pose.owner.instance);
        struct Leave {Api& api;uintptr_t instance;~Leave(){api.leave(instance);}} leave{api,pose.owner.instance};
        try {
            Reader read{api};
            const auto owner=owned(read,pose);
            if(!fresh(pose,api.now()))throw Error{"publication_expired"};
            // Policy mode follows the player across actor replacement (level load,
            // respawn): the old actor is no longer ours, so its lease is released
            // unwritten and the current owner is decoded afresh. Diagnostics stay strict.
            // Records the new owner shares keep the abandoned lease's saved originals.
            std::map<uintptr_t,uint32_t> inherited;
            if(m_lease&&m_lease->owner!=owner&&m_path.empty()){for(const auto& row:m_lease->records)inherited[row.address]=row.original;
                m_lease.reset();++m_state.abandoned;m_state.applied=Mode::Native;}
            if(m_lease&&m_lease->owner!=owner){++m_state.lost_lease;throw Error{"lost_owner_lease"};}
            const auto layout=header(read,pose.owner.instance);
            if(m_lease&&!verify(read,*m_lease,layout,m_state.applied!=m_state.requested)){++m_state.lost_lease;throw Error{"lost_resource_or_topology_lease"};}
            // Every applied record must still contain the mask this diagnostic owns.
            if(m_lease){std::vector<uint16_t> ids;for(const auto& row:m_lease->records)ids.push_back(row.id);prefetch(read,layout,m_lease->indices,ids);
                for(const auto& row:m_lease->records)if((read.at<uint32_t>(row.address+0x20)&MASK)!=SHADOW){++m_state.conflicts;throw Error{"owned_mask_conflict"};}
                read.drop();}
            if(m_lease&&m_state.applied==m_state.requested){m_state.status="applied";return;}
            auto next=decode(read,owner,layout,m_state.requested);
            for(auto& row:next.records)if(auto it=inherited.find(row.address);it!=inherited.end())row.original=it->second;
            if(!fresh(pose,api.now())||owned(read,pose)!=owner||header(read,pose.owner.instance)!=layout||!verify(read,next,layout))throw Error{"prewrite_lease_changed"};
            std::map<uint16_t,Record> all;
            if(m_lease)for(auto row:m_lease->records)all.emplace(row.id,row);
            for(auto& row:next.records){if(auto old=all.find(row.id);old!=all.end())row.original=old->second.original;all[row.id]=row;}
            std::set<uint16_t> selected;for(auto& row:next.records)selected.insert(row.id);
            struct Patch{Record record;uint32_t before{},after{};};std::vector<Patch> patches;
            // Match native setter/remover invalidation once per transaction.
            // CACHE_READY may regenerate later and is never saved as ownership.
            for(auto& [id,row]:all){const auto before=read.at<uint32_t>(row.address+0x20);const auto mask=selected.contains(id)?SHADOW:row.original;
                patches.push_back({row,before,(before&~(MASK|CACHE_READY))|mask});}
            // Validate complete batch before first write; then check each original.
            for(const auto& patch:patches)if(read.at<uint32_t>(patch.record.address+0x20)!=patch.before)throw Error{"prewrite_flags_changed"};
            // All allocations/bookkeeping are prepared before first mutation.
            std::vector<Change> changes;changes.reserve(patches.size());
            for(const auto& patch:patches)changes.push_back({patch.record.id,patch.before,patch.after});
            const bool restoring=m_lease.has_value()&&m_state.requested==Mode::Native;
            std::string completed_status=restoring?"restored":m_state.requested==Mode::Native?"native":"applied";
            const auto groups=next.groups,outfit_hidden=next.outfit_hidden;
            std::optional<Lease> committed_lease;if(!next.records.empty())committed_lease.emplace(std::move(next));
            static_assert(std::is_nothrow_move_assignable_v<decltype(m_lease)>);
            bool committed{};size_t touched{};
            const auto rollback=[&]()noexcept{
                if(committed||!touched)return;++m_state.failures;const auto before=m_state.rollback_failures;
                for(size_t i=0;i<touched;++i){const auto& patch=patches[i];uint32_t current{};bool ok{};
                    // Restoring persistent state also invalidates any properties
                    // rebuilt after a partial write; do not resurrect old cache.
                    try{ok=api.read(patch.record.address+0x20,&current,sizeof(current))&&
                        ((current&MASK)==(patch.after&MASK)||(current&MASK)==(patch.before&MASK))&&
                        api.write(patch.record.address+0x20,(current&~(MASK|CACHE_READY))|(patch.before&MASK));}catch(...){ok=false;}
                    if(!ok)++m_state.rollback_failures;
                }
                if(m_state.rollback_failures!=before)m_poisoned=true;
            };
            struct Recovery {const decltype(rollback)& run;~Recovery(){run();}} recovery{rollback};
            for(size_t i=0;i<patches.size();++i){const auto& patch=patches[i];
                if(read.at<uint32_t>(patch.record.address+0x20)!=patch.before)throw Error{"per_write_flags_changed"};
                // Include a writer which reports failure after partially modifying a word.
                touched=i+1;if(!api.write(patch.record.address+0x20,patch.after))throw Error{"write_failed"};++m_state.writes;
            }
            // These publications are noexcept moves/trivial scalar changes only.
            m_lease=std::move(committed_lease);m_state.changes=std::move(changes);m_state.status=std::move(completed_status);
            m_state.actor=pose.owner.actor;m_state.instance=pose.owner.instance;m_state.resource=layout.resource;
            m_state.applied=m_state.requested;++m_state.updates;if(restoring)++m_state.restored;m_state.groups=groups;m_state.outfit_hidden=outfit_hidden;
            committed=true;
            if(m_path.empty()&&!m_lease&&m_policy==Mode::Native)m_enabled.store(false);
        }catch(const Error& error){fail(m_poisoned?"rollback_unrecoverable_restart_required":error.what());}
        catch(...){fail(m_poisoned?"rollback_unrecoverable_restart_required":"transaction_exception");}
    }
    bool configure_native(HMODULE game) {
        // Binds whenever the image matches, so the head-hiding policy works without
        // the diagnostic control variable; that variable only adds file control.
        wchar_t path[1024]{};const auto length=GetEnvironmentVariableW(L"SM2VR_PLAYER_PART_VISIBILITY_CONTROL",path,std::size(path));
        if(length>=std::size(path)||(length&&!std::filesystem::path{path}.is_absolute()))return false;
        auto* base=reinterpret_cast<const uint8_t*>(game);if(!base)return false;
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>4096)return false;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE||!game_build::supported(nt->FileHeader.TimeDateStamp)||nt->OptionalHeader.SizeOfImage<0x4f31890)return false;
        constexpr std::array<game_build::Code,4> signatures{
            game_build::hashed(0x1d90f60,13,"3c9e042daacf77463fcd3e1d7f005af599b408bfbb0c29c347df4314106d926a"),
            game_build::hashed(0x220d0c0,12,"08bad9fd6929c95c4e0e69b057c921ba973eca50686358784e33b47131da7b22"),
            game_build::hashed(0x225c310,11,"594d4203d6f260ca18cf6e5f717def2d153dabb83c88aeef16baa062491a0807"),
            game_build::hashed(0x225c340,9,"e8b5aef0c290e8ce839b7e34818ac0fe24446662a736d2aa2731ee4ad1e46ea3")};
        for(const auto& bytes:signatures){const auto rva=bytes.reference;const auto at=game_build::rva(rva);if(!at||!game_build::code(rva,std::span<const uint8_t>{base+at,bytes.size()},bytes))return false;}
        if(!game_build::rva(0x4f31888))return false;
        m_arena_global=reinterpret_cast<uintptr_t>(base)+game_build::rva(0x4f31888);
        configure({[](uintptr_t p,void* out,size_t n){SIZE_T got{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(p),out,n,&got)&&got==n;},
            [](uintptr_t p,uint32_t value){SIZE_T got{};return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),&value,sizeof(value),&got)&&got==sizeof(value);},
            [](const auto& pose){return EngineArmProbe::get().current_current(pose);},
            [](uintptr_t p){EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(p+0x288));},
            [](uintptr_t p){LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(p+0x288));}});
        return control(length?std::filesystem::path{path}:std::filesystem::path{});
    }
    void arena_global_for_test(uintptr_t address){m_arena_global=address;}
private:
    struct Error:std::runtime_error{using std::runtime_error::runtime_error;};
    struct Reader {
        Api& api;size_t budget{4*1024*1024};uintptr_t base{};std::vector<uint8_t> window;
        void copy(uintptr_t p,void* out,size_t n){
            if(!window.empty()&&n&&p>=base&&n<=window.size()&&p-base<=window.size()-n){std::memcpy(out,window.data()+(p-base),n);return;}
            if(p<0x10000||p>=0x800000000000ULL||n>budget||!n||n>1048576||p+n>=0x800000000000ULL||!api.read(p,out,n))throw Error{"bounded_native_read_unavailable"};budget-=n;}
        // Checking an outfit template's ~1,000 selected records is one bounded read of their
        // storage span, not thousands of small ones. Unreadable or too large: per-record reads.
        // Dropped before the write phase, whose checks must see memory as it is.
        void prefetch(uintptr_t p,size_t n){window.clear();if(!n||n>1048576||n>budget||p<0x10000||p+n>=0x800000000000ULL)return;
            std::vector<uint8_t> bytes(n);if(!api.read(p,bytes.data(),n))return;budget-=n;base=p;window=std::move(bytes);}
        void drop(){window.clear();}
        template<class T>T at(uintptr_t p){T value{};copy(p,&value,sizeof(value));return value;}
        template<class T>std::vector<T> array(uintptr_t p,size_t n){std::vector<T> value(n);if(n)copy(p,value.data(),n*sizeof(T));return value;}
    };
    struct Layout {uintptr_t instance{},resource{},arena{},ids{},tokens{},indices{},storage{};int32_t count{},index_count{};bool operator==(const Layout&)const=default;};
    struct Record {uint16_t id{};int16_t slot{},parent{},sibling{},child{};uintptr_t address{};uint32_t original{};bool operator==(const Record&)const=default;};
    struct Cell {uint32_t token{};std::string text;bool operator==(const Cell&)const=default;};
    using Owner=std::array<uintptr_t,9>;
    struct Lease {Owner owner{};Layout layout;std::vector<int16_t> ids,indices;std::vector<uint32_t> tokens;std::vector<Cell> cells;std::vector<Record> records;size_t groups{},outfit_hidden{};};
    // Decoded names of the last resource. Interned names do not change while the same resource
    // keeps the same token table, so hiding again after a takeover reads no name text.
    struct Names {uintptr_t arena{},resource{};std::vector<uint32_t> tokens;std::vector<Cell> cells;};
    mutable std::mutex m_mutex;Api m_api;Snapshot m_state;std::optional<Lease> m_lease;std::optional<Names> m_names;
    bool m_poisoned{},m_bound{};Mode m_policy{Mode::Native};
    std::atomic<bool> m_enabled{false};std::filesystem::path m_path;Clock::time_point m_next{};uint64_t m_generation{},m_sequence{};uintptr_t m_arena_global{};
    static bool fresh(const EngineArmProbe::Pose& p,Clock::time_point now){return p.received!=Clock::time_point{}&&now>=p.received&&now-p.received<=std::chrono::milliseconds(100);}
    void fail(const char* error){++m_state.refused;m_state.status=error;m_state.applied.reset();}
    void refuse(const char* error){std::lock_guard lock{m_mutex};fail(error);}
    void poll(){
        Api api;std::filesystem::path path;uint64_t generation{},sequence{};
        {std::lock_guard lock{m_mutex};api=m_api;if(!api.now||m_path.empty())return;const auto now=api.now();if(now<m_next)return;m_next=now+std::chrono::seconds(1);path=m_path;generation=m_generation;sequence=++m_sequence;++m_state.polls;}
        const auto value=api.control?api.control(path):Control{{},"unavailable"};
        std::lock_guard lock{m_mutex};if(generation!=m_generation||sequence!=m_sequence){++m_state.superseded;return;}
        m_state.control_status=value.status;if(!value.mode){++m_state.invalid;return;}if(m_state.requested!=*value.mode){m_state.requested=*value.mode;++m_state.revision;}
    }
    static Owner owned(Reader& r,const EngineArmProbe::Pose& p){const auto& o=p.owner;
        const auto props=r.at<uintptr_t>(o.actor+0x20),entity=r.at<uintptr_t>(o.actor+0x38),link=r.at<uintptr_t>(o.controller+0xe0),wrapper=r.at<uintptr_t>(o.controller+0xe8);
        if(!props||!entity||r.at<uintptr_t>(o.actor+0xd8)!=o.instance||r.at<uintptr_t>(o.instance+0x58)!=props||r.at<uintptr_t>(link)!=o.instance||r.at<uintptr_t>(o.controller+8)!=entity||entity!=o.entity||wrapper!=o.wrapper||r.at<uintptr_t>(wrapper+0x10)!=o.instance||r.at<uintptr_t>(wrapper+0x2858)!=o.network||r.at<uintptr_t>(o.network)!=o.definition||r.at<uintptr_t>(r.at<uintptr_t>(wrapper)+8)!=o.definition||r.at<uint16_t>(o.controller+0x1e9)!=0x100||r.at<uintptr_t>(o.instance+0xd0)!=p.buffer)throw Error{"owner_structure_changed"};
        return {o.actor,o.instance,props,entity,o.controller,wrapper,o.network,o.definition,p.buffer};
    }
    // Each refusal names the field that failed; the counts are kept for the log either way.
    Layout header(Reader& r,uintptr_t instance){
        Layout v;v.instance=instance;v.resource=r.at<uintptr_t>(instance+0x250);if(!v.resource)throw Error{"layout_no_resource"};
        v.arena=r.at<uintptr_t>(m_arena_global);v.ids=r.at<uintptr_t>(v.resource+0x118);v.tokens=r.at<uintptr_t>(v.resource+0x128);v.count=r.at<int32_t>(v.resource+0x130);v.indices=r.at<uintptr_t>(instance+0xa0);v.index_count=r.at<int32_t>(instance+0xa8);v.storage=r.at<uintptr_t>(instance+0xb0);
        m_state.name_count=v.count;m_state.record_count=v.index_count;
        if(!v.arena)throw Error{"layout_no_arena"};if(!v.storage)throw Error{"layout_no_storage"};
        if(v.count<=0||v.count>MAX_RECORDS)throw Error{"layout_name_count"};if(v.index_count<=0||v.index_count>MAX_RECORDS)throw Error{"layout_record_count"};return v;
    }
    static Record record(Reader& r,const Layout& l,const std::vector<int16_t>& indices,uint16_t id){
        if(id>=indices.size()||indices[id]<0)throw Error{"unmapped_runtime_record"};Record out;out.id=id;out.slot=indices[id];out.address=l.storage+out.slot*0x40;out.original=r.at<uint32_t>(out.address+0x20)&MASK;
        const auto links=r.at<std::array<int16_t,4>>(out.address+0x32);out.parent=links[0];out.sibling=links[1];out.child=links[3];
        for(auto link:{out.parent,out.sibling,out.child})if(link < -1||link>=l.index_count)throw Error{"runtime_topology_bounds"};return out;
    }
    static std::string text(Reader& r,uintptr_t arena,uint32_t token){
        if(!token||token>0x6400000-2)throw Error{"intern_token_bounds"};const auto length=r.at<uint16_t>(arena+token);
        if(!length||length>256||token+2+length>0x6400000)throw Error{"intern_name_bounds"};std::string value(length,'\0');r.copy(arena+token+2,value.data(),length);
        for(unsigned char c:value)if(c<32||c>126)throw Error{"intern_name_encoding"};return value;
    }
    static std::vector<std::string_view> names(Mode mode){switch(mode){case Mode::Native:return {};case Mode::LeftShoulder:return {"titus_sholder_armor_L_01"};case Mode::ShouldersHead:return {"titus_sholder_armor_L_01","titus_sholder_armor_R_01","head_01","helmet_01"};case Mode::RightHand:return {"titus_hand_R_01"};case Mode::Head:return {"head_01","helmet_01"};}return {};}
    bool cached(const Layout& l,const std::vector<uint32_t>& tokens)const{return m_names&&m_names->arena==l.arena&&m_names->resource==l.resource&&m_names->tokens==tokens;}
    // One bounded read covering the storage slots of these logical IDs (see Reader::prefetch).
    static void prefetch(Reader& r,const Layout& l,const std::vector<int16_t>& indices,const std::vector<uint16_t>& ids){
        int32_t lo=MAX_RECORDS,hi=-1;for(auto id:ids)if(id<indices.size()&&indices[id]>=0){lo=std::min<int32_t>(lo,indices[id]);hi=std::max<int32_t>(hi,indices[id]);}
        if(hi>=0)r.prefetch(l.storage+static_cast<uintptr_t>(lo)*0x40,static_cast<size_t>(hi-lo+1)*0x40);
    }
    Lease decode(Reader& r,Owner owner,const Layout& l,Mode mode){
        Lease out;out.owner=owner;out.layout=l;out.ids=r.array<int16_t>(l.ids,l.count);out.tokens=r.array<uint32_t>(l.tokens,l.count);out.indices=r.array<int16_t>(l.indices,l.index_count);
        for(auto id:out.ids)if(id<0||id>=l.index_count)throw Error{"resource_id_bounds"};for(auto id:out.indices)if(id < -1)throw Error{"physical_index_bounds"};
        if(cached(l,out.tokens))out.cells=m_names->cells;
        else {std::set<uint32_t> tokens;for(auto token:out.tokens)tokens.insert(token);
            for(auto token:tokens)out.cells.push_back({token,text(r,l.arena,token)});
            m_names=Names{l.arena,l.resource,out.tokens,out.cells};}
        std::map<uint32_t,const std::string*> strings;for(const auto& cell:out.cells)strings[cell.token]=&cell.text;
        const auto named=[&](std::string_view target){std::set<uint16_t> ids;for(size_t i=0;i<out.ids.size();++i)if(*strings.at(out.tokens[i])==target)ids.insert(static_cast<uint16_t>(out.ids[i]));return ids;};
        // Exact single names (Titus, the diagnostics); the head policy on an outfit template
        // without head_01/helmet_01 takes every head, helmet, hood and lenses group instead.
        std::vector<uint16_t> starts;
        const auto targets=names(mode);
        const bool outfit=mode==Mode::Head&&(named("head_01").size()!=1||named("helmet_01").size()!=1);
        if(outfit){std::set<uint16_t> roots;for(size_t i=0;i<out.ids.size();++i)if(head_group(*strings.at(out.tokens[i])))roots.insert(static_cast<uint16_t>(out.ids[i]));
            if(roots.empty())throw Error{"selected_head_parts_absent"};starts.assign(roots.begin(),roots.end());}
        else for(auto target:targets){const auto ids=named(target);if(ids.size()!=1)throw Error{"selected_name_absent_or_ambiguous"};starts.push_back(*ids.begin());}
        {std::vector<uint16_t> all(out.indices.size());for(size_t i=0;i<all.size();++i)all[i]=static_cast<uint16_t>(i);prefetch(r,l,out.indices,all);}
        std::map<uint16_t,Record> records;
        const auto node=[&](uint16_t id)->Record{auto found=records.find(id);if(found!=records.end())return found->second;auto value=record(r,l,out.indices,id);records[id]=value;return value;};
        std::set<uint16_t> selected;
        for(const auto start:starts){uint16_t current=start;std::set<uint16_t> seen;
            for(;;){if(!seen.insert(current).second||seen.size()>512)throw Error{"selected_subtree_cycle_or_bound"};selected.insert(current);const auto row=node(current);
                if(row.child>=0){if(node(row.child).parent!=current)throw Error{"selected_child_parent_mismatch"};current=row.child;continue;}
                std::set<uint16_t> ascending;while(current!=start){if(!ascending.insert(current).second)throw Error{"selected_ancestry_cycle"};const auto value=node(current);if(value.parent<0||value.sibling<0||node(value.sibling).parent!=value.parent)throw Error{"selected_circular_sibling_mismatch"};if(node(value.parent).child!=value.sibling){current=value.sibling;break;}current=value.parent;}
                if(current==start)break;
            }
        }
        if(selected.size()>4096)throw Error{"selected_record_budget"};
        std::map<int16_t,size_t> slots;for(auto slot:out.indices)if(slot>=0)++slots[slot];
        for(auto id:selected)if(slots[out.indices[id]]!=1)throw Error{"aliased_selected_runtime_record"};
        for(auto id:selected){out.records.push_back(node(id));if((r.at<uint32_t>(out.records.back().address+0x1c)&OUTFIT_HIDDEN)==OUTFIT_HIDDEN)++out.outfit_hidden;}
        out.groups=starts.size();r.drop();return out;
    }
    bool verify(Reader& r,const Lease& lease,const Layout& l,bool names=true){
        if(lease.layout!=l||lease.ids!=r.array<int16_t>(l.ids,l.count)||lease.tokens!=r.array<uint32_t>(l.tokens,l.count)||lease.indices!=r.array<int16_t>(l.indices,l.index_count))return false;
        if(names&&!(cached(l,lease.tokens)&&m_names->cells==lease.cells))for(const auto& cell:lease.cells)if(text(r,l.arena,cell.token)!=cell.text)return false;
        std::vector<uint16_t> ids;for(const auto& row:lease.records)ids.push_back(row.id);prefetch(r,l,lease.indices,ids);
        for(const auto& row:lease.records){auto current=record(r,l,lease.indices,row.id);current.original=row.original;if(current!=row){r.drop();return false;}}
        r.drop();return true;
    }
};
