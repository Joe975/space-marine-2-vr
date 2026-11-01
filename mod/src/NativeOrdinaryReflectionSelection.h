#pragma once
#include "NativeCallCensus.h"
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <filesystem>
#include <fstream>
#include <mutex>

// Reflection lighting has a second PVS filter after the volume visibility mask.
// Build a private union of those two filters; keep the real eye and its matrices.
class NativeOrdinaryReflectionSelection {
public:
    using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
    using Resolve=uintptr_t(*)(uintptr_t);
    using Scope=ordinary_scene_scope::Scope;
    using Provider=std::function<std::optional<Scope>()>;
    struct Pvs {uint64_t mask{};bool active{true};};
    struct Inventory {uintptr_t records{};int32_t count{};uint8_t eye{};};
    static std::optional<Inventory> identify(const Scope& scope,uintptr_t owner,uintptr_t view){
        using ordinary_scene_scope::read;
        uintptr_t expected{};Inventory out;
        if(ordinary_scene_scope::inspect(scope).mask || view!=scope.scene.entry || !owner ||
           !read(scope.scene.renderer+0x668,expected) || expected!=owner ||
           !read(owner+0xa8,out.records) || !out.records || !read(owner+0xb0,out.count) || out.count<0 || out.count>256 ||
           out.records>UINTPTR_MAX-size_t(out.count)*0x148 || !read(view+0x33,out.eye) || out.eye!=scope.scene.slot)return {};
        return out;
    }
    struct Selection {
        alignas(16) std::array<uint8_t,0xc0> owner{};
        std::vector<uint8_t> records;
        std::vector<uint8_t> order;
        std::array<uintptr_t,20> partition{};
        std::array<uint64_t,4> mask{3,0,0,0};
        uint64_t different{};
        uintptr_t address(){const auto p=reinterpret_cast<uintptr_t>(records.data()),q=reinterpret_cast<uintptr_t>(order.data());std::memcpy(owner.data()+0xa8,&p,8);std::memcpy(owner.data()+0xb8,&q,8);return reinterpret_cast<uintptr_t>(owner.data());}
    };
    static NativeOrdinaryReflectionSelection& get(){static auto* p=new NativeOrdinaryReflectionSelection;return *p;}
    static std::optional<Selection> prepare(const Scope& scope,uintptr_t owner,Resolve resolve,uint64_t fallback,bool use_pvs=true){
        using ordinary_scene_scope::read;
        const auto inv=identify(scope,owner,scope.scene.entry);
        if(!inv || inv->count>256 || !resolve)return {};
        Selection out;out.records.resize(size_t(inv->count)*0x148);out.order.resize(size_t(inv->count));
        SIZE_T n{};uintptr_t order{};
        if(!read(owner,out.owner) || !read(owner+0xb8,order) ||
           (inv->count && (!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(inv->records),out.records.data(),out.records.size(),&n) || n!=out.records.size() ||
           !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(order),out.order.data(),out.order.size(),&n) || n!=out.order.size())))return {};
        for(const auto index:out.order)if(index>=inv->count)return {};
        std::array<bool,256> visited{};
        for(const auto i:out.order){
            if(visited[i])continue;
            visited[i]=true;
            const auto record=inv->records+i*0x148;uint64_t visible{},pvs=use_pvs?fallback:3;uintptr_t bank{};int32_t index{};int8_t group{};
            uint8_t disabled{};
            // Record fields come from the copy taken above, not one system call each (MOD_REVIEW_2026-10-29).
            const auto* copied=out.records.data()+i*0x148;
            std::memcpy(&visible,copied+0x138,8);std::memcpy(&disabled,copied+0x140,1);
            // Native skips these records before touching the PVS proxy. Their
            // unused metadata may be invalid; preserve the same short circuit.
            if(disabled || !(visible&3))continue;
            const auto resolved=use_pvs?resolve(record+0x118):0;
            if(resolved){
                std::memcpy(&index,copied+0x12c,4);std::memcpy(&group,copied+0x134,1);
                if(index<0 || index>1000000 || group<0 || group>31 ||
                   resolved>UINTPTR_MAX-size_t(group+4)*0x20 ||
                   !read(resolved+size_t(group+4)*0x20,bank) || !bank || bank>UINTPTR_MAX-size_t(index)*0x20 || !read(bank+size_t(index)*0x20,pvs))return {};
            }
            const uint64_t bit=uint64_t(1)<<inv->eye;
            const bool native=(visible&pvs&bit)!=0,united=(visible&pvs&3)!=0;
            if(native!=united)++out.different;
            const auto mask=united?visible|bit:visible&~bit;
            std::memcpy(out.records.data()+i*0x148+0x138,&mask,8);
            // Only these private proxies resolve to the private all-eye mask.
            // Eligibility was evaluated above from both original filters.
            const int32_t zero=0;std::memcpy(out.records.data()+i*0x148+0x12c,&zero,4);
            out.records[i*0x148+0x134]=0;
            out.records[i*0x148+0x136]=1;
        }
        if(ordinary_scene_scope::inspect(scope).mask)return {};
        return out;
    }
    bool install(void* target,Provider provider,Resolve resolve,std::function<std::optional<Pvs>()> fallback,std::function<bool()> enabled){
        if(m_native || !target || !provider || !resolve || !fallback || !enabled)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        if(MH_CreateHook(reinterpret_cast<void*>(resolve),reinterpret_cast<void*>(&on_resolve),reinterpret_cast<void**>(&m_resolve))!=MH_OK){MH_RemoveHook(target);m_native=nullptr;return false;}
        m_provider=std::move(provider);m_fallback=std::move(fallback);m_enabled=std::move(enabled);
        if(MH_EnableHook(reinterpret_cast<void*>(resolve))==MH_OK && MH_EnableHook(target)==MH_OK)return true;
        MH_RemoveHook(reinterpret_cast<void*>(resolve));MH_RemoveHook(target);m_native=nullptr;m_resolve=nullptr;return false;
    }
    bool install_in_game(uintptr_t base,const std::filesystem::path& path){
        constexpr game_build::Code expected=game_build::hashed(0x1f8eb70,16,"983533c0799b4ca747101ebc49a67b431234ef13970c9945570e130ba8750485");
        constexpr game_build::Code resolver=game_build::hashed(0x1edeaa0,16,"81a5b8c39e7e28c231944655e77d37fa660e28f35aa897014508f0bb5e9f72b0");
        std::array<uint8_t,16> bytes{},r{};
        if((!path.empty() && !path.is_absolute()) || !ordinary_scene_scope::read(base+game_build::rva(0x1f8eb70),bytes) || !game_build::code(0x1f8eb70,bytes,expected) || !ordinary_scene_scope::read(base+game_build::rva(0x1edeaa0),r) || !game_build::code(0x1edeaa0,r,resolver))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(0x1f8eb70)),[]()->std::optional<Scope>{const auto scene=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();if(!scene || !pair)return {};return Scope{*scene,*pair};},
            reinterpret_cast<Resolve>(base+game_build::rva(0x1edeaa0)),[base]()->std::optional<Pvs>{Pvs value;uint8_t use{},skip{};
                if(!ordinary_scene_scope::read(base+game_build::rva(0x4cb4080),value.mask) || !ordinary_scene_scope::read(base+game_build::rva(0x4ca400b),use) || !ordinary_scene_scope::read(base+game_build::rva(0x4faefb0),skip))return {};
                value.active=use!=0 && skip==0;return value;},
            [path,next=std::chrono::steady_clock::time_point{},enabled=true]()mutable{const auto now=std::chrono::steady_clock::now();if(path.empty() || now<next)return enabled;next=now+std::chrono::seconds(1);std::ifstream file{path};std::string line;if(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();enabled=line!="native_reflections";}return enabled;});
    }
    std::array<uint64_t,4> stats()const{std::lock_guard lock{m_mutex};return m_counts;}
private:
    inline static thread_local Selection* s_selection{};
    static uintptr_t on_resolve(uintptr_t proxy){
        if(s_selection){
            const auto first=reinterpret_cast<uintptr_t>(s_selection->records.data());
            if(proxy>=first+0x118 && proxy<first+s_selection->records.size() && (proxy-first-0x118)%0x148==0){
                s_selection->partition[16]=reinterpret_cast<uintptr_t>(s_selection->mask.data());
                return reinterpret_cast<uintptr_t>(s_selection->partition.data());
            }
        }
        return get().m_resolve(proxy);
    }
    static void on_call(uintptr_t block,uintptr_t command,uintptr_t view,uintptr_t input,uintptr_t owner){
        NativeCallCensus::get().count(NativeCallCensus::Hook::reflection);
        auto& self=get();std::optional<Selection> selected;
        try{const auto scope=self.m_provider();std::lock_guard lock{self.m_mutex};++self.m_counts[0];
            if(scope && block==scope->scene.renderer+0x610 && view==scope->scene.entry){
                const auto pvs=self.m_fallback();
                auto candidate=pvs?prepare(*scope,owner,self.m_resolve,pvs->mask,pvs->active):std::nullopt;
                if(candidate){self.m_counts[2]+=candidate->different;
                    if(self.m_dispatch!=scope->scene.dispatch_sequence){self.m_dispatch=scope->scene.dispatch_sequence;self.m_active=self.m_enabled();}
                    if(self.m_active){selected=std::move(candidate);++self.m_counts[3];}
                }else ++self.m_counts[1];
            }else ++self.m_counts[1];
        }catch(...){}
        struct Active{Selection* previous{s_selection};Active(Selection* value){s_selection=value;}~Active(){s_selection=previous;}} active{selected?&*selected:nullptr};
        self.m_native(block,command,view,input,selected?selected->address():owner);
    }
    Native m_native{};Resolve m_resolve{};Provider m_provider;std::function<std::optional<Pvs>()> m_fallback;std::function<bool()> m_enabled;
    mutable std::mutex m_mutex;std::array<uint64_t,4> m_counts{};uint64_t m_dispatch{};bool m_active{};
};
