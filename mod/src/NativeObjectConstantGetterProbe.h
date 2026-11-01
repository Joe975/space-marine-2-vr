#pragma once
#include "NativeCommandBindingIdentity.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>

// Passive metadata from an ORIGINAL getter invocation. Never calls a getter to
// answer a lookup, and never reads a GPU virtual address as process memory.
class NativeObjectConstantGetterProbe {
public:
    using Clock=std::chrono::steady_clock;using Now=std::function<Clock::time_point()>;
    using Reader=std::function<bool(uintptr_t,void*,size_t)>;
    using Getter=uintptr_t(*)(uintptr_t,uintptr_t);
    static constexpr size_t RECORD_LIMIT=32,CALL_LIMIT=262144,VALIDATION_LIMIT=64,CHUNKS_PER_FRAME=8,CHEAP_READ_LIMIT=1048576,CHEAP_RESERVATION=16;
    struct Request {std::string id;uint32_t pid{};uintptr_t instance{};unsigned thread{},duration{};};
    enum Refusal:uint32_t {none=0,not_armed=1,expired=2,wrong_context=3,source_changed=4,missing_return=5,exhausted=6,unsupported_flags=7,poisoned=8,unreadable=9};
    struct Locator {uint32_t refusal{not_armed};uint64_t generation{},sequence{};uintptr_t chunk{},resource{},cpu{},gpu{},stream{},allocator{};unsigned frame{},slot{};uint32_t reads{};std::string id;uint64_t observed_calls{},reserved_cheap_reads{};unsigned validations{};};
    struct Context {uintptr_t device{},stream{},allocator{},resource{};uint8_t flags{};native_geometry_identity::Evidence owner;};
    struct Metadata {uintptr_t chunk{},vtable{},wrapper{},resource{},cpu{},gpu_getter{},cpu_getter{};unsigned frame{},slot{};};
    static NativeObjectConstantGetterProbe& get(){static auto* p=new NativeObjectConstantGetterProbe;return *p;}
    static bool rpm(uintptr_t p,void* out,size_t n){SIZE_T bytes{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,n,&bytes) && bytes==n;}
    static bool parse(const std::string& text,Request& r){
        if(text.empty() || text.size()>160)return false;std::istringstream in{text};std::string pid,instance,thread,duration,extra;
        if(!(in>>r.id>>pid>>instance>>thread>>duration) || in>>extra || r.id.empty() || r.id.size()>40 ||
            !std::ranges::all_of(r.id,[](unsigned char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';}))return false;
        const auto number=[](std::string_view s,uint64_t& v){int radix=10;if(s.starts_with("0x")){s.remove_prefix(2);radix=16;}if(s.empty())return false;const auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),v,radix);return ec==std::errc{} && end==s.data()+s.size();};
        uint64_t a{},b{},c{},d{};if(!number(pid,a) || !number(instance,b) || !number(thread,c) || !number(duration,d) ||
            !a || a>UINT32_MAX || b<0x10000 || b>=0x800000000000ULL-0x300 || (b&7) || c>=64 || !d || d>5000)return false;
        r.pid=uint32_t(a);r.instance=uintptr_t(b);r.thread=unsigned(c);r.duration=unsigned(d);return true;
    }
    void configure(uintptr_t base,const std::filesystem::path& directory){
        native_command_binding::PreserveError error;if(!directory.is_absolute() || !std::filesystem::is_directory(directory))return;
        std::lock_guard lock{m_mutex};if(m_base)return;m_base=base;m_directory=directory;
        m_image=native_command_binding::inspect_image(base,rpm);
    }
    void poll(){
        native_command_binding::PreserveError error;
        try{
            std::lock_guard poll_lock{m_poll};const auto now=m_now();
            {std::lock_guard lock{m_mutex};if(!m_base || now<m_next || m_seen.size()>=8)return;m_next=now+std::chrono::milliseconds(50);if(m_active && now<m_until)return;}
            std::ifstream input{m_directory/"object_constant_getter_request.txt",std::ios::binary};std::array<char,161> bytes{};input.read(bytes.data(),bytes.size());
            Request request;if(!parse({bytes.data(),size_t(input.gcount())},request) || request.pid!=GetCurrentProcessId())return;
            {std::lock_guard lock{m_mutex};if(std::ranges::find(m_seen,request.id)!=m_seen.end())return;m_seen.push_back(request.id);}
            if(!install_game())return;
            if(!arm(request))return;
            // Written only after hook installation and a fully qualified source
            // lease. The caller waits for this acknowledgement before allocation.
            const auto path=m_directory/(request.id+".cb_getter_ack.json");if(std::filesystem::exists(path)){disarm();return;}
            std::ofstream out{path};if(!out){disarm();return;}
            {std::lock_guard lock{m_mutex};out<<"{\"id\":\""<<request.id<<"\",\"pid\":"<<request.pid<<",\"generation\":"<<m_generation<<",\"instance\":"<<request.instance<<",\"producer_thread\":"<<request.thread<<",\"duration_ms\":"<<request.duration<<",\"stream\":"<<m_context.stream<<",\"allocator\":"<<m_context.allocator<<",\"candidate_chunks\":"<<m_candidates.size()<<",\"armed\":true,\"native_calls_added\":false}\n";}
            out.flush();if(!out)disarm();
        }catch(...){disarm();}
    }
    template<class Read> Locator lookup(uintptr_t stream,uintptr_t allocator,uint64_t gpu,uint32_t offset,uint32_t count,const native_geometry_identity::Evidence& owner,Read&& read){
        native_command_binding::PreserveError error;Locator out;Request request;Context context;Record record;uint64_t generation{};
        {std::lock_guard lock{m_mutex};out.id=m_request.id;out.generation=m_generation;out.observed_calls=m_calls;out.reserved_cheap_reads=m_cheap_reads;out.validations=m_validations;out.refusal=current();if(out.refusal)return out;
            if(!native_command_binding::same(m_context.owner,owner) || stream!=m_context.stream || allocator!=m_context.allocator){out.refusal=wrong_context;return out;}
            const auto found=std::ranges::find(m_records,gpu,&Record::gpu);if(found==m_records.end()){out.refusal=missing_return;return out;}
            if(found->poison){out.refusal=poisoned;return out;}request=m_request;context=m_context;record=*found;generation=m_generation;
        }
        try{
            Bounded<Read> memory{read};const auto actual=source(request,memory);const auto meta=metadata(record.meta.chunk,actual,request.thread,memory,false);
            memory.recheck();out.reads=memory.reads;
            if(!same(context,actual) || !same(record.meta,meta) || !count || count>4096 || uint64_t(offset)+count>0x40000)throw Failure{source_changed};
            {std::lock_guard lock{m_mutex};out.refusal=current();if(out.refusal)return out;if(generation!=m_generation){out.refusal=wrong_context;return out;}
                const auto now=std::ranges::find(m_records,record.meta.chunk,[](const Record& r){return r.meta.chunk;});
                if(now==m_records.end() || now->poison || now->sequence!=record.sequence){out.refusal=source_changed;return out;}}
            if(meta.cpu>0x800000000000ULL-uint64_t(offset)*16-uint64_t(count)*16)throw Failure{source_changed};
            out.refusal=none;out.generation=generation;out.sequence=record.sequence;out.chunk=meta.chunk;out.resource=meta.resource;out.cpu=meta.cpu;out.gpu=gpu;out.stream=stream;out.allocator=allocator;out.frame=meta.frame;out.slot=meta.slot;out.reads=memory.reads;return out;
        }catch(const Failure& f){out.refusal=f.reason;}catch(...){out.refusal=unreadable;}
        // A detected A->B transition poisons this chunk for the entire request;
        // returning to A cannot resurrect its old original-return observation.
        {std::lock_guard lock{m_mutex};if(generation==m_generation)for(auto& r:m_records)if(r.meta.chunk==record.meta.chunk)r.poison=true;}
        return out;
    }
    static void write(std::ostream& out,const Locator& l){out<<"{\"refusal\":"<<l.refusal<<",\"request_id\":\""<<l.id<<"\",\"generation\":"<<l.generation<<",\"sequence\":"<<l.sequence<<",\"chunk\":"<<l.chunk<<",\"resource\":"<<l.resource<<",\"cpu_base\":"<<l.cpu<<",\"observed_original_return\":"<<l.gpu<<",\"stream\":"<<l.stream<<",\"allocator\":"<<l.allocator<<",\"membership_frame\":"<<l.frame<<",\"membership_slot\":"<<l.slot<<",\"reads\":"<<l.reads<<",\"observed_calls\":"<<l.observed_calls<<",\"reserved_cheap_reads\":"<<l.reserved_cheap_reads<<",\"validations\":"<<l.validations<<",\"atomic\":false,\"gpu_contents_verified\":false,\"native_calls_added\":false}";}
private:
    friend struct ObjectConstantGetterTests;
    struct Failure {uint32_t reason;};
    template<class Read> struct Bounded {
        Read& raw;uint32_t reads{};size_t used{},bytes{};
        struct Cell {uintptr_t p{};size_t n{};std::array<uint8_t,128> data{};};std::array<Cell,64> cells{};
        bool copy(uintptr_t p,void* out,size_t n){if(reads>=128 || !n || n>128 || p<0x10000 || p>0x800000000000ULL-n || bytes+n>8192)return false;++reads;bytes+=n;return raw(p,out,n);}
        void read(uintptr_t p,void* out,size_t n){if(used>=cells.size() || !copy(p,out,n))throw Failure{unreadable};auto& cell=cells[used++];cell.p=p;cell.n=n;std::memcpy(cell.data.data(),out,n);}
        template<class T> T at(uintptr_t p){T value{};read(p,&value,sizeof(value));return value;}
        uintptr_t ptr(uintptr_t p){const auto value=at<uintptr_t>(p);if(value<0x10000 || value>=0x800000000000ULL)throw Failure{unreadable};return value;}
        void recheck(){for(size_t i=0;i<used;++i){std::array<uint8_t,128> after{};if(!copy(cells[i].p,after.data(),cells[i].n) || std::memcmp(after.data(),cells[i].data.data(),cells[i].n))throw Failure{source_changed};}}
    };
    struct Record {Metadata meta;uint64_t gpu{},sequence{};bool poison{};};
    struct Pending {Request request;Context context;Metadata meta;uint64_t generation{};bool known{},valid{};};
    template<class Read> Context source(const Request& r,Bounded<Read>& memory){
        Context c;const uint64_t handle=(uint64_t(469)<<48)|(r.instance+0x70);const auto local=reinterpret_cast<uintptr_t>(&handle);
        c.owner=native_geometry_identity::capture(local,[&](uintptr_t p,void* target,size_t n){if(p==local && n==8){std::memcpy(target,&handle,8);return true;}return memory.copy(p,target,n);},0);
        if(!native_command_binding::player(c.owner) || c.owner.instance!=r.instance)throw Failure{wrong_context};
        c.resource=memory.ptr(r.instance+0x250);c.device=memory.ptr(m_base+game_build::rva(0x4ff0c08));const auto threads=memory.template at<int32_t>(c.device+0x180);
        if(threads<1 || threads>64 || r.thread>=unsigned(threads))throw Failure{wrong_context};c.stream=memory.ptr(c.device+0x398+r.thread*8);
        c.flags=memory.template at<uint8_t>(c.stream+0x10);if(c.flags&1)throw Failure{unsupported_flags};c.allocator=memory.ptr(c.stream+0x28);
        const auto alignment=memory.template at<uint32_t>(c.allocator);if(!alignment || alignment>4096 || (alignment&(alignment-1)))throw Failure{wrong_context};return c;
    }
    template<class Read> Metadata metadata(uintptr_t chunk,const Context& c,unsigned,Bounded<Read>& memory,bool current_frame_only){
        Metadata out;out.chunk=chunk;const auto current=memory.template at<uint32_t>(c.allocator+0x5c0);if(current>1)throw Failure{wrong_context};unsigned matches{};
        for(unsigned frame=0;frame<2;++frame){if(current_frame_only && frame!=current)continue;const auto last=memory.template at<int32_t>(c.allocator+frame*0x2c0+0x280);
            if(last<0 || last>=int32_t(CHUNKS_PER_FRAME))throw Failure{exhausted};for(int32_t i=0;i<=last;++i)if(memory.template at<uintptr_t>(c.allocator+frame*0x2c0+0x40+i*8)==chunk){++matches;out.frame=frame;out.slot=unsigned(i);}}
        if(matches!=1)throw Failure{wrong_context};out.vtable=memory.ptr(chunk);if(out.vtable!=m_base+game_build::rva(0x37a4840))throw Failure{wrong_context};
        out.gpu_getter=memory.ptr(out.vtable+8);out.cpu_getter=memory.ptr(out.vtable+16);
        if(out.gpu_getter!=m_base+game_build::rva(0x2100620) || out.cpu_getter!=m_base+game_build::rva(0x2100640))throw Failure{wrong_context};
        out.wrapper=memory.ptr(chunk+0x18);out.resource=memory.ptr(out.wrapper+0x28);out.cpu=memory.ptr(chunk+0x30);return out;
    }
    static bool same(const Context& a,const Context& b){return a.device==b.device && a.stream==b.stream && a.allocator==b.allocator && a.flags==b.flags && a.resource==b.resource && native_command_binding::same(a.owner,b.owner);}
    static bool same(const Metadata& a,const Metadata& b){return a.chunk==b.chunk && a.vtable==b.vtable && a.wrapper==b.wrapper && a.resource==b.resource && a.cpu==b.cpu && a.gpu_getter==b.gpu_getter && a.cpu_getter==b.cpu_getter && a.frame==b.frame && a.slot==b.slot;}
    uint32_t current()const{if(!m_active)return not_armed;if(m_now()>=m_until)return expired;if(m_calls>=CALL_LIMIT || m_validation_exhausted || m_cheap_reads>CHEAP_READ_LIMIT)return exhausted;return none;}
    bool arm(const Request& request){
        if(request.pid!=GetCurrentProcessId() || !request.duration || request.duration>5000)return false;
        try{auto read=m_read;Bounded<Reader> memory{read};const auto context=source(request,memory);std::vector<uintptr_t> candidates;
            // This is only a cheap filter for unrelated streams, never ownership
            // evidence. A chunk allocated after pre-arm requires a new request.
            for(unsigned frame=0;frame<2;++frame){const auto last=memory.template at<int32_t>(context.allocator+frame*0x2c0+0x280);if(last< -1 || last>=int32_t(CHUNKS_PER_FRAME))return false;
                for(int32_t i=0;i<=last;++i){const auto chunk=memory.ptr(context.allocator+frame*0x2c0+0x40+i*8);if(std::ranges::find(candidates,chunk)==candidates.end())candidates.push_back(chunk);}}
            if(candidates.empty())return false;memory.recheck();std::lock_guard lock{m_mutex};
            m_records.reserve(RECORD_LIMIT);m_candidates=std::move(candidates);m_request=request;m_context=context;m_records.clear();++m_generation;m_sequence=0;m_calls=0;m_cheap_reads=0;m_validations=0;m_validation_exhausted=false;m_until=m_now()+std::chrono::milliseconds(request.duration);m_active=true;return true;
        }catch(...){return false;}
    }
    void disarm(){std::lock_guard lock{m_mutex};m_active=false;++m_generation;m_records.clear();}
    bool cheap(const Pending& p){
        // Known entries still get pre/post identity checks on EVERY observed
        // original call. No owner traversal/other engine locks on this path.
        {std::lock_guard lock{m_mutex};if(current() || p.generation!=m_generation)return false;
            if(m_cheap_reads>CHEAP_READ_LIMIT-CHEAP_RESERVATION){m_validation_exhausted=true;return false;}m_cheap_reads+=CHEAP_RESERVATION;}
        try{
            auto read=m_read;Bounded<Reader> memory{read};const auto& a=p.context;const auto& b=p.meta;
            if(memory.ptr(m_base+game_build::rva(0x4ff0c08))!=a.device || memory.ptr(a.device+0x398+p.request.thread*8)!=a.stream ||
               memory.ptr(a.stream+0x28)!=a.allocator || memory.template at<uint8_t>(a.stream+0x10)!=a.flags || memory.ptr(p.request.instance+0x250)!=a.resource)return false;
            const auto frame=memory.template at<uint32_t>(a.allocator+0x5c0);if(frame!=b.frame)return false;
            const auto last=memory.template at<int32_t>(a.allocator+frame*0x2c0+0x280);if(last<0 || last>=int32_t(CHUNKS_PER_FRAME) || b.slot>unsigned(last))return false;
            return memory.template at<uintptr_t>(a.allocator+frame*0x2c0+0x40+b.slot*8)==b.chunk &&
                memory.ptr(b.chunk)==b.vtable && memory.ptr(b.vtable+8)==b.gpu_getter && memory.ptr(b.vtable+16)==b.cpu_getter &&
                memory.ptr(b.chunk+0x18)==b.wrapper && memory.ptr(b.wrapper+0x28)==b.resource && memory.ptr(b.chunk+0x30)==b.cpu;
        }catch(...){return false;}
    }
    void poison(const Pending& p){std::lock_guard lock{m_mutex};if(p.generation==m_generation)for(auto& r:m_records)if(r.meta.chunk==p.meta.chunk)r.poison=true;}
    Pending before(uintptr_t chunk,uintptr_t flags){
        Pending p;
        {std::lock_guard lock{m_mutex};if(current() || (flags&1) || uint8_t(flags)!=m_context.flags || std::ranges::find(m_candidates,chunk)==m_candidates.end())return p;++m_calls;p.request=m_request;p.context=m_context;p.generation=m_generation;
            const auto found=std::ranges::find(m_records,chunk,[](const Record& r){return r.meta.chunk;});if(found!=m_records.end()){if(found->poison)return {};p.meta=found->meta;p.known=true;}
            else {if(m_validations>=VALIDATION_LIMIT || m_records.size()>=RECORD_LIMIT){m_validation_exhausted=true;return {};}++m_validations;}}
        if(p.known){p.valid=cheap(p);if(!p.valid)poison(p);return p;}
        try{auto read=m_read;Bounded<Reader> memory{read};const auto context=source(p.request,memory);if(!same(context,p.context))return {};
            p.meta=metadata(chunk,context,p.request.thread,memory,true);memory.recheck();p.valid=true;return p;}catch(...){return {};}
    }
    void after(const Pending& p,uint64_t result){
        if(!p.valid)return;
        if(p.known){const bool stable=cheap(p);std::lock_guard lock{m_mutex};if(current() || m_generation!=p.generation)return;for(auto& r:m_records)if(r.meta.chunk==p.meta.chunk && (!stable || r.gpu!=result))r.poison=true;return;}
        bool valid=false;
        try{auto read=m_read;Bounded<Reader> memory{read};const auto context=source(p.request,memory);const auto meta=metadata(p.meta.chunk,context,p.request.thread,memory,true);memory.recheck();valid=result && same(context,p.context) && same(meta,p.meta);}catch(...){}
        std::lock_guard lock{m_mutex};if(current() || m_generation!=p.generation)return;
        const auto found=std::ranges::find(m_records,p.meta.chunk,[](const Record& r){return r.meta.chunk;});
        if(found!=m_records.end()){if(!valid || !same(found->meta,p.meta) || found->gpu!=result)found->poison=true;return;}
        if(!valid)return;if(m_records.size()>=RECORD_LIMIT){m_validation_exhausted=true;return;}
        if(std::ranges::any_of(m_records,[&](const Record& r){return r.gpu==result;})){m_validation_exhausted=true;return;}
        m_records.push_back({p.meta,result,++m_sequence,false});
    }
    static uintptr_t hook(uintptr_t chunk,uintptr_t flags){
        native_command_binding::PreserveError error;const auto incoming=error.value;Pending pending;
        try{pending=get().before(chunk,flags);}catch(...){}SetLastError(incoming);
        uintptr_t result{};try{result=get().m_original(chunk,flags);}catch(...){error.value=GetLastError();throw;}error.value=GetLastError();
        try{get().after(pending,result);}catch(...){}return result;
    }
    bool install_game(){
        if(m_original)return true;if(!m_image.valid)return false;
        constexpr game_build::Code gpu=game_build::hashed(0x2100620,25,"3cabca49948ede316d305df3f541369d1f1bf1e5a0f8e879d822d600ddecf932");
        constexpr game_build::Code cpu=game_build::hashed(0x2100640,16,"e7553574e6a8b64f9c1795ce36e3c1c10a15e53bc7a2579792ae4e6225a5a6ad");
        std::array<uint8_t,25> a{};std::array<uint8_t,16> b{};if(!m_read(m_base+game_build::rva(0x2100620),a.data(),a.size()) || !game_build::code(0x2100620,a,gpu) || !m_read(m_base+game_build::rva(0x2100640),b.data(),b.size()) || !game_build::code(0x2100640,b,cpu))return false;
        return install(reinterpret_cast<void*>(m_base+game_build::rva(0x2100620)));
    }
    bool install(void* target){
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&m_original))!=MH_OK)return false;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_original=nullptr;return false;
    }
    Getter m_original{};uintptr_t m_base{};native_command_binding::Image m_image;std::filesystem::path m_directory;
    Reader m_read=rpm;Now m_now=Clock::now;std::mutex m_mutex,m_poll;Request m_request;Context m_context;std::vector<Record> m_records;
    std::vector<std::string> m_seen;std::vector<uintptr_t> m_candidates;Clock::time_point m_until{},m_next{};uint64_t m_generation{},m_sequence{},m_calls{},m_cheap_reads{};unsigned m_validations{};bool m_active{},m_validation_exhausted{};
};
