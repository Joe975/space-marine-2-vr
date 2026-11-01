#pragma once
#include "NativeGeometryIdentity.h"
#include "GameBuild.h"
#include "NativeConsoleTargets.h"
#include <atomic>
#include <array>
#include <algorithm>
#include <optional>
#include <vector>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>

// Explicit passive cross-thread census. A native view/camera ID is DATA, never
// inherited Scene ownership. No native arguments, queries or results change.
class NativeConsoleDrawCensus {
public:
    using Clock=std::chrono::steady_clock;using Now=std::function<Clock::time_point()>;
    struct PreserveError {DWORD value{GetLastError()};~PreserveError(){SetLastError(value);}};
    static NativeConsoleDrawCensus& get(){static auto* value=new NativeConsoleDrawCensus;return *value;}
    bool configure(const std::filesystem::path& directory,Now now=Clock::now,uintptr_t base=0){
        std::lock_guard lock{m_mutex};if(m_configured || !directory.is_absolute() || !std::filesystem::is_directory(directory) || !now)return false;
        if(base){IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
            if(!ordinary_scene_scope::read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x1000 || !ordinary_scene_scope::read(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage<0x1000 || nt.OptionalHeader.SizeOfImage>0x40000000 || base>UINTPTR_MAX-nt.OptionalHeader.SizeOfImage)return false;
            m_base=base;m_image_size=nt.OptionalHeader.SizeOfImage;
            constexpr game_build::Code signature=game_build::hashed(0x213d690,16,"736024b87904fa939bde5ab29175957ae70a04c3bda9d391e59387db3fa81c33");std::array<uint8_t,16> actual{};
            m_indexed_backend=m_image_size>0x213d6a0 && ordinary_scene_scope::read(base+game_build::rva(0x213d690),actual) && game_build::code(0x213d690,actual,signature);
            m_targets_backend=m_indexed_backend && NativeConsoleTargets::supported(base,m_image_size,[](uintptr_t p,void* out,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,size,&n) && n==size;});
        }
        m_directory=directory;m_now=std::move(now);m_configured=true;return true;
    }
    void poll()noexcept{PreserveError error;try{std::lock_guard lock{m_mutex};if(!m_configured)return;const auto now=m_now();
        if(m_active && (now>=m_until || !m_collecting)){m_active=false;m_collecting=false;save(m_exhausted?"work_bound":"expired");}
        if(m_active || now<m_next || m_seen.size()>=8)return;m_next=now+std::chrono::milliseconds(250);
        std::ifstream file{m_directory/"console_draw_census_request.txt",std::ios::binary};std::array<char,1025> raw{};file.read(raw.data(),raw.size());if(file.gcount()>1024)return;
        std::istringstream in{std::string{raw.data(),size_t(file.gcount())}};std::string id,pid_text,duration_text,token;uint64_t pid{},duration{};
        if(!(in>>id>>pid_text>>duration_text) || !safe(id) || !number(pid_text,pid) || pid!=GetCurrentProcessId() ||
            !number(duration_text,duration) || !duration || duration>5000 || std::ranges::find(m_seen,id)!=m_seen.end())return;
        std::vector<uint64_t> handles;bool classes{};while(in>>token){if(token=="classes"){if(classes || !handles.empty() || in>>token)return;classes=true;break;}
            uint64_t handle{};if(handles.size()>=32 || !number(token,handle) || !handle || static_cast<int64_t>(handle)<0 || (handle&7) || (handle&0xffffffffffffULL)<0x10000 || std::ranges::find(handles,handle)!=handles.end())return;handles.push_back(handle);}
        if((handles.empty() && !classes) || std::filesystem::exists(m_directory/(id+".console_draw_census.json")))return;
        m_seen.push_back(id);m_id=id;m_handles=std::move(handles);m_class_scope=classes;m_discovered.clear();m_discovered.reserve(32);++m_generation;m_started=now;m_until=now+std::chrono::milliseconds(duration);
        m_contexts.clear();m_selections.clear();m_draws.clear();m_queries.clear();m_reads=m_bytes=m_scanned=m_validations=m_exhausted=m_rejected=m_dedup_skips=m_record_drops=m_scope_limit_rejections=0;m_read_time={};m_draw_serial=0;
        m_active=true;m_collecting=true;save("accepted");
    }catch(...) {}}
    class Selection {
    public:
        Selection(uintptr_t array,int start,int count,uintptr_t view,uintptr_t caller,bool scene_tls)noexcept
            :m_previous{s_selection}{s_selection=this;begin(array,start,count,view,caller,scene_tls,0);}
        ~Selection(){finish();s_selection=m_previous;}
    private:
        friend class NativeConsoleDrawCensus;
        friend struct ObjectVisibilityProbeTests;
        friend class Draw;
        struct Direct{};
        Selection(Direct,uintptr_t view,uintptr_t descriptor,uintptr_t caller,bool scene_tls)noexcept
            :m_previous{s_selection}{s_selection=this;begin(0,0,1,view,caller,scene_tls,descriptor);}
        void begin(uintptr_t array,int start,int count,uintptr_t view,uintptr_t caller,bool scene_tls,uintptr_t direct)noexcept{
            auto& self=get();if(!self.m_collecting)return;PreserveError error;try{std::lock_guard lock{self.m_mutex};
                if(!self.live())return;m_generation=self.m_generation;m_view=view;
                if(!self.read(view,m_raw) || start<0 || count<0 || start>100000 || count>100000-start)return;
                const auto now=self.m_now();const auto existing=std::ranges::find_if(self.m_contexts,[&](const auto& row){return
                    row.thread==GetCurrentThreadId() && row.caller==caller && row.raw==m_raw && row.array==array && row.start==start && row.count==count && row.descriptor==direct;});
                if(existing!=self.m_contexts.end()){
                    ++existing->calls;existing->last_seen=now;
                    if(now-existing->last_sample<std::chrono::seconds(1)){++self.m_dedup_skips;return;}
                    existing->last_sample=now;++existing->sample_attempts;
                }else{
                    if(self.m_contexts.size()>=128){++self.m_record_drops;return;}
                    self.m_contexts.push_back({GetCurrentThreadId(),caller,array,direct,start,count,m_raw,now,now,now,1,1});
                }
                if(self.m_selections.size()>=64){++self.m_record_drops;return;}
                uintptr_t data{};if(!direct && (!self.read(array,data) || !data || data>UINTPTR_MAX-size_t(start+count)*8))return;
                for(int i=0;i<count && self.live();++i){
                    if(self.m_scanned>=8192){self.exhaust();break;}++self.m_scanned;uintptr_t descriptor=direct;
                    if(!direct && !self.read(data+size_t(start+i)*8,descriptor))continue;
                    if(auto part=self.part(descriptor)){m_parts.push_back(std::move(*part));if(m_parts.size()>=32)break;}
                }
                if(m_parts.empty() || !self.live())return;
                m_index=self.m_selections.size();self.m_selections.push_back({GetCurrentThreadId(),caller,view,array,start,count,scene_tls,bool(direct),m_raw,{},{},false,false,now,now,1});
                auto& selected=self.m_selections[m_index];selected.parts=m_parts;
                m_recorded=true;s_selection=this;
            }catch(...) {}}
        void finish()noexcept{if(!m_recorded)return;PreserveError error;auto& self=get();try{std::lock_guard lock{self.m_mutex};
            if(!self.current(m_generation) || m_index>=self.m_selections.size())return;
            std::array<uint32_t,6> after{};bool stable=self.read(m_view,after) && after==m_raw;
            for(const auto& p:m_parts){const auto again=self.part(p.descriptor);stable=bool(again) && again->handle==p.handle && again->object==p.object &&
                again->identity.kind_flags==p.identity.kind_flags && again->identity.instance==p.identity.instance && again->identity.type_link==p.identity.type_link && again->identity.name_address==p.identity.name_address && again->identity.properties==p.identity.properties && again->identity.weak==p.identity.weak &&
                again->type==p.type && again->flags==p.flags && again->identity.class_owner==p.identity.class_owner && again->identity.class_name==p.identity.class_name && stable;}
            auto& row=self.m_selections[m_index];row.after=after;row.completed=true;row.stable=stable;
        }catch(...) {}}
        Selection* m_previous{};uint64_t m_generation{};uintptr_t m_view{};size_t m_index{};bool m_recorded{};
        std::array<uint32_t,6> m_raw{};
        struct Part {uintptr_t descriptor{},object{};uint64_t handle{};uint16_t flags{};uint8_t type{};native_geometry_identity::Evidence identity;};
        std::vector<Part> m_parts;
    };
    class Draw {
    public:
        Draw(uintptr_t command,uintptr_t view,uintptr_t descriptor,uintptr_t caller,bool scene_tls)noexcept{
            if(!get().m_collecting)return;PreserveError error;try{
                if(!s_selection)m_direct=std::unique_ptr<Selection>{new Selection{Selection::Direct{},view,descriptor,caller,scene_tls}};
                auto* selected=s_selection;if(!selected || !selected->m_recorded)return;auto& self=get();std::lock_guard lock{self.m_mutex};
                if(!self.current(selected->m_generation))return;
                const auto found=std::ranges::find(selected->m_parts,descriptor,&Selection::Part::descriptor);if(found==selected->m_parts.end())return;
                const auto prior=std::ranges::find_if(self.m_draws,[&](const auto& row){return row.selection==selected->m_index && row.thread==GetCurrentThreadId() && row.caller==caller && row.handle==found->handle && row.frontier.command==command;});
                if(prior!=self.m_draws.end()){++prior->calls;prior->last=self.m_now();m_index=size_t(prior-self.m_draws.begin());}
                else if(self.m_draws.size()<128){m_index=self.m_draws.size();self.m_draws.push_back({selected->m_index,GetCurrentThreadId(),caller,descriptor,found->object,found->handle,view,1,self.m_now(),self.m_now()});}
                else {++self.m_record_drops;return;}
                m_generation=selected->m_generation;m_recorded=true;auto& frontier=self.m_draws[m_index].frontier;frontier=Frontier{};frontier.command=command;frontier.call_serial=++self.m_draw_serial;m_call_serial=frontier.call_serial;
                if(command && self.read(command,frontier.vtable) && self.m_base && frontier.vtable>=self.m_base && frontier.vtable<=self.m_base+self.m_image_size-0xb8 && self.read(frontier.vtable+0xb0,frontier.callback) && frontier.callback>=self.m_base && frontier.callback<self.m_base+self.m_image_size){
                    frontier.mapping_read=true;frontier.callback_rva=frontier.callback-self.m_base;frontier.arguments_read=self.read(descriptor+0xd8,frontier.arguments);
                    if(frontier.callback_rva==game_build::rva(0x213d690) && self.m_indexed_backend && command<=UINTPTR_MAX-0x10a34){frontier.counter_read=self.read(command+0x10a2c,frontier.counters_before);
                        frontier.targets.supported=self.m_targets_backend;
                        if(self.m_targets_backend){const auto fresh=self.part(descriptor);std::array<uint32_t,6> raw{};frontier.targets.identity_before=fresh && same_part(*fresh,*found) && self.read(view,raw) && raw==selected->m_raw;
                            if(frontier.targets.identity_before){m_part=*fresh;m_raw=raw;frontier.targets.before=NativeConsoleTargets::capture(command,[&](uintptr_t p,void* out,size_t size){return self.raw(p,out,size);});}}
                    }
                }
            }catch(...) {}}
        ~Draw(){if(!m_recorded)return;PreserveError error;auto& self=get();try{std::lock_guard lock{self.m_mutex};if(!self.current(m_generation) || m_index>=self.m_draws.size())return;
            auto& row=self.m_draws[m_index];auto& f=row.frontier;if(f.call_serial!=m_call_serial)return;uintptr_t vt{},callback{};
            f.mapping_stable=f.mapping_read && self.read(f.command,vt) && vt==f.vtable && self.read(vt+0xb0,callback) && callback==f.callback;
            if(f.mapping_stable){std::array<uint8_t,32> args{};f.arguments_stable=f.arguments_read && self.read(row.descriptor+0xd8,args) && args==f.arguments;
                if(f.counter_read)f.counter_after_read=self.read(f.command+0x10a2c,f.counters_after);
                if(f.targets.supported && f.targets.identity_before && m_part){const auto fresh=self.part(row.descriptor);std::array<uint32_t,6> raw{};
                    f.targets.identity_after=fresh && same_part(*fresh,*m_part) && self.read(row.view,raw) && raw==m_raw;
                    if(f.targets.identity_after)f.targets.after=NativeConsoleTargets::capture(f.command,[&](uintptr_t p,void* out,size_t size){return self.raw(p,out,size);});
                    f.targets.same_call=self.current(m_generation) && f.targets.identity_after && f.arguments_stable;
                }}
        }catch(...) {}}
    private:std::unique_ptr<Selection> m_direct;std::optional<Selection::Part> m_part;std::array<uint32_t,6> m_raw{};uint64_t m_generation{},m_call_serial{};size_t m_index{};bool m_recorded{};
    };
    static void query(uintptr_t manager,uintptr_t object,int column,int32_t result,uintptr_t caller)noexcept{
        auto& self=get();if(!self.m_collecting || !s_selection)return;PreserveError error;try{std::lock_guard lock{self.m_mutex};auto& selection=*s_selection;
            if(!selection.m_recorded || !self.current(selection.m_generation))return;
            const auto found=std::ranges::find(selection.m_parts,object,&Selection::Part::object);if(found==selection.m_parts.end())return;
            const auto prior=std::ranges::find_if(self.m_queries,[&](const auto& row){return row.selection==selection.m_index && row.thread==GetCurrentThreadId() && row.caller==caller && row.manager==manager && row.object==object && row.handle==found->handle && row.column==column && row.result==result;});
            if(prior!=self.m_queries.end()){++prior->calls;prior->last=self.m_now();}
            else if(self.m_queries.size()<256)self.m_queries.push_back({selection.m_index,GetCurrentThreadId(),caller,manager,object,found->handle,column,result,1,self.m_now(),self.m_now()});
            else ++self.m_record_drops;
        }catch(...) {}}
private:
    friend struct ObjectVisibilityProbeTests;
    using Part=Selection::Part;
    struct Selected {DWORD thread{};uintptr_t caller{},view{},array{};int start{},count{};bool scene_tls{},direct{};
        std::array<uint32_t,6> before{},after{};std::vector<Part> parts;bool completed{},stable{};Clock::time_point last_sample{},last_seen{};unsigned raw_calls{1};};
    struct RawContext {DWORD thread{};uintptr_t caller{},array{},descriptor{};int start{},count{};std::array<uint32_t,6> raw{};Clock::time_point first{},last_seen{},last_sample{};unsigned calls{},sample_attempts{};};
    static bool same_part(const Part& a,const Part& b){return a.descriptor==b.descriptor && a.object==b.object && a.handle==b.handle && a.flags==b.flags && a.type==b.type && a.identity.kind_flags==b.identity.kind_flags && a.identity.instance==b.identity.instance && a.identity.properties==b.identity.properties && a.identity.type_link==b.identity.type_link && a.identity.class_owner==b.identity.class_owner && a.identity.name_address==b.identity.name_address && a.identity.weak==b.identity.weak && a.identity.class_name==b.identity.class_name;}
    struct Frontier {uint64_t call_serial{};NativeConsoleTargets::Pair targets;uintptr_t command{},vtable{},callback{},callback_rva{};bool mapping_read{},mapping_stable{},arguments_read{},arguments_stable{},counter_read{},counter_after_read{};std::array<uint8_t,32> arguments{};std::array<uint32_t,2> counters_before{},counters_after{};};
    struct Drawn {size_t selection{};DWORD thread{};uintptr_t caller{},descriptor{},object{};uint64_t handle{};uintptr_t view{};unsigned calls{1};Clock::time_point first{},last{};Frontier frontier;};
    struct Queried {size_t selection{};DWORD thread{};uintptr_t caller{},manager{},object{};uint64_t handle{};int column{},result{};unsigned calls{1};Clock::time_point first{},last{};};
    static bool safe(const std::string& id){return !id.empty() && id.size()<=40 && std::ranges::all_of(id,[](unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';});}
    static bool number(const std::string& s,uint64_t& value){const auto r=std::from_chars(s.data(),s.data()+s.size(),value);return r.ec==std::errc{} && r.ptr==s.data()+s.size();}
    bool live()const{return m_active && m_collecting && m_now()<m_until;}
    bool current(uint64_t generation)const{return live() && m_generation==generation;}
    void exhaust(){++m_exhausted;m_collecting=false;}
    bool raw(uintptr_t address,void* target,size_t size){
        if(!live())return false;if(m_reads>=16384 || size>512 || m_bytes+size>1048576 || m_read_time>=std::chrono::milliseconds(20)){exhaust();return false;}
        if(address<0x10000 || address>UINTPTR_MAX-size)return false;++m_reads;m_bytes+=unsigned(size);SIZE_T copied{};
        const auto start=Clock::now();const bool ok=ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),target,size,&copied) && copied==size;
        m_read_time+=Clock::now()-start;return ok;
    }
    template<class T> bool read(uintptr_t address,T& out){return raw(address,&out,sizeof(out));}
    std::optional<Part> part(uintptr_t descriptor){
        uint64_t handle{};if(descriptor>UINTPTR_MAX-0x198 || !read(descriptor+0x100,handle) || (!m_class_scope && std::ranges::find(m_handles,handle)==m_handles.end()))return {};
        if(m_validations>=256){exhaust();return {};}++m_validations;
        auto identity=native_geometry_identity::capture(descriptor,[&](uintptr_t p,void* target,size_t size){return raw(p,target,size);});
        const bool wanted=!identity.mask && identity.class_chain_stable && identity.weak_roundtrip &&
            (identity.class_name=="test_scene_mod__usable_button_interplay__x__floor_placement__actor" || identity.class_name=="test_scene_mod__usable_button_interplay__x__button__actor");
        Part value;value.descriptor=descriptor;value.handle=handle;value.identity=std::move(identity);
        if(!wanted || value.identity.handle!=handle || !read(descriptor+0xf8,value.object) || !read(descriptor+0x194,value.flags) || !read(descriptor+0x60,value.type)){++m_rejected;return {};}
        if(!handle || static_cast<int64_t>(handle)<0 || (handle&7) || (handle&0xffffffffffffULL)<0x10000)return {};
        if(m_class_scope && std::ranges::find(m_discovered,handle)==m_discovered.end()){
            if(m_discovered.size()>=32){++m_scope_limit_rejections;return {};}m_discovered.push_back(handle);
        }
        value.object&=~uintptr_t(7);return value;
    }
    double ms(Clock::time_point time)const{return std::chrono::duration<double,std::milli>{time-m_started}.count();}
    void save(const char* outcome){
        std::ofstream out{m_directory/(m_id+".console_draw_census.json")};out<<"{\"model\":\"console_draw_census_v1\",\"id\":\""<<m_id<<"\",\"pid\":"<<GetCurrentProcessId()<<",\"outcome\":\""<<outcome<<"\",\"complete_census\":false,\"dispatch_join_verified\":false,\"gpu_execution_verified\":false,\"native_calls_added\":false,\"memory_writes\":false,\"reads\":"<<m_reads<<",\"bytes_read\":"<<m_bytes<<",\"read_time_ms\":"<<std::chrono::duration<double,std::milli>{m_read_time}.count()<<",\"scanned\":"<<m_scanned<<",\"identity_validations\":"<<m_validations<<",\"exhausted\":"<<m_exhausted<<",\"rejected\":"<<m_rejected<<",\"scope\":\""<<(m_class_scope?"exact_console_classes":"explicit_handles")<<"\",\"scope_limit_rejections\":"<<m_scope_limit_rejections<<",\"class_seen_before_selection_finish\":true,\"class_seen_handles\":[";for(size_t i=0;i<m_discovered.size();++i){if(i)out<<',';out<<m_discovered[i];}out<<"],\"dedup_unsampled_calls\":"<<m_dedup_skips<<",\"record_drops\":"<<m_record_drops<<",\"raw_contexts\":[";
        for(size_t i=0;i<m_contexts.size();++i){if(i)out<<',';const auto& c=m_contexts[i];out<<"{\"thread\":"<<c.thread<<",\"caller\":"<<c.caller<<",\"array\":"<<c.array<<",\"descriptor\":"<<c.descriptor<<",\"start\":"<<c.start<<",\"count\":"<<c.count<<",\"calls\":"<<c.calls<<",\"sample_attempts\":"<<c.sample_attempts<<",\"first_ms\":"<<ms(c.first)<<",\"last_seen_ms\":"<<ms(c.last_seen)<<",\"raw_view\":[";for(size_t j=0;j<6;++j){if(j)out<<',';out<<c.raw[j];}out<<"]}";}out<<"],\"selections\":[";
        for(size_t i=0;i<m_selections.size();++i){if(i)out<<',';const auto& s=m_selections[i];out<<"{\"index\":"<<i<<",\"thread\":"<<s.thread<<",\"caller\":"<<s.caller<<",\"view\":"<<s.view<<",\"array\":"<<s.array<<",\"start\":"<<s.start<<",\"count\":"<<s.count<<",\"scene_tls_present\":"<<(s.scene_tls?"true":"false")<<",\"direct_draw\":"<<(s.direct?"true":"false")<<",\"completed\":"<<(s.completed?"true":"false")<<",\"sample_stable\":"<<(s.stable?"true":"false")<<",\"native_camera_id\":"<<s.before[5]<<",\"sample_ms\":"<<ms(s.last_sample)<<",\"last_raw_seen_ms\":"<<ms(s.last_seen)<<",\"raw_calls\":"<<s.raw_calls<<",\"raw_view_before\":[";
            for(size_t j=0;j<6;++j){if(j)out<<',';out<<s.before[j];}out<<"],\"raw_view_after\":[";for(size_t j=0;j<6;++j){if(j)out<<',';out<<s.after[j];}out<<"],\"parts\":[";
            for(size_t j=0;j<s.parts.size();++j){if(j)out<<',';const auto& p=s.parts[j];out<<"{\"descriptor\":"<<p.descriptor<<",\"object\":"<<p.object<<",\"handle\":"<<p.handle<<",\"flags194\":"<<p.flags<<",\"render_type\":"<<unsigned(p.type)<<",\"identity\":";native_geometry_identity::write(out,p.identity);out<<'}';}out<<"]}";}
        out<<"],\"draws\":[";for(size_t i=0;i<m_draws.size();++i){if(i)out<<',';const auto& d=m_draws[i];out<<"{\"selection\":"<<d.selection<<",\"thread\":"<<d.thread<<",\"caller\":"<<d.caller<<",\"descriptor\":"<<d.descriptor<<",\"object\":"<<d.object<<",\"handle\":"<<d.handle<<",\"view\":"<<d.view<<",\"calls\":"<<d.calls<<",\"first_ms\":"<<ms(d.first)<<",\"last_ms\":"<<ms(d.last)<<",\"command_frontier\":{\"latest_call_serial\":"<<d.frontier.call_serial<<",\"command\":"<<d.frontier.command<<",\"vtable\":"<<d.frontier.vtable<<",\"callback\":"<<d.frontier.callback<<",\"callback_rva\":"<<d.frontier.callback_rva<<",\"mapping_read\":"<<(d.frontier.mapping_read?"true":"false")<<",\"mapping_stable\":"<<(d.frontier.mapping_stable?"true":"false")<<",\"arguments_read\":"<<(d.frontier.arguments_read?"true":"false")<<",\"arguments_stable\":"<<(d.frontier.arguments_stable?"true":"false")<<",\"counter_read\":"<<(d.frontier.counter_read?"true":"false")<<",\"counter_after_read\":"<<(d.frontier.counter_after_read?"true":"false")<<",\"primitive_counter_before\":"<<d.frontier.counters_before[0]<<",\"draw_counter_before\":"<<d.frontier.counters_before[1]<<",\"primitive_counter_after\":"<<d.frontier.counters_after[0]<<",\"draw_counter_after\":"<<d.frontier.counters_after[1]<<",\"gpu_execution_verified\":false,\"raw_arguments_d8_f8\":[";for(size_t k=0;k<32;++k){if(k)out<<',';out<<unsigned(d.frontier.arguments[k]);}out<<"],\"targets\":";NativeConsoleTargets::write(out,d.frontier.targets);out<<"}}";}
        out<<"],\"queries\":[";for(size_t i=0;i<m_queries.size();++i){if(i)out<<',';const auto& q=m_queries[i];out<<"{\"selection\":"<<q.selection<<",\"thread\":"<<q.thread<<",\"caller\":"<<q.caller<<",\"manager\":"<<q.manager<<",\"object\":"<<q.object<<",\"handle\":"<<q.handle<<",\"column\":"<<q.column<<",\"result\":"<<q.result<<",\"calls\":"<<q.calls<<",\"first_ms\":"<<ms(q.first)<<",\"last_ms\":"<<ms(q.last)<<'}';}out<<"]}\n";
    }
    inline static thread_local Selection* s_selection{};
    std::filesystem::path m_directory;Now m_now;uintptr_t m_base{};size_t m_image_size{};bool m_indexed_backend{},m_targets_backend{};std::mutex m_mutex;bool m_configured{},m_active{},m_class_scope{};std::atomic_bool m_collecting{};
    Clock::time_point m_started{},m_until{},m_next{};Clock::duration m_read_time{};uint64_t m_generation{},m_draw_serial{};std::string m_id;
    std::vector<std::string> m_seen;std::vector<uint64_t> m_handles,m_discovered;std::vector<RawContext> m_contexts;std::vector<Selected> m_selections;std::vector<Drawn> m_draws;std::vector<Queried> m_queries;
    unsigned m_reads{},m_bytes{},m_scanned{},m_validations{},m_exhausted{},m_rejected{},m_dedup_skips{},m_record_drops{},m_scope_limit_rejections{};
};
