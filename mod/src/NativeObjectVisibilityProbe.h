#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include "NativeSceneClearProbe.h"
#include "NativeObjectVisibilityCausalProbe.h"
#include "NativeGeometryIdentity.h"
#include "NativeConsoleDrawCensus.h"
#include "NativeCommandBindingIdentity.h"
#include "NativeCommandResourceEvidence.h"
#include "OrdinaryJoinedCapture.h"
#include "NativeDrawSkinSnapshot.h"
#include "NativeConsoleQueryAdmissionProbe.h"
#include <MinHook.h>
#include <intrin.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <memory>

// Optional CPU draw-admission receipts. Never changes a native query, view mask,
// descriptor or return value. Only the authenticated Scene thread is observed;
// this is not a census of worker-side request generation or GPU execution.
class NativeObjectVisibilityProbe {
public:
    using Select=uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,int,int,uintptr_t,uint16_t);
    using Draw=void(*)(uintptr_t,uintptr_t,uintptr_t,uint64_t,uintptr_t,uint8_t);
    struct Record {uintptr_t descriptor{},object{};uint16_t flags{};uint8_t render_type{};bool render_type_read{};unsigned selected{},admitted{};std::array<uint32_t,16> primitive_prefix{};bool prefix_read{};int native_view{-1};native_geometry_identity::Evidence identity;native_command_binding::Evidence command_binding;native_command_resources::Evidence command_resources;std::shared_ptr<native_draw_skin::Evidence> skin_snapshot;};
    struct Query {uintptr_t manager{},object{},caller{};int column{},result{};unsigned calls{};};
    struct Eye {std::vector<Record> records;std::vector<Query> queries;unsigned selectors{},foreign_views{},invalid{},truncated{},identity_attempts{},identity_budget_exhausted{};bool complete{};};
    static constexpr size_t LIMIT=1024,QUERY_LIMIT=256;
    struct Frame {std::string name;uint64_t dispatch{},render_id{},revision{};uintptr_t list{},renderer{};std::array<Eye,2> eyes;bool joined{};std::shared_ptr<NativeSceneClearProbe::Session> clears;};
    struct PreserveError {DWORD value{GetLastError()};~PreserveError(){SetLastError(value);}};
    static NativeObjectVisibilityProbe& get(){static auto* value=new NativeObjectVisibilityProbe;return *value;}
    static bool enabled(){return get().m_select!=nullptr;}
    static bool active(){return s_eye!=nullptr;}
    static bool safe_name(const std::string& name){return !name.empty() && name.size()<=40 && std::ranges::all_of(name,[](unsigned char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';});}
    bool install(void* select,void* draw,const std::filesystem::path& directory){
        if(m_select || !select || !draw || !directory.is_absolute() || !std::filesystem::is_directory(directory))return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(select,reinterpret_cast<void*>(&on_select),reinterpret_cast<void**>(&m_select))!=MH_OK)return false;
        if(MH_CreateHook(draw,reinterpret_cast<void*>(&on_draw),reinterpret_cast<void**>(&m_draw))!=MH_OK){MH_RemoveHook(select);m_select=nullptr;return false;}
        m_directory=directory;
        if(MH_EnableHook(select)==MH_OK && MH_EnableHook(draw)==MH_OK)return true;
        for(auto target:{select,draw}){MH_DisableHook(target);MH_RemoveHook(target);}m_select=nullptr;m_draw=nullptr;return false;
    }
    bool install_in_game(uintptr_t base,const std::filesystem::path& directory){
        constexpr game_build::Code select=game_build::hashed(0x1f79c50,18,"d7d1f5ae578e244c6ddce6d0d5a668d1ee684e64faeb28528888695380615425");
        constexpr game_build::Code draw=game_build::hashed(0x1f79e80,17,"6491a5b681293a2a87862a5069afdb9c4fe1f07b651ef6aab361226126975f99");
        std::array<uint8_t,18> a{};std::array<uint8_t,17> b{};
        if(!ordinary_scene_scope::read(base+game_build::rva(0x1f79c50),a) || !game_build::code(0x1f79c50,a,select) || !ordinary_scene_scope::read(base+game_build::rva(0x1f79e80),b) || !game_build::code(0x1f79e80,b,draw))return false;
        m_base=base;m_binding_image=native_command_binding::inspect_image(base,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});
        const bool installed=install(reinterpret_cast<void*>(base+game_build::rva(0x1f79c50)),reinterpret_cast<void*>(base+game_build::rva(0x1f79e80)),directory);
        if(installed){NativeSceneClearProbe::get().install_in_game(base);NativeObjectVisibilityCausalProbe::get().configure(directory,base+game_build::rva(0x1f79d7c));NativeConsoleDrawCensus::get().configure(directory,NativeConsoleDrawCensus::Clock::now,base);NativeConsoleQueryAdmissionProbe::get().configure_in_game(directory,base);NativeObjectConstantGetterProbe::get().configure(base,directory);}return installed;
    }
    class Scope {
    public:
        Scope(const std::optional<NativeRenderProbe::Scene>& scene,const std::optional<NativeCameraEntries::OrdinaryPair>& pair):m_previous{s_eye}{
            PreserveError preserve_error;
            s_eye=nullptr;auto& probe=get();if(!probe.m_select || !scene || !pair)return;
            try{ordinary_scene_scope::Scope scope{*scene,*pair};
                // The original Scene selects renderer+0x140 AFTER hook entry.
                // Admit a pending scope using all other provenance checks; its
                // completion must pass the full active-entry check below.
                if(ordinary_scene_scope::inspect(scope).mask&~uint32_t(16))return;
                std::lock_guard lock{probe.m_mutex};
                if(scene->slot==0){
                    NativeConsoleDrawCensus::get().poll();NativeConsoleQueryAdmissionProbe::get().poll();
                    probe.m_frame.reset();const auto now=std::chrono::steady_clock::now();if(probe.m_count>=8 || now<probe.m_next)return;
                    probe.m_next=now+std::chrono::seconds(1);std::string name;bool joined{};
                    {std::ifstream file{probe.m_directory/L"joined_capture_request.txt",std::ios::binary};std::array<char,43> text{};file.read(text.data(),text.size());name.assign(text.data(),size_t(file.gcount()));
                        while(!name.empty() && (name.back()=='\r' || name.back()=='\n'))name.pop_back();
                        joined=safe_name(name) && name!=probe.m_last_name && !std::filesystem::exists(probe.m_directory/(name+".draw_visibility.json")) && !std::filesystem::exists(probe.m_directory/name);}
                    if(!joined){std::ifstream file{probe.m_directory/L"draw_visibility_request.txt"};if(!std::getline(file,name))return;if(!name.empty() && name.back()=='\r')name.pop_back();}
                    if(!safe_name(name) || name==probe.m_last_name || std::filesystem::exists(probe.m_directory/(name+".draw_visibility.json")))return;
                    if(joined && !OrdinaryJoinedCapture::get().reserve(name,scene->dispatch_sequence,pair->left.render_id))return;
                    probe.m_last_name=name;++probe.m_count;probe.m_frame=std::make_shared<Frame>(Frame{name,scene->dispatch_sequence,pair->left.render_id,pair->left.revision,scene->list,scene->renderer});
                    probe.m_frame->clears=NativeSceneClearProbe::get().session();
                    probe.m_frame->joined=joined;
                }
                if(!probe.m_frame || probe.m_frame->dispatch!=scene->dispatch_sequence || probe.m_frame->revision!=pair->left.revision || probe.m_frame->list!=scene->list ||
                   probe.m_frame->renderer!=scene->renderer || (scene->slot==1 && !probe.m_frame->eyes[0].complete))return;
                m_scope=scope;m_frame=probe.m_frame;s_eye=&m_frame->eyes[scene->slot];m_eye=s_eye;
                m_clear_binding=std::make_unique<NativeSceneClearProbe::Binding>(m_frame->clears,scope);m_eye->records.reserve(LIMIT);m_eye->queries.reserve(QUERY_LIMIT);
            }catch(...) {s_eye=nullptr;m_eye=nullptr;m_clear_binding.reset();m_frame.reset();m_scope.reset();}
        }
        ~Scope(){PreserveError error;s_eye=m_previous;m_clear_binding.reset();m_frame.reset();m_scope.reset();}
        void finish(){
            PreserveError preserve_error;
            if(!m_eye || !m_scope)return;try{auto& probe=get();std::lock_guard lock{probe.m_mutex};
                if(probe.m_frame!=m_frame){++m_eye->invalid;return;}
                if(ordinary_scene_scope::inspect(*m_scope).mask){++m_eye->invalid;return;}m_eye->complete=true;
                if(m_scope->scene.slot==1 && m_frame){if(m_frame->clears)m_frame->clears->finished=true;const auto& f=*m_frame;
                    if(probe.save(f) && f.joined && std::ranges::all_of(f.eyes,[](const Eye& e){return e.complete && !e.invalid && !e.truncated;}))
                        OrdinaryJoinedCapture::get().complete(f.name,f.dispatch,f.render_id);}
            }catch(...) {++m_eye->invalid;}
        }
    private:Eye* m_previous{};Eye* m_eye{};std::optional<ordinary_scene_scope::Scope> m_scope;std::shared_ptr<Frame> m_frame;std::unique_ptr<NativeSceneClearProbe::Binding> m_clear_binding;
    };
    static void query(uintptr_t manager,uintptr_t object,int column,int result,uintptr_t caller)noexcept{
        NativeConsoleDrawCensus::query(manager,object,column,result,caller);
        if(!s_eye)return;try{auto& eye=*s_eye;auto found=std::ranges::find_if(eye.queries,[&](const Query& q){return q.manager==manager && q.object==object && q.column==column && q.result==result && q.caller==caller;});
            if(found!=eye.queries.end()){++found->calls;return;}if(eye.queries.size()==QUERY_LIMIT){++eye.truncated;return;}
            eye.queries.push_back({manager,object,caller,column,result,1});
        }catch(...) {++s_eye->invalid;}
    }
    static void describe(Eye& eye,uintptr_t descriptor,bool admitted,int native_view=-1){
        PreserveError preserve_error;
        const auto found=std::ranges::find_if(eye.records,[&](const Record& record){return record.descriptor==descriptor && record.native_view==native_view;});
        if(found!=eye.records.end()){if(admitted)++found->admitted;else ++found->selected;return;}
        if(eye.records.size()==LIMIT){++eye.truncated;return;}Record record;record.descriptor=descriptor;record.native_view=native_view;
        if(!descriptor || descriptor>UINTPTR_MAX-0x198 || !ordinary_scene_scope::read(descriptor+0xf8,record.object) || !ordinary_scene_scope::read(descriptor+0x194,record.flags)){++eye.invalid;return;}
        // Optional first-observation byte; the descriptor can be pooled/reused later.
        record.render_type_read=ordinary_scene_scope::read(descriptor+0x60,record.render_type);
        record.object&=~uintptr_t(7);record.prefix_read=record.object && ordinary_scene_scope::read(record.object,record.primitive_prefix);
        if(eye.identity_attempts<64){++eye.identity_attempts;record.identity=native_geometry_identity::capture(descriptor);}
        else {++eye.identity_budget_exhausted;record.identity.mask=native_geometry_identity::budget;}
        if(admitted)record.admitted=1;else record.selected=1;eye.records.push_back(record);
    }
    static void select_observation(uintptr_t array,int start,int count,uintptr_t view)noexcept{
        if(!s_eye)return;try{auto& eye=*s_eye;++eye.selectors;uint8_t slot{};
            const auto scene=NativeRenderProbe::current_scene();
            if(!scene || !ordinary_scene_scope::read(view,slot)){++eye.invalid;return;}
            // Native context indices and query columns are not eye indices.
            // Scene provenance owns the eye; retain the native selector as data.
            if(slot!=scene->slot)++eye.foreign_views;
            uintptr_t data{};if(start<0 || count<0 || start>100000 || count>100000-start || !ordinary_scene_scope::read(array,data) || !data || data>UINTPTR_MAX-size_t(start+count)*8){++eye.invalid;return;}
            for(int i=0;i<count;++i){if(eye.records.size()==LIMIT){++eye.truncated;break;}uintptr_t descriptor{};
                if(!ordinary_scene_scope::read(data+size_t(start+i)*8,descriptor)){++eye.invalid;break;}describe(eye,descriptor,false,slot);}
        }catch(...) {++s_eye->invalid;}
    }
private:
    friend struct ObjectVisibilityProbeTests;
    using DrawToken=std::shared_ptr<native_draw_skin::Evidence>;
    static DrawToken command_observation(uintptr_t command,uintptr_t descriptor,int native_view){
        if(!s_eye)return {};
        const auto found=std::ranges::find_if(s_eye->records,[&](const Record& r){return r.descriptor==descriptor && r.native_view==native_view;});
        if(found==s_eye->records.end() || !found->admitted || found->command_binding.mask!=native_command_binding::not_requested || !native_command_binding::player(found->identity))return {};
        found->command_binding=native_command_binding::capture(command,descriptor,found->identity,get().m_binding_image,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});
        found->command_resources=native_command_resources::before(descriptor,found->identity,found->command_binding,get().m_binding_image,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});
        if(!found->command_binding.mask){found->skin_snapshot=std::make_shared<native_draw_skin::Evidence>();found->skin_snapshot->object_before=native_object_constants::capture(descriptor,found->identity,get().m_binding_image,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});found->skin_snapshot->before=native_draw_skin::capture(descriptor,found->identity,get().m_binding_image,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});}
        return found->skin_snapshot;
    }
    static void command_after(uintptr_t command,uintptr_t descriptor,int native_view,uint64_t dispatch,const DrawToken& token){
        auto& probe=get();if(!token || token->post_observed || !s_eye || !probe.m_frame || probe.m_frame->dispatch!=dispatch)return;
        const auto found=std::ranges::find_if(s_eye->records,[&](const Record& r){return r.descriptor==descriptor && r.native_view==native_view;});
        if(found==s_eye->records.end() || found->command_binding.command!=command || found->skin_snapshot!=token)return;
        native_command_resources::after(found->command_resources,command,descriptor,found->identity,found->command_binding,probe.m_binding_image,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});
        if(found->skin_snapshot){found->skin_snapshot->post_observed=true;found->skin_snapshot->object_after=native_object_constants::capture(descriptor,found->identity,probe.m_binding_image,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});found->skin_snapshot->after=native_draw_skin::capture(descriptor,found->identity,probe.m_binding_image,[](uintptr_t p,void* target,size_t size){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),target,size,&n) && n==size;});}
    }
    static uintptr_t on_select(uintptr_t a,uintptr_t b,uintptr_t c,int d,int e,uintptr_t f,uint16_t g){
        const auto incoming=GetLastError();NativeConsoleQueryAdmissionProbe::Selection admission{a,f,reinterpret_cast<uintptr_t>(_ReturnAddress())};NativeConsoleDrawCensus::Selection census{c,d,e,f,reinterpret_cast<uintptr_t>(_ReturnAddress()),NativeRenderProbe::current_scene().has_value()};NativeObjectVisibilityCausalProbe::Selection causal_context{f};select_observation(c,d,e,f);SetLastError(incoming);return get().m_select(a,b,c,d,e,f,g);
    }
    static void on_draw(uintptr_t a,uintptr_t b,uintptr_t c,uint64_t d,uintptr_t e,uint8_t f){
        struct RestoreError {DWORD value;~RestoreError(){SetLastError(value);}} restore_error{GetLastError()};
        const auto incoming=restore_error.value;NativeConsoleDrawCensus::Draw census{a,b,c,reinterpret_cast<uintptr_t>(_ReturnAddress()),NativeRenderProbe::current_scene().has_value()};int captured_view=-1;uint64_t dispatch{};DrawToken token;if(s_eye)try{uint8_t slot{};const auto scene=NativeRenderProbe::current_scene();
            if(scene && ordinary_scene_scope::read(b,slot)){describe(*s_eye,c,true,slot);token=command_observation(a,c,slot);captured_view=slot;dispatch=get().m_frame?get().m_frame->dispatch:0;}else ++s_eye->invalid;
        }catch(...) {++s_eye->invalid;}SetLastError(incoming);get().m_draw(a,b,c,d,e,f);
        const auto native_error=GetLastError();restore_error.value=native_error;if(captured_view>=0)try{command_after(a,c,captured_view,dispatch,token);}catch(...) {if(s_eye)++s_eye->invalid;}SetLastError(native_error);
    }
    bool save(const Frame& frame){
        std::ofstream out{m_directory/(frame.name+".draw_visibility.json")};
        out<<"{\"model\":\"ordinary_object_visibility_v1\",\"cpu_only\":true,\"gpu_execution_verified\":false,\"primitive_bounds_verified\":false,\"worker_generation_census\":false,\"dispatch\":"<<frame.dispatch<<",\"render_id\":"<<frame.render_id<<",\"revision\":"<<frame.revision<<",\"list\":"<<frame.list<<",\"renderer\":"<<frame.renderer<<",\"eyes\":[";
        for(size_t i=0;i<2;++i){if(i)out<<',';const auto& eye=frame.eyes[i];out<<"{\"eye\":"<<i<<",\"complete\":"<<(eye.complete?"true":"false")<<",\"selectors\":"<<eye.selectors<<",\"foreign_views\":"<<eye.foreign_views<<",\"invalid\":"<<eye.invalid<<",\"truncated\":"<<eye.truncated<<",\"identity_attempts\":"<<eye.identity_attempts<<",\"identity_budget_exhausted\":"<<eye.identity_budget_exhausted<<",\"records\":[";
            for(size_t j=0;j<eye.records.size();++j){if(j)out<<',';const auto& r=eye.records[j];out<<"{\"descriptor\":"<<r.descriptor<<",\"object\":"<<r.object<<",\"native_view\":"<<r.native_view<<",\"flags\":"<<r.flags<<",\"render_type\":"<<unsigned(r.render_type)<<",\"render_type_read\":"<<(r.render_type_read?"true":"false")<<",\"selected\":"<<r.selected<<",\"admitted\":"<<r.admitted<<",\"primitive_prefix_read\":"<<(r.prefix_read?"true":"false")<<",\"geometry_identity\":";native_geometry_identity::write(out,r.identity);out<<",\"primitive_prefix_words\":[";
                for(size_t k=0;k<r.primitive_prefix.size();++k){if(k)out<<',';out<<r.primitive_prefix[k];}out<<"],\"command_binding\":";native_command_binding::write(out,r.command_binding);out<<",\"command_resources\":";native_command_resources::write(out,r.command_resources);out<<",\"skin_snapshot\":";if(r.skin_snapshot)native_draw_skin::write(out,*r.skin_snapshot);else out<<"null";out<<'}';}
            out<<"],\"queries\":[";for(size_t j=0;j<eye.queries.size();++j){if(j)out<<',';const auto& q=eye.queries[j];out<<"{\"manager\":"<<q.manager<<",\"object\":"<<q.object<<",\"caller\":"<<q.caller<<",\"column\":"<<q.column<<",\"result\":"<<q.result<<",\"calls\":"<<q.calls<<'}';}out<<"]}";
        }out<<"],\"clear_events\":";if(frame.clears)NativeSceneClearProbe::write(out,*frame.clears);else out<<"null";out<<",\"joined_capture_requested\":"<<(frame.joined?"true":"false")<<"}\n";out.flush();return bool(out);
    }
    Select m_select{};Draw m_draw{};uintptr_t m_base{};native_command_binding::Image m_binding_image;std::filesystem::path m_directory;std::string m_last_name;unsigned m_count{};
    std::chrono::steady_clock::time_point m_next{};std::mutex m_mutex;std::shared_ptr<Frame> m_frame;
    inline static thread_local Eye* s_eye{};
};
