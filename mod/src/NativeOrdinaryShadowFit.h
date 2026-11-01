#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <intrin.h>
#include <mutex>

// Read-only receipts at the actual directional fit boundary. Shared cache
// allocation does not itself prove its depth is sampled with the wrong transform.
class NativeOrdinaryShadowFit {
public:
    using Scope=ordinary_scene_scope::Scope;
    using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
    using Provider=std::function<std::optional<Scope>()>;
    struct Evidence {
        uint64_t dispatch{};unsigned eye{};int16_t camera{-1},cache_camera{-1};
        uintptr_t light{},sub{},shadow{};std::array<float,16> matrix{},cascade{};
        bool complete{};
    };
    static NativeOrdinaryShadowFit& get(){static auto* p=new NativeOrdinaryShadowFit;return *p;}
    static std::optional<Evidence> identify(const Scope& scope,uintptr_t owner,uintptr_t light,uintptr_t view,uintptr_t shadow,uintptr_t sub){
        using ordinary_scene_scope::read;
        if(!owner || owner>UINTPTR_MAX-0x21000 || ordinary_scene_scope::inspect(scope,0).mask)return {};
        unsigned eye{};
        if(view==scope.pair.left.entry)eye=0;else if(view==scope.pair.right.entry)eye=1;else return {};
        const auto block=owner+0xff60+eye*0x3ed0;int32_t count{},kind{},selected{};int16_t index{};
        if(!read(block,count) || count<1 || count>4 || light<block+0x10 ||
           (light-block-0x10)%0xfb0 || (light-block-0x10)/0xfb0>=size_t(count) ||
           !read(light+0xa4,kind) || kind!=1 || !read(light+0xfac,selected) || selected<0 || selected>7 ||
           sub!=light+0x100+size_t(selected)*0x1c0 || !read(sub+0x1b2,index) || index<2 || index>=64 ||
           shadow!=scope.scene.list+0x19c+size_t(index)*0xa60)return {};
        Evidence e;e.dispatch=scope.scene.dispatch_sequence;e.eye=eye;e.camera=index;e.light=light;e.sub=sub;e.shadow=shadow;
        return e;
    }
    bool install(void* target,Provider provider,std::function<uintptr_t()> owner,uintptr_t caller=0){
        if(m_native || !target || !provider || !owner)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        m_provider=std::move(provider);m_owner=std::move(owner);m_caller=caller;
        if(MH_EnableHook(target)==MH_OK)return true;
        MH_RemoveHook(target);m_native=nullptr;return false;
    }
    bool install_in_game(uintptr_t base){
        constexpr game_build::Code expected=game_build::hashed(0x1f7f0f0,16,"ab9c3a02261fcc96fd840d335c51bf4f4c923352fa55555eb409f530f838f1f3");
        std::array<uint8_t,16> bytes{};std::array<uint8_t,5> call{};
        if(!ordinary_scene_scope::read(base+game_build::rva(0x1f7f0f0),bytes) || !game_build::code(0x1f7f0f0,bytes,expected) ||
           !ordinary_scene_scope::read(base+game_build::rva(0x1f86f27),call) || !game_build::code(0x1f86f27,call,game_build::hashed(0x1f86f27,5,"df10fdbed664b14630c10d21aef0f6bd7b8913d8125163c7ab84e069418768c2")))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(0x1f7f0f0)),[]()->std::optional<Scope>{
            const auto s=NativeRenderProbe::current_scene();const auto p=NativeRenderProbe::current_ordinary_pair();
            if(!s || !p)return {};return Scope{*s,*p};
        },[base]{uintptr_t owner{};ordinary_scene_scope::read(base+game_build::rva(0x4faef88),owner);return owner;},base+game_build::rva(0x1f86f2c));
    }
    std::array<Evidence,2> evidence()const{std::lock_guard lock{m_mutex};return m_evidence;}
    std::array<uint64_t,6> stats()const{std::lock_guard lock{m_mutex};return m_counts;}
private:
    static void on_call(uintptr_t light,uintptr_t view,uintptr_t bounds,uintptr_t points,uintptr_t shadow,uintptr_t sub){
        auto& self=get();const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
        {std::lock_guard lock{self.m_mutex};++self.m_counts[0];self.m_counts[5]=caller;}
        std::optional<Scope> scope;std::optional<Evidence> receipt;uintptr_t owner{};
        try{if(!self.m_caller || caller==self.m_caller){
            scope=self.m_provider();owner=self.m_owner();
            {std::lock_guard lock{self.m_mutex};++self.m_counts[1];if(scope){++self.m_counts[2];if(!ordinary_scene_scope::inspect(*scope,0).mask)++self.m_counts[3];}}
            if(scope && scope->scene.dispatch_sequence%120==0)receipt=identify(*scope,owner,light,view,shadow,sub);
            if(receipt){std::lock_guard lock{self.m_mutex};++self.m_counts[4];}
        }}catch(...){}
        self.m_native(light,view,bounds,points,shadow,sub);
        try{if(receipt){
            const auto after=self.m_provider();
            if(!after || !ordinary_scene_scope::same(*scope,*after) || owner!=self.m_owner() ||
               !identify(*after,owner,light,view,shadow,sub))return;
            receipt->complete=ordinary_scene_scope::read(shadow+0x15c,receipt->matrix) &&
                ordinary_scene_scope::read(sub+0xd0,receipt->cascade) && ordinary_scene_scope::read(owner+0x1fc52,receipt->cache_camera);
            std::lock_guard lock{self.m_mutex};self.m_evidence[receipt->eye]=*receipt;
        }}catch(...){}
    }
    Native m_native{};Provider m_provider;std::function<uintptr_t()> m_owner;uintptr_t m_caller{};
    mutable std::mutex m_mutex;std::array<Evidence,2> m_evidence{};std::array<uint64_t,6> m_counts{};
};
