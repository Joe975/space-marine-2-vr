#pragma once
#include "NativeCallCensus.h"
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <intrin.h>
#include <mutex>

// Bounded observation and opt-in same-job exposure sharing for native sharpening.
class NativeOrdinarySharpen {
public:
    using Scope=ordinary_scene_scope::Scope;
    using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,float,uint8_t,uint16_t,float);
    using Provider=std::function<std::optional<Scope>()>;
    struct Eye {uint32_t calls{},strength{},exposure{},invoked_exposure{};bool shared{};uintptr_t source{},destination{};};
    struct Record {uint64_t dispatch{};std::array<Eye,2> eyes;std::optional<Scope> left;DWORD thread{};};
    std::function<void(const Scope&,uintptr_t,uint32_t)> boundary;
    static NativeOrdinarySharpen& get(){static auto* p=new NativeOrdinarySharpen;return *p;}
    bool install(void* target,Provider provider,uintptr_t caller=0,bool share=false,bool continuous=false){
        if(m_original || !target || !provider)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        void* original{};if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),&original)!=MH_OK)return false;
        m_original=reinterpret_cast<Native>(original);m_provider=std::move(provider);m_caller=caller;m_share=share;m_continuous=continuous;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_original=nullptr;return false;
    }
    bool install_in_game(uintptr_t base){
        constexpr game_build::Code expected=game_build::hashed(0x1f90b20,15,"46e3e2157aa3b96e825c6d255aa8f8ddd42434b97aeeae42cbd9416e9639b01f");
        constexpr game_build::Code call=game_build::hashed(0x180a47e,5,"e3cd433d99dd15e0476ccd41e06919b14975f2fcf1e056fb00f3e3c6ebe5735c");std::array<uint8_t,15> bytes{};std::array<uint8_t,5> site{};
        if(!ordinary_scene_scope::read(base+game_build::rva(0x1f90b20),bytes) || !game_build::code(0x1f90b20,bytes,expected) || !ordinary_scene_scope::read(base+game_build::rva(0x180a47e),site) || !game_build::code(0x180a47e,site,call))return false;
        wchar_t flag[8]{};const bool share=GetEnvironmentVariableW(L"SM2VR_ORDINARY_SHARED_SHARPEN_EXPOSURE",flag,8)==1 && flag[0]==L'1';
        wchar_t runtime[8]{};const bool continuous=GetEnvironmentVariableW(L"SM2VR_ORDINARY_HEADSET",runtime,8)==1 && runtime[0]==L'1';
        return install(reinterpret_cast<void*>(base+game_build::rva(0x1f90b20)),[]()->std::optional<Scope>{const auto s=NativeRenderProbe::current_scene();const auto p=NativeRenderProbe::current_ordinary_pair();if(!s || !p)return {};return Scope{*s,*p};},base+game_build::rva(0x180a483),share,continuous);
    }
    static std::optional<float> shared_exposure(const Scope& left,float value,const Scope& right){
        if(left.scene.slot!=0 || right.scene.slot!=1 || !std::isfinite(value) || value<=0)return {};
        auto normalized=right;normalized.scene.slot=0;normalized.scene.ordinal=1;normalized.scene.entry=right.pair.left.entry;
        if(!ordinary_scene_scope::same(left,normalized))return {};return value;
    }
    void write(std::ostream& out,uint64_t dispatch){std::lock_guard lock{m_mutex};out<<"{\"installed\":"<<(m_original?"true":"false")<<",\"eyes\":[";const auto r=std::ranges::find(m_records,dispatch,&Record::dispatch);for(size_t i=0;i<2;++i){if(i)out<<',';const auto e=r==m_records.end()?Eye{}:r->eyes[i];out<<"{\"calls\":"<<e.calls<<",\"strength_bits\":"<<e.strength<<",\"exposure_bits\":"<<e.exposure<<",\"invoked_exposure_bits\":"<<e.invoked_exposure<<",\"shared\":"<<(e.shared?"true":"false")<<",\"source\":"<<e.source<<",\"destination\":"<<e.destination<<'}';}out<<"]}";}
private:
    std::optional<Scope> admit(uintptr_t caller,uintptr_t source,uintptr_t destination,float strength,float& exposure){
        if(m_caller && caller!=m_caller)return {};const auto scope=m_provider();if(!scope || ordinary_scene_scope::inspect(*scope).mask)return {};
        std::lock_guard lock{m_mutex};auto r=std::ranges::find(m_records,scope->scene.dispatch_sequence,&Record::dispatch);
        if(r==m_records.end()){
            if(!m_records.empty() && scope->scene.dispatch_sequence<=m_records.back().dispatch)return {};
            if(m_records.size()>=4){if(!m_continuous)return {};m_records.erase(m_records.begin());}
            m_records.push_back({scope->scene.dispatch_sequence});r=std::prev(m_records.end());}
        auto& e=r->eyes[scope->scene.slot];if(e.calls)return {};++e.calls;e.source=source;e.destination=destination;std::memcpy(&e.strength,&strength,4);std::memcpy(&e.exposure,&exposure,4);
        if(!scope->scene.slot){r->left=*scope;r->thread=GetCurrentThreadId();}
        else if(m_share && r->left && r->thread==GetCurrentThreadId()){
            float value{};std::memcpy(&value,&r->eyes[0].exposure,4);if(const auto shared=shared_exposure(*r->left,value,*scope)){exposure=*shared;e.shared=true;}
        }
        std::memcpy(&e.invoked_exposure,&exposure,4);return scope;
    }
    static void on_call(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,float e,uint8_t f,uint16_t g,float h){
        NativeCallCensus::get().count(NativeCallCensus::Hook::sharpen);
        auto& self=get();std::optional<Scope> scope;
        try{scope=self.admit(reinterpret_cast<uintptr_t>(_ReturnAddress()),a,b,e,h);if(scope && self.boundary)self.boundary(*scope,a,9);}catch(...){}
        self.m_original(a,b,c,d,e,f,g,h);
        try{if(scope && self.boundary)self.boundary(*scope,b,10);}catch(...){}
    }
    Native m_original{};Provider m_provider;uintptr_t m_caller{};bool m_share{},m_continuous{};std::mutex m_mutex;std::vector<Record> m_records;
};
