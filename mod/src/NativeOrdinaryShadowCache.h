#pragma once
#include "NativeOrdinaryLightVisibility.h"
#include "GameBuild.h"
#include "PerformanceTrace.h"

// Diagnostic native refresh branch: never patch cached resources or view state.
// Preparation precedes Scene, so authenticate against registered producer eyes.
class NativeOrdinaryShadowCache {
public:
    using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,int32_t,uint8_t,uint8_t);
    using Pair=NativeCameraEntries::OrdinaryPair;
    using Provider=std::function<std::optional<Pair>(uintptr_t)>;
    static NativeOrdinaryShadowCache& get(){static auto* p=new NativeOrdinaryShadowCache;return *p;}
    // shading.txt modes that force the native cascade refit for both ordinary eyes.
    static bool refreshes(const std::string& mode){
        return mode=="light_union_shadow_refresh" || mode=="stereo_cubemaps" || mode=="native_reflections" ||
            mode=="light_union_shadow_refresh_ssr" || mode=="light_union_shadow_refresh_ssao" ||
            mode=="light_union_shadow_refresh_screen_shadows" || mode=="light_union_shadow_refresh_all" || mode=="light_union_shadow_refresh_native_volumetrics" ||
            mode=="light_union_shadow_refresh_native_shadow_lights" ||
            mode=="light_union_shadow_refresh_shared_spot_shadows";
    }
    static std::optional<Pair> pair_for_view(uintptr_t view){
        using E=NativeCameraEntries;using ordinary_scene_scope::read;
        for(const auto& state:E::get().status()){
            if(!state.valid || state.inflight || state.entries!=2 || state.list<0x160 || state.list>UINTPTR_MAX-0x2980c ||
               (view!=state.list+8 && view!=state.list+8+0xa60))continue;
            int32_t count{};if(!read(state.list+0x29808,count) || count<2 || count>64)continue;
            std::array<E::Entry,2> entries{};bool valid=true;
            for(size_t i=0;i<2;++i){auto& e=entries[i];e.address=state.list+8+i*0xa60;e.slot=int32_t(i);e.role=E::Role::main;uint64_t flags{};uint8_t id{};
                if(!read(e.address,flags) || (flags&0x50000044) || !read(e.address+0x33,id) || id!=i || !read(e.address+0x34,e.world)){valid=false;break;}
            }
            if(valid)if(auto pair=E::get().ordinary_pair(state.list-0x160,entries,state.revision);pair && E::get().current(*pair))return pair;
        }return {};
    }
    static std::optional<unsigned> identify(const Pair& pair,uintptr_t owner,uintptr_t light,uintptr_t view,uint8_t special){
        using ordinary_scene_scope::read;
        if(special || !owner || owner>UINTPTR_MAX-0x21000 || !NativeCameraEntries::get().current(pair))return {};
        unsigned eye{};if(view==pair.left.entry)eye=0;else if(view==pair.right.entry)eye=1;else return {};
        if(pair.left.slot!=0 || pair.right.slot!=1)return {};
        const auto block=owner+0x90+eye*0x3ed0;int32_t count{},kind{};
        if(!read(block,count) || count<1 || count>4 || light<block+0x10 ||
           (light-block-0x10)%0xfb0 || (light-block-0x10)/0xfb0>=size_t(count) ||
           !read(light+0xa4,kind) || kind!=1)return {};
        return eye;
    }
    bool install(void* target,Provider provider,std::function<uintptr_t()> owner,std::function<bool()> enabled,uintptr_t caller=0){
        if(m_native || !target || !provider || !owner || !enabled)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        m_provider=std::move(provider);m_owner=std::move(owner);m_enabled=std::move(enabled);m_caller=caller;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_native=nullptr;return false;
    }
    bool install_in_game(uintptr_t base,const std::filesystem::path& path){
        constexpr game_build::Code expected=game_build::hashed(0x1f7fb90,16,"0e28832ef3f04bc2dfa2a9d5859d0134a6340e62d1cba64d5ace5f1c8f753a07");
        std::array<uint8_t,16> bytes{};std::array<uint8_t,5> call{};
        if(!path.is_absolute() || !ordinary_scene_scope::read(base+game_build::rva(0x1f7fb90),bytes) || !game_build::code(0x1f7fb90,bytes,expected) ||
           !ordinary_scene_scope::read(base+game_build::rva(0x1f7d934),call) || !game_build::code(0x1f7d934,call,game_build::hashed(0x1f7d934,5,"886673f3c531af04ed6aa5fb4338496b05956606c29b8ad128bdb25806b92819")))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(0x1f7fb90)),pair_for_view,[base]{uintptr_t owner{};ordinary_scene_scope::read(base+game_build::rva(0x4faef88),owner);return owner;},
        [path,next=std::chrono::steady_clock::time_point{},enabled=false]()mutable{
            const auto now=std::chrono::steady_clock::now();if(now<next)return enabled;next=now+std::chrono::seconds(1);
            std::ifstream file{path};std::string line;if(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();enabled=refreshes(line);}return enabled;
        },base+game_build::rva(0x1f7d939));
    }
    std::array<uint64_t,6> stats()const{std::lock_guard lock{m_mutex};return m_counts;}
private:
    static void on_call(uintptr_t light,uintptr_t view,uintptr_t count,int32_t quality,uint8_t special,uint8_t refresh){
        auto& self=get();const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());std::optional<unsigned> timed_eye;
        {std::lock_guard lock{self.m_mutex};++self.m_counts[0];}
        try{if(!self.m_caller || caller==self.m_caller){
            {std::lock_guard lock{self.m_mutex};++self.m_counts[1];}
            const auto pair=self.m_provider(view);const auto owner=self.m_owner();
            if(pair){
                {std::lock_guard lock{self.m_mutex};++self.m_counts[2];}
                const auto eye=identify(*pair,owner,light,view,special);
                if(eye && count==light+0xf00){
                    timed_eye=eye;
                    std::lock_guard lock{self.m_mutex};++self.m_counts[3];
                    const std::array<uint64_t,2> tokens{pair->left.token,pair->right.token};
                    if(tokens!=self.m_tokens){self.m_tokens=tokens;self.m_choice=self.m_enabled();}
                    if(self.m_choice && NativeCameraEntries::get().current(*pair)){refresh=1;++self.m_counts[4+*eye];}
                }
            }
        }}catch(...){}
        const bool detail=timed_eye && PerformanceTrace::get().probes();
        const auto start=detail?PerformanceTrace::now_ms():0;
        self.m_native(light,view,count,quality,special,refresh);
        if(detail)PerformanceTrace::get().record(*timed_eye?"shadow_prepare_right_cpu":"shadow_prepare_left_cpu",start,PerformanceTrace::now_ms()-start,refresh);
    }
    Native m_native{};Provider m_provider;std::function<uintptr_t()> m_owner;std::function<bool()> m_enabled;uintptr_t m_caller{};
    mutable std::mutex m_mutex;std::array<uint64_t,6> m_counts{};std::array<uint64_t,2> m_tokens{};bool m_choice{};
};
