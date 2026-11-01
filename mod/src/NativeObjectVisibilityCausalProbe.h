#pragma once
#include "NativeOrdinarySceneScope.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <charconv>

// Explicitly requested, self-expiring causal experiment. This is not a renderer
// visibility policy: keys must come from this process's passive receipt. It only
// replaces zero at the authenticated mesh caller, in the right Scene, when the
// actual matching left call in the same dispatch returned a positive value.
class NativeObjectVisibilityCausalProbe {
public:
    using Clock=std::chrono::steady_clock;
    using Now=std::function<Clock::time_point()>;
    struct PreserveError {DWORD value{GetLastError()};~PreserveError(){SetLastError(value);}};
    static NativeObjectVisibilityCausalProbe& get(){static auto* value=new NativeObjectVisibilityCausalProbe;return *value;}
    static bool enabled(){return get().m_caller!=0;}
    bool configure(const std::filesystem::path& directory,uintptr_t exact_mesh_caller,Now now=[] {return Clock::now();}){
        if(m_caller || !directory.is_absolute() || !std::filesystem::is_directory(directory) || !exact_mesh_caller || !now)return false;
        m_directory=directory;m_caller=exact_mesh_caller;m_now=std::move(now);return true;
    }
    class Scope {
    public:
        Scope(const std::optional<NativeRenderProbe::Scene>& scene,const std::optional<NativeCameraEntries::OrdinaryPair>& pair):m_previous{s_current}{
            s_current=nullptr;auto& probe=get();if(!probe.m_caller || !scene || !pair)return;PreserveError error;
            try{
                ordinary_scene_scope::Scope owned{*scene,*pair};if(ordinary_scene_scope::inspect(owned).mask&~uint32_t(16))return;
                DWORD expected{};const auto thread=GetCurrentThreadId();probe.m_thread.compare_exchange_strong(expected,thread);
                if(probe.m_thread.load()!=thread)return;
                if(scene->slot==0){
                    probe.poll();probe.m_frame.reset();if(!probe.live())return;
                    probe.m_frame=Frame{owned};
                }
                if(!probe.live() || !probe.m_frame || !same_dispatch(probe.m_frame->left,owned) ||
                   (scene->slot==1 && !probe.m_frame->left_complete))return;
                m_owned=owned;s_current=this;
            }catch(...) {s_current=nullptr;}
        }
        ~Scope(){s_current=m_previous;}
        void finish(){
            PreserveError error;if(!m_owned)return;auto& probe=get();
            try{if(m_owned->scene.slot==0 && probe.m_frame)probe.m_frame->left_complete=ordinary_scene_scope::inspect(*m_owned).mask==0;
                if(m_owned->scene.slot==1 && probe.m_log)probe.m_log.flush();
            }catch(...) {if(probe.m_frame)probe.m_frame->left_complete=false;}
        }
    private:friend class NativeObjectVisibilityCausalProbe;
        Scope* m_previous{};std::optional<ordinary_scene_scope::Scope> m_owned;
    };
    // The successful live receipt has native selectors0/1 in the two main
    // Scenes. This causal experiment supports only that verified context pair;
    // foreign contexts remain native even when nested under a main Scene.
    class Selection {
    public:
        explicit Selection(uintptr_t view):m_previous{s_selection}{
            s_selection=nullptr;if(!enabled() || !s_current || !s_current->m_owned)return;PreserveError error;
            uint8_t context{};if(!ordinary_scene_scope::read(view,context) || context!=s_current->m_owned->scene.slot)return;
            m_view=view;m_context=context;s_selection=this;
        }
        ~Selection(){s_selection=m_previous;}
    private:friend class NativeObjectVisibilityCausalProbe;
        Selection* m_previous{};uintptr_t m_view{};uint8_t m_context{};
    };
    // Called AFTER exactly one native query. Never issues a native query itself.
    static int32_t query(uintptr_t manager,uintptr_t object,int column,int32_t native,uintptr_t caller)noexcept{
        auto& probe=get();if(!probe.m_caller || caller!=probe.m_caller || !s_current || !s_selection)return native;PreserveError error;
        try{return probe.consider(manager,object,column,native,caller);}catch(...) {return native;}
    }
private:
    struct Left {uintptr_t manager{},object{},caller{};int column{},result{};bool ambiguous{};};
    struct Frame {ordinary_scene_scope::Scope left;bool left_complete{};std::vector<Left> calls;};
    static bool safe_id(const std::string& id){return !id.empty() && id.size()<=40 && std::ranges::all_of(id,[](unsigned char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';});}
    static bool number(const std::string& token,uint64_t& value){const auto r=std::from_chars(token.data(),token.data()+token.size(),value);return r.ec==std::errc{} && r.ptr==token.data()+token.size();}
    static bool same_dispatch(const ordinary_scene_scope::Scope& a,const ordinary_scene_scope::Scope& b){
        // Compare the complete owned pair and dispatch while allowing the two
        // actual Scene eye entries/ordinals to differ.
        auto copy=b;copy.scene=a.scene;return ordinary_scene_scope::same(a,copy) &&
            a.scene.dispatch_sequence==b.scene.dispatch_sequence && a.scene.list==b.scene.list && a.scene.renderer==b.scene.renderer &&
            a.scene.camera_revision_before==b.scene.camera_revision_before && a.scene.render_counter_before==b.scene.render_counter_before;
    }
    bool live(){return !m_id.empty() && m_log.good() && m_events<2048 && m_now()<m_until;}
    void poll(){
        const auto now=m_now();if(!m_id.empty() && now>=m_until){m_log<<"{\"event\":\"expired\",\"changes\":"<<m_changes<<"}\n";m_log.close();m_id.clear();m_keys.clear();}
        if(!m_id.empty())return;
        if(now<m_next || m_seen.size()>=8)return;m_next=now+std::chrono::milliseconds(250);
        std::ifstream file{m_directory/"object_visibility_causal_request.txt",std::ios::binary};std::string line;
        for(char ch;line.size()<=512 && file.get(ch) && ch!='\n';)line.push_back(ch);
        if(line.size()>512)return;std::istringstream input{line};std::string id,pid_token,duration_token,token;uint64_t pid{},duration{};
        if(!(input>>id>>pid_token>>duration_token) || !safe_id(id) || std::ranges::find(m_seen,id)!=m_seen.end() ||
           !number(pid_token,pid) || pid!=GetCurrentProcessId() || !number(duration_token,duration) || !duration || duration>5000)return;
        std::vector<uintptr_t> keys;
        while(input>>token){uint64_t key{};if(keys.size()==8 || !number(token,key) || !key || (key&7) || key>UINTPTR_MAX ||
                std::ranges::find(keys,uintptr_t(key))!=keys.end())return;keys.push_back(uintptr_t(key));}
        if(keys.empty())return;const auto output=m_directory/(id+".object_visibility_causal.jsonl");if(std::filesystem::exists(output))return;
        // Every syntactically accepted request is one-shot, including an I/O
        // failure. Rewriting its file cannot silently re-arm the experiment.
        m_seen.push_back(id);if(m_log.is_open())m_log.close();m_id.clear();m_keys.clear();
        m_log.open(output,std::ios::out);if(!m_log.good())return;
        m_id=id;m_keys=std::move(keys);m_until=now+std::chrono::milliseconds(duration);m_events=0;m_changes=0;
        m_log<<"{\"event\":\"accepted\",\"pid\":"<<pid<<",\"id\":\""<<id<<"\",\"duration_ms\":"<<duration<<",\"caller\":"<<m_caller<<",\"keys\":[";
        for(size_t i=0;i<m_keys.size();++i){if(i)m_log<<',';m_log<<m_keys[i];}m_log<<"]}\n";m_log.flush();
    }
    int32_t consider(uintptr_t manager,uintptr_t object,int column,int32_t native,uintptr_t caller){
        if(!live() || !m_frame || !s_current->m_owned || std::ranges::find(m_keys,object)==m_keys.end())return native;
        const auto& scope=*s_current->m_owned;const auto& scene=scope.scene;
        const char* reason="native";int32_t result=native;std::optional<Left> left;
        uint8_t native_context{};
        if(!manager || !object || column<0 || column>7 || !same_dispatch(m_frame->left,scope) || ordinary_scene_scope::inspect(scope).mask ||
           !ordinary_scene_scope::read(s_selection->m_view,native_context) || native_context!=s_selection->m_context || native_context!=scene.slot ||
           column!=int(native_context)*2)reason="scope_invalid"; // exact1f79d7c caller contract, not a global column convention
        else if(scene.slot==0){
            auto found=std::ranges::find(m_frame->calls,object,&Left::object);
            if(found==m_frame->calls.end())m_frame->calls.push_back({manager,object,caller,column,native,false});
            else if(found->manager!=manager || found->caller!=caller || found->column!=column || found->result!=native)found->ambiguous=true;
            reason="left_actual";
        }else if(scene.slot!=1)reason="wrong_eye";
        else if(native!=0)reason="right_not_zero";
        else if(!m_frame->left_complete)reason="left_incomplete";
        else {
            const auto found=std::ranges::find(m_frame->calls,object,&Left::object);
            if(found==m_frame->calls.end())reason="left_missing";
            else {left=*found;
                if(found->ambiguous || found->manager!=manager || found->caller!=caller || found->column==column)reason="left_mismatch";
                else if(found->result<=0)reason="left_not_positive";
                else {result=found->result;reason="qualified";}
            }
        }
        if(m_now()>=m_until){result=native;reason="expired";}
        ++m_events;m_log<<"{\"event\":\"query\",\"dispatch\":"<<scene.dispatch_sequence<<",\"render_id\":"<<scope.pair.left.render_id
            <<",\"revision\":"<<scope.pair.left.revision<<",\"list\":"<<scene.list<<",\"renderer\":"<<scene.renderer<<",\"eye\":"<<scene.slot
            <<",\"manager\":"<<manager<<",\"object\":"<<object<<",\"caller\":"<<caller<<",\"native_context\":"<<unsigned(native_context)<<",\"column\":"<<column<<",\"original\":"<<native
            <<",\"result\":"<<result<<",\"qualification\":\""<<reason<<"\",\"left_column\":"<<(left?left->column:-1)<<",\"left_result\":"<<(left?left->result:-1)<<"}\n";
        if(!m_log.good())return native;if(result!=native)++m_changes;return result;
    }
    uintptr_t m_caller{};std::filesystem::path m_directory;Now m_now;std::ofstream m_log;std::string m_id;
    std::vector<std::string> m_seen;std::vector<uintptr_t> m_keys;Clock::time_point m_until{},m_next{};size_t m_events{},m_changes{};std::optional<Frame> m_frame;
    std::atomic<DWORD> m_thread{};
    inline static thread_local Scope* s_current{};
    inline static thread_local Selection* s_selection{};
};
