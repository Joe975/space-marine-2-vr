#pragma once
#include "NativeGeometryIdentity.h"
#include "GameBuild.h"
#include "NativeOrdinarySceneScope.h"
#include <optional>
#include <string>
#include <cstdint>
#include <atomic>
#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <vector>

// Time-window causal experiment on one boolean-only native selector consumer.
// Raw pass context is not Scene/eye/job ownership. No partner query or cache.
class NativeConsoleQueryAdmissionProbe {
public:
    using Clock=std::chrono::steady_clock;using Now=std::function<Clock::time_point()>;
    struct PreserveError {DWORD value{GetLastError()};~PreserveError(){SetLastError(value);}};
    static NativeConsoleQueryAdmissionProbe& get(){static auto* value=new NativeConsoleQueryAdmissionProbe;return *value;}
    bool configure(const std::filesystem::path& directory,uintptr_t caller,Now now=Clock::now){
        std::lock_guard lock{m_mutex};if(m_caller || !caller || !directory.is_absolute() || !std::filesystem::is_directory(directory) || !now)return false;
        m_directory=directory;m_caller=caller;m_now=std::move(now);return true;
    }
    bool configure_in_game(const std::filesystem::path& directory,uintptr_t base){
        // call2093860; xor edx,edx; test eax,eax; je skip. Returned value has
        // only boolean consumption here, never a resource/index interpretation.
        constexpr game_build::Code expected=game_build::hashed(0x1f79d77,15,"00c9fc21e62b21c674371420b4263070cd7b1a89cb9496eb262d2732ce26a248");
        std::array<uint8_t,15> actual{};if(!ordinary_scene_scope::read(base+game_build::rva(0x1f79d77),actual) || !game_build::code(0x1f79d77,actual,expected))return false;
        return configure(directory,base+game_build::rva(0x1f79d7c));
    }
    void poll()noexcept{PreserveError error;try{std::lock_guard lock{m_mutex};if(!m_caller)return;const auto now=m_now();
        if(m_pending && (!m_active || now>=m_until)){m_pending=false;m_active=false;save(m_exhausted?"work_bound":"expired");}
        if(m_pending || now<m_next || m_seen.size()>=8)return;m_next=now+std::chrono::milliseconds(250);
        std::ifstream file{m_directory/"console_query_admission_request.txt",std::ios::binary};std::array<char,1025> raw{};file.read(raw.data(),raw.size());if(file.gcount()>1024)return;
        std::istringstream in{std::string{raw.data(),size_t(file.gcount())}};std::string id,pid_text,duration_text,token;uint64_t pid{},duration{};
        if(!(in>>id>>pid_text>>duration_text) || !safe(id) || !number(pid_text,pid) || pid!=GetCurrentProcessId() || !number(duration_text,duration) || !duration || duration>5000 || std::ranges::find(m_seen,id)!=m_seen.end())return;
        std::vector<uint64_t> handles;bool classes{};while(in>>token){if(token=="classes"){if(classes || !handles.empty() || in>>token)return;classes=true;break;}
            uint64_t handle{};if(handles.size()>=32 || !number(token,handle) || !handle || static_cast<int64_t>(handle)<0 || (handle&7) || (handle&0xffffffffffffULL)<0x10000 || std::ranges::find(handles,handle)!=handles.end())return;handles.push_back(handle);}
        if((handles.empty() && !classes) || std::filesystem::exists(m_directory/(id+".console_query_admission.json")))return;
        m_seen.push_back(id);m_id=id;m_handles=std::move(handles);m_class_scope=classes;m_discovered.clear();m_discovered.reserve(32);++m_generation;m_started=now;m_until=now+std::chrono::milliseconds(duration);
        m_rows.clear();m_reads=m_bytes=m_attempts=m_changes=m_rejected=m_exhausted=m_record_drops=m_scope_limit_rejections=0;m_read_time={};m_bookkeeping_failed=0;m_pending=true;m_active=true;save("accepted");
    }catch(...) {}}
    class Selection {
    public:
        Selection(uintptr_t iterator,uintptr_t view,uintptr_t caller)noexcept:m_previous{s_selection},m_iterator{iterator},m_view{view},m_caller{caller}{s_selection=this;}
        ~Selection(){s_selection=m_previous;}
    private:friend class NativeConsoleQueryAdmissionProbe;Selection* m_previous{};uintptr_t m_iterator{},m_view{},m_caller{};
    };
    class Query {
    public:
        Query(uintptr_t manager,uintptr_t object,int column,uintptr_t caller)noexcept{
            PreserveError error;NativeConsoleQueryAdmissionProbe* observed{};
            try{auto& self=get();observed=&self;if(!self.m_active || !s_selection || caller!=self.m_caller || !manager || object<0x10000 || (object&7))return;
                std::lock_guard lock{self.m_mutex};if(!self.live())return;
                m_generation=self.m_generation;m_iterator=s_selection->m_iterator;m_view=s_selection->m_view;m_selector_caller=s_selection->m_caller;
                if(!self.read(m_view,m_raw) || !pass(m_raw,column))return;
                if(self.m_attempts>=4096){self.exhaust();return;}++self.m_attempts;
                uintptr_t descriptor{};if(m_iterator>UINTPTR_MAX-0x10 || !self.read(m_iterator+0x10,descriptor))return;
                auto identity=self.part(descriptor,object);if(!identity){++self.m_rejected;return;}
                m_part=std::move(*identity);m_manager=manager;m_object=object;m_column=column;m_caller=caller;m_prepared=true;
            }catch(...){if(observed)++observed->m_bookkeeping_failed;}}
        int32_t finish(int32_t native)noexcept{
            if(!m_prepared)return native;PreserveError error;auto& self=get();try{std::lock_guard lock{self.m_mutex};
                if(!self.current(m_generation))return native;
                uintptr_t descriptor{};std::array<uint32_t,6> raw{};
                if(!self.read(m_view,raw) || raw!=m_raw || !self.read(m_iterator+0x10,descriptor) || descriptor!=m_part.descriptor){++self.m_rejected;return native;}
                const auto again=self.part(descriptor,m_object);
                if(!again || !same(m_part,*again) || !self.current(m_generation)){++self.m_rejected;return native;}
                const bool discover=self.m_class_scope && std::ranges::find(self.m_discovered,m_part.handle)==self.m_discovered.end();
                if(discover && self.m_discovered.size()>=32){++self.m_scope_limit_rejections;return native;}
                const bool change=native==0;const auto now=self.m_now();
                const auto prior=std::ranges::find_if(self.m_rows,[&](const auto& row){return row.thread==GetCurrentThreadId() && row.raw==m_raw && row.handle==m_part.handle && row.object==m_object && row.manager==m_manager && row.changed==change;});
                const bool inserted=prior==self.m_rows.end();
                if(inserted){
                    if(self.m_rows.size()>=256){++self.m_record_drops;return native;}
                    Row row;row.thread=GetCurrentThreadId();row.raw=m_raw;row.handle=m_part.handle;row.object=m_object;row.manager=m_manager;row.caller=m_caller;row.selector_caller=m_selector_caller;row.column=m_column;row.changed=change;row.first=row.last=now;row.first_native=row.last_native=native;row.first_descriptor=row.last_descriptor=descriptor;row.identity=m_part.identity;self.m_rows.push_back(std::move(row));
                }
                // Record allocation may pause; expire immediately before the
                // nonallocating publication/result change and undo a new row.
                if(!self.current(m_generation)){if(inserted)self.m_rows.pop_back();return native;}
                if(discover)self.m_discovered.push_back(m_part.handle); // Capacity32 reserved at arm; nonallocating publication.
                if(!inserted){++prior->calls;prior->last=now;prior->last_native=native;prior->last_descriptor=descriptor;}
                if(change)++self.m_changes;return change?1:native;
            }catch(...){++self.m_bookkeeping_failed;return native;}}
    private:
        friend struct ObjectVisibilityProbeTests;
        friend class NativeConsoleQueryAdmissionProbe;
        struct Part {uintptr_t descriptor{},object{};uint64_t handle{};uint8_t type{};uint16_t flags{};native_geometry_identity::Evidence identity;};
        static bool pass(const std::array<uint32_t,6>& raw,int column){return raw[0]<=1 && raw[1]==1864 && raw[2]==raw[0]*8192 && raw[3]==1536 && raw[4]==12517440 && raw[5]==raw[0] && column==int(raw[0])*2;}
        static bool same(const Part& a,const Part& b){return a.descriptor==b.descriptor && a.object==b.object && a.handle==b.handle && a.type==b.type && a.flags==b.flags && a.identity.kind_flags==b.identity.kind_flags && a.identity.instance==b.identity.instance && a.identity.type_link==b.identity.type_link && a.identity.name_address==b.identity.name_address && a.identity.properties==b.identity.properties && a.identity.weak==b.identity.weak && a.identity.class_owner==b.identity.class_owner && a.identity.class_name==b.identity.class_name;}
        uint64_t m_generation{};uintptr_t m_iterator{},m_view{},m_selector_caller{},m_manager{},m_object{},m_caller{};int m_column{};bool m_prepared{};std::array<uint32_t,6> m_raw{};Part m_part;
    };
private:
    friend struct ObjectVisibilityProbeTests;
    using Part=Query::Part;
    struct Row {DWORD thread{};std::array<uint32_t,6> raw{};uint64_t handle{};uintptr_t object{},manager{},caller{},selector_caller{},first_descriptor{},last_descriptor{};int column{},first_native{},last_native{};bool changed{};unsigned calls{1};Clock::time_point first{},last{};native_geometry_identity::Evidence identity;};
    static bool safe(const std::string& id){return !id.empty() && id.size()<=40 && std::ranges::all_of(id,[](unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';});}
    static bool number(const std::string& text,uint64_t& value){const auto r=std::from_chars(text.data(),text.data()+text.size(),value);return r.ec==std::errc{} && r.ptr==text.data()+text.size();}
    bool live()const{return m_pending && m_active && m_now()<m_until;}
    bool current(uint64_t generation)const{return live() && generation==m_generation;}
    void exhaust(){++m_exhausted;m_active=false;}
    bool raw(uintptr_t address,void* target,size_t size){if(!live())return false;
        if(m_reads>=262144 || size>512 || m_bytes+size>8388608 || m_read_time>=std::chrono::milliseconds(100)){exhaust();return false;}
        if(address<0x10000 || address>UINTPTR_MAX-size)return false;++m_reads;m_bytes+=unsigned(size);SIZE_T copied{};
        const auto start=Clock::now();const bool ok=ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),target,size,&copied) && copied==size;m_read_time+=Clock::now()-start;return ok;
    }
    template<class T> bool read(uintptr_t address,T& value){return raw(address,&value,sizeof(value));}
    std::optional<Part> part(uintptr_t descriptor,uintptr_t object){
        Part value;value.descriptor=descriptor;if(descriptor>UINTPTR_MAX-0x198 || !read(descriptor+0x100,value.handle) || (!m_class_scope && std::ranges::find(m_handles,value.handle)==m_handles.end()) || !read(descriptor+0xf8,value.object) || (value.object&~uintptr_t(7))!=object || !read(descriptor+0x60,value.type) || value.type!=56 || !read(descriptor+0x194,value.flags) || !(value.flags&1))return {};
        if(!value.handle || static_cast<int64_t>(value.handle)<0 || (value.handle&7) || (value.handle&0xffffffffffffULL)<0x10000)return {};
        value.object&=~uintptr_t(7);value.identity=native_geometry_identity::capture(descriptor,[&](uintptr_t p,void* out,size_t size){return raw(p,out,size);});const auto& e=value.identity;
        if(e.mask || e.handle!=value.handle || !e.class_chain_stable || !e.weak_roundtrip || (e.class_name!="test_scene_mod__usable_button_interplay__x__floor_placement__actor" && e.class_name!="test_scene_mod__usable_button_interplay__x__button__actor"))return {};return value;
    }
    double ms(Clock::time_point time)const{return std::chrono::duration<double,std::milli>{time-m_started}.count();}
    void save(const char* outcome){std::ofstream out{m_directory/(m_id+".console_query_admission.json")};
        out<<"{\"model\":\"console_pass_query_admission_v1\",\"id\":\""<<m_id<<"\",\"pid\":"<<GetCurrentProcessId()<<",\"outcome\":\""<<outcome<<"\",\"dispatch_join_verified\":false,\"owned_eye_verified\":false,\"gpu_execution_verified\":false,\"native_calls_added\":false,\"memory_writes\":false,\"boolean_consumer_caller\":"<<m_caller<<",\"scope\":\""<<(m_class_scope?"exact_console_classes":"explicit_handles")<<"\",\"scope_limit_rejections\":"<<m_scope_limit_rejections<<",\"discovered_handles\":[";for(size_t i=0;i<m_discovered.size();++i){if(i)out<<',';out<<m_discovered[i];}out<<"],\"changes\":"<<m_changes<<",\"attempts\":"<<m_attempts<<",\"reads\":"<<m_reads<<",\"bytes_read\":"<<m_bytes<<",\"read_time_ms\":"<<std::chrono::duration<double,std::milli>{m_read_time}.count()<<",\"rejected\":"<<m_rejected<<",\"exhausted\":"<<m_exhausted<<",\"record_drops\":"<<m_record_drops<<",\"bookkeeping_failures\":"<<m_bookkeeping_failed.load()<<",\"records\":[";
        for(size_t i=0;i<m_rows.size();++i){if(i)out<<',';const auto& r=m_rows[i];out<<"{\"thread\":"<<r.thread<<",\"handle\":"<<r.handle<<",\"object\":"<<r.object<<",\"manager\":"<<r.manager<<",\"caller\":"<<r.caller<<",\"selector_caller\":"<<r.selector_caller<<",\"column\":"<<r.column<<",\"render_type\":56,\"native_query_flag_required\":true,\"first_descriptor\":"<<r.first_descriptor<<",\"last_descriptor\":"<<r.last_descriptor<<",\"calls\":"<<r.calls<<",\"changed\":"<<(r.changed?"true":"false")<<",\"first_native\":"<<r.first_native<<",\"last_native\":"<<r.last_native<<",\"effective_if_zero\":1,\"first_ms\":"<<ms(r.first)<<",\"last_ms\":"<<ms(r.last)<<",\"raw_view\":[";for(size_t j=0;j<6;++j){if(j)out<<',';out<<r.raw[j];}out<<"],\"identity\":";native_geometry_identity::write(out,r.identity);out<<'}';}out<<"]}\n";
    }
    inline static thread_local Selection* s_selection{};
    std::filesystem::path m_directory;Now m_now;uintptr_t m_caller{};std::mutex m_mutex;bool m_pending{},m_class_scope{};std::atomic_bool m_active{};std::atomic_uint m_bookkeeping_failed{};
    Clock::time_point m_started{},m_until{},m_next{};Clock::duration m_read_time{};uint64_t m_generation{};std::string m_id;std::vector<std::string> m_seen;std::vector<uint64_t> m_handles,m_discovered;std::vector<Row> m_rows;
    unsigned m_reads{},m_bytes{},m_attempts{},m_changes{},m_rejected{},m_exhausted{},m_record_drops{},m_scope_limit_rejections{};
};
