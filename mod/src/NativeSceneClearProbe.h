#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include "NativeCommandBindingIdentity.h"
#include <MinHook.h>
#include <memory>
#include <chrono>
#include <array>
#include <ostream>

// Passive same-thread invocation evidence. It never calls a GPU/native helper,
// changes clear arguments, or treats a depth resource as an eye's final target.
class NativeSceneClearProbe {
public:
    using Clear=void(*)(uintptr_t,uint32_t,uintptr_t);
    using Clock=std::chrono::steady_clock;
    using Now=Clock::time_point(*)();
    using Reader=bool(*)(uintptr_t,void*,size_t);
    inline static constexpr game_build::Code SIGNATURE=game_build::hashed(0x213dfe0,24,"83b21d87094cc342068e3d6fc4c6141ac633bc10bc92e16e3610a62566b5d93a");
    static constexpr size_t EVENT_LIMIT=128,READ_LIMIT=4096,BYTE_LIMIT=65536;
    enum Mask:uint32_t {unreadable=1,changed=2,unsupported=4,ownership=8,budget=16,expired=32};
    struct State {uintptr_t vtable{},wrapper{},resource{},dsv{},values{};uint32_t depth_word{};uint8_t present{},stencil{};uint32_t mask{};bool operator==(const State&)const=default;};
    struct Event {uint64_t serial{},dispatch{},ordinal{},render{},pair_render{},revision{};uint32_t slot{},flags{},thread{};uintptr_t command{},argument{},entry{},list{},renderer{};bool identity_before{},identity_after{},same_call{};State before,after;};
    struct Session {
        std::array<Event,EVENT_LIMIT> events{};size_t count{},attempts{},reads{},bytes{},unjoined{},refused{},dropped{};double rpm_ms{};
        bool installed{},exhausted{},timed_out{},finished{};Clock::time_point until{};
    };
    struct PreserveError {DWORD value{GetLastError()};~PreserveError(){SetLastError(value);}};
    static NativeSceneClearProbe& get(){static auto* p=new NativeSceneClearProbe;return *p;}
    std::shared_ptr<Session> session(){auto s=std::make_shared<Session>();s->installed=m_native!=nullptr;s->until=m_now()+std::chrono::seconds(5);return s;}
    bool install(void* target,uintptr_t vtable){
        if(m_native || !target || !vtable)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_clear),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        m_vtable=vtable;
        if(MH_EnableHook(target)==MH_OK)return true;
        MH_RemoveHook(target);m_native=nullptr;m_vtable=0;return false;
    }
    template<class Read>static bool supported(uintptr_t base,Read&& read){
        PreserveError error;
        try{const auto image=native_command_binding::inspect_image(base,read);std::array<uint8_t,24> bytes{};uintptr_t callback{};
            return image.valid && image.contains(base+game_build::rva(0x37d7d80),0x160) && image.code(base+game_build::rva(0x213dfe0)) && read(base+game_build::rva(0x213dfe0),bytes.data(),bytes.size()) && game_build::code(0x213dfe0,bytes,SIGNATURE) && read(base+game_build::rva(0x37d7d80)+0x158,&callback,8) && callback==base+game_build::rva(0x213dfe0);
        }catch(...){return false;}
    }
    bool install_in_game(uintptr_t base){
        PreserveError error;
        const auto read=[](uintptr_t p,void* out,size_t n){SIZE_T got{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,n,&got) && got==n;};
        return supported(base,read) && install(reinterpret_cast<void*>(base+game_build::rva(0x213dfe0)),base+game_build::rva(0x37d7d80));
    }
    class Binding {
    public:
        Binding(std::shared_ptr<Session> session,const ordinary_scene_scope::Scope& scope):m_previous{s_binding},m_session{std::move(session)},m_scope{scope}{s_binding=this;}
        ~Binding(){s_binding=m_previous;}
        Binding(const Binding&)=delete;Binding& operator=(const Binding&)=delete;
    private:
        friend class NativeSceneClearProbe;
        Binding* m_previous{};std::shared_ptr<Session> m_session;ordinary_scene_scope::Scope m_scope;
    };
    static void write(std::ostream& out,const Session& s){
        out<<"{\"model\":\"ordinary_scene_clear_events_v1\",\"installed\":"<<(s.installed?"true":"false")<<",\"cpu_only\":true,\"atomic\":false,\"gpu_execution_verified\":false,\"depth_eye_ownership_verified\":false,\"absence_verified\":false,\"other_threads_observed\":false,\"attempts\":"<<s.attempts<<",\"reads\":"<<s.reads<<",\"bytes\":"<<s.bytes<<",\"rpm_ms\":"<<s.rpm_ms<<",\"unjoined\":"<<s.unjoined<<",\"refused\":"<<s.refused<<",\"dropped\":"<<s.dropped<<",\"exhausted\":"<<(s.exhausted?"true":"false")<<",\"timed_out\":"<<(s.timed_out?"true":"false")<<",\"events\":[";
        for(size_t i=0;i<s.count;++i){if(i)out<<',';const auto& e=s.events[i];out<<"{\"serial\":"<<e.serial<<",\"dispatch\":"<<e.dispatch<<",\"ordinal\":"<<e.ordinal<<",\"scene_render_counter\":"<<e.render<<",\"pair_render_id\":"<<e.pair_render<<",\"revision\":"<<e.revision<<",\"slot\":"<<e.slot<<",\"thread\":"<<e.thread<<",\"command\":"<<e.command<<",\"flags\":"<<e.flags<<",\"values_argument\":"<<e.argument<<",\"entry\":"<<e.entry<<",\"list\":"<<e.list<<",\"renderer\":"<<e.renderer<<",\"identity_before\":"<<(e.identity_before?"true":"false")<<",\"identity_after\":"<<(e.identity_after?"true":"false")<<",\"same_call\":"<<(e.same_call?"true":"false")<<",\"before\":";write_state(out,e.before);out<<",\"after\":";write_state(out,e.after);out<<'}';}out<<"]}";
    }
private:
    friend struct ObjectVisibilityProbeTests;
    static bool memory(uintptr_t p,void* out,size_t n){SIZE_T got{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,n,&got) && got==n;}
    bool current(Session& s){if(s.finished)return false;if(m_now()>=s.until){s.timed_out=true;return false;}return true;}
    bool read(Session& s,uintptr_t p,void* out,size_t n){
        if(!current(s) || s.reads>=READ_LIMIT || n>BYTE_LIMIT-s.bytes || s.rpm_ms>=4.0){s.exhausted=true;return false;}
        ++s.reads;s.bytes+=n;if(!p || p>=0x800000000000ULL || n>0x800000000000ULL-p)return false;
        const auto begin=Clock::now();bool ok{};try{ok=m_reader(p,out,n);}catch(...){}s.rpm_ms+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();return ok && current(s);
    }
    template<class T>bool read(Session& s,uintptr_t p,T& out){return read(s,p,&out,sizeof(out));}
    bool identity(Session& s,const ordinary_scene_scope::Scope& expected){
        const auto actual=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();
        if(!actual || !pair || !ordinary_scene_scope::same(expected,{*actual,*pair}) || !actual->valid() || actual->slot>1 || actual->ordinal!=actual->slot+1 || (actual->view_flags_before&0x50000044))return false;
        uintptr_t active{};uint32_t counter{};uint64_t flags{};int32_t count{};
        return read(s,actual->renderer+0x140,active) && read(s,actual->list+0x29c78,counter) && read(s,actual->list+0x29968,count) && read(s,actual->entry,flags) && active==actual->entry && counter==actual->render_counter_before && count>=2 && count<=64 && flags==actual->view_flags_before;
    }
    State snapshot(Session& s,uintptr_t command,uintptr_t argument){
        State a;if(!command || command>0x800000000000ULL-0x11958){a.mask=unreadable;return a;}
        if(!read(s,command,a.vtable)){a.mask=unreadable;return a;}if(a.vtable!=m_vtable){a.mask=unsupported;return a;}
        const auto fields=[&](State& v){
            v.values=argument?argument:command+0xfe80;
            return read(s,command,v.vtable) && read(s,command+0xfdc8,v.wrapper) && read(s,command+0x1194c,v.present) && read(s,command+0x11950,v.dsv) &&
                v.values<0x800000000000ULL-0x15 && read(s,v.values+0x10,v.depth_word) && read(s,v.values+0x14,v.stencil) &&
                (!v.wrapper || (v.wrapper<0x800000000000ULL-0x280 && read(s,v.wrapper+0x278,v.resource)));
        };
        if(!fields(a)){a.mask=unreadable;return a;}if(a.vtable!=m_vtable){a.mask=unsupported;return a;}
        State b;if(!fields(b)){a.mask=unreadable;return a;}if(b.vtable!=m_vtable)a.mask=unsupported;else if(a!=b)a.mask=changed;return a;
    }
    static void write_state(std::ostream& out,const State& s){out<<"{\"mask\":"<<s.mask<<",\"vtable\":"<<s.vtable<<",\"depth_wrapper\":"<<s.wrapper<<",\"depth_resource\":"<<s.resource<<",\"dsv\":"<<s.dsv<<",\"values\":"<<s.values<<",\"depth_word\":"<<s.depth_word<<",\"stencil\":"<<unsigned(s.stencil)<<",\"depth_present\":"<<unsigned(s.present)<<'}';}
    static void on_clear(uintptr_t command,uint32_t flags,uintptr_t values){
        PreserveError restore;const auto incoming=restore.value;auto& probe=get();std::shared_ptr<Session> session;std::optional<ordinary_scene_scope::Scope> owner;size_t index{};bool reserved{};
        try{if(s_binding){session=s_binding->m_session;owner=s_binding->m_scope;
            if(session && probe.current(*session)){
                if(session->attempts>=EVENT_LIMIT){++session->dropped;session->exhausted=true;}
                else {++session->attempts;index=session->count++;reserved=true;auto& e=session->events[index];const auto& scene=owner->scene;e.serial=index+1;e.dispatch=scene.dispatch_sequence;e.ordinal=scene.ordinal;e.render=scene.render_counter_before;e.pair_render=owner->pair.left.render_id;e.revision=scene.camera_revision_before.value_or(0);e.slot=scene.slot;e.entry=scene.entry;e.list=scene.list;e.renderer=scene.renderer;e.thread=GetCurrentThreadId();e.command=command;e.flags=flags;e.argument=values;
                    e.identity_before=probe.identity(*session,*owner);if(e.identity_before)e.before=probe.snapshot(*session,command,values);else {e.before.mask=ownership;++session->unjoined;}
                }
            }
        }}catch(...){if(session)++session->refused;}
        SetLastError(incoming);try{probe.m_native(command,flags,values);}catch(...){restore.value=GetLastError();throw;}restore.value=GetLastError();
        try{if(reserved){auto& e=session->events[index];e.identity_after=probe.current(*session) && owner && probe.identity(*session,*owner);
            if(e.identity_after)e.after=probe.snapshot(*session,command,values);else e.after.mask=ownership;
            e.same_call=e.identity_before && e.identity_after && probe.current(*session);
            if(!e.same_call || e.before.mask || e.after.mask)++session->refused;
        }}catch(...){if(session)++session->refused;}
    }
    Clear m_native{};uintptr_t m_vtable{};Now m_now{Clock::now};Reader m_reader{memory};
    inline static thread_local Binding* s_binding{};
};
