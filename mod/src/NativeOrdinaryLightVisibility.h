#pragma once
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include "SceneQueryDependencyTiming.h"
#include "NativeObjectVisibilityProbe.h"
#include <MinHook.h>
#include <intrin.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#include "LightVisibilityHold.h"
#include "LightListProbe.h"
#include "StereoLightList.h"

// Share delayed light occlusion results inside the authenticated two-eye job.
// Native light selection, distance tests and shadows still run. The broader
// force-visible mode remains available solely for causal diagnostics.
class NativeOrdinaryLightVisibility {
public:
    using Job=void(*)(uintptr_t,uintptr_t);
    using Query=int32_t(*)(uintptr_t,uintptr_t,int8_t);
    using Pair=NativeCameraEntries::OrdinaryPair;
    using Provider=std::function<std::optional<Pair>(uintptr_t,uintptr_t)>;
    enum class Mode {native,force_visible,stereo_union};
    static Mode parse(const std::string& value){
        if(value=="light_visibility")return Mode::force_visible;
        if(value=="stereo_cubemaps" || value=="native_reflections" || value=="light_union" || value=="light_union_shadow_refresh" || value=="light_union_ssr" || value=="light_union_ssao" ||
           value=="light_union_screen_shadows" || value=="light_union_all" || value=="light_union_shadow_refresh_ssr" ||
           value=="light_union_shadow_refresh_ssao" || value=="light_union_shadow_refresh_screen_shadows" || value=="light_union_shadow_refresh_all" ||
           value=="light_union_shadow_refresh_native_volumetrics" || value=="light_union_shadow_refresh_native_shadow_lights" || value=="light_union_shadow_refresh_shared_spot_shadows")return Mode::stereo_union;
        return Mode::native;
    }
    // Volumetric consumers of the same per-eye delayed occlusion results, outside the light job:
    // sun/moon light shafts (0x1fb25d0, two calls), the fog sun-visibility test (0x1e2d980) and
    // per-view fog volume visibility bits (0x1e20800, called from Scene). Each draws its effect for
    // an eye only if that eye's own query says visible, so one eye can lose god rays the other has.
    enum class Site {lightshafts,fog_sun,fog_volume};
    static constexpr std::array<std::pair<uintptr_t,Site>,4> SCENE_SITES{{{0x1fb2719,Site::lightshafts},{0x1fb27bc,Site::lightshafts},
        {0x1e2d9c4,Site::fog_sun},{0x1e208c3,Site::fog_volume}}};
    static std::optional<Site> site_for(uintptr_t base,uintptr_t caller){
        if(!base || caller<base)return {};
        for(const auto& [rva,site]:SCENE_SITES)if(game_build::rva(rva) && caller==base+game_build::rva(rva))return site;
        return {};
    }
    // Global holding each site's query manager: shafts and fog sun use getter 0x20939e0, fog volumes 0x20939d0.
    static constexpr uintptr_t manager_global(Site site){return site==Site::fog_volume?0x4ff0bd0:0x4ff0bc8;}
    // Light union also unions the volumetric sites, except in this A/B mode.
    static bool unions_volumetrics(const std::string& value){
        return parse(value)==Mode::stereo_union && value!="light_union_shadow_refresh_native_volumetrics";
    }
    struct SceneSites {
        std::function<std::optional<Site>(uintptr_t)> site;
        std::function<std::optional<ordinary_scene_scope::Scope>()> scope;
        std::function<uintptr_t(Site)> manager;
        std::function<bool()> enabled;
    };
    // Call once after install(); the scene sites share the query detour. Published with release
    // ordering and never replaced, so the detour reads them without copying or locking.
    bool install_scene_sites(SceneSites sites){
        if(!m_query || !sites.site || !sites.scope || !sites.manager || !sites.enabled || m_scene_ready.load(std::memory_order_acquire))return false;
        {std::lock_guard lock{m_mutex};m_scene=std::move(sites);}
        m_scene_ready.store(true,std::memory_order_release);return true;
    }
    // [unioned shafts, fog sun, fog volume; refused (same order); changed: union differed from this eye]
    std::array<uint64_t,9> scene_stats()const{std::lock_guard lock{m_mutex};return m_scene_counts;}
    static NativeOrdinaryLightVisibility& get(){static auto* p=new NativeOrdinaryLightVisibility;return *p;}
    static std::optional<Pair> pair_for(uintptr_t base,uintptr_t owner,uintptr_t list){
        uintptr_t expected_owner{},render{};int32_t count{};
        if(!ordinary_scene_scope::read(base+game_build::rva(0x4faf810),expected_owner) || owner!=expected_owner || !owner ||
           !ordinary_scene_scope::read(base+game_build::rva(0x4f7c6c8),render) || !render || list!=render+0x160 ||
           !ordinary_scene_scope::read(list+0x29b14,count) || count<2 || count>4)return {};
        const auto revision=NativeCameraEntries::get().revision(render);if(!revision)return {};
        std::array<NativeCameraEntries::Entry,2> entries{};
        for(size_t i=0;i<2;++i){auto& e=entries[i];e.address=list+8+i*0xa60;e.slot=int32_t(i);e.role=NativeCameraEntries::Role::main;uint64_t flags{};uint8_t id{};
            if(!ordinary_scene_scope::read(e.address,flags) || (flags&0x50000044) || !ordinary_scene_scope::read(e.address+0x33,id) || id!=i || !ordinary_scene_scope::read(e.address+0x34,e.world))return {};
        }
        return NativeCameraEntries::get().ordinary_pair(render,entries,*revision);
    }
    bool install(void* job,void* query,Provider provider,std::function<uintptr_t()> manager,std::function<Mode()> enabled,uintptr_t base=0){
        if(m_job || !job || !query || !provider || !manager || !enabled)return false;
        auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(job,reinterpret_cast<void*>(&on_job),reinterpret_cast<void**>(&m_job))!=MH_OK)return false;
        if(MH_CreateHook(query,reinterpret_cast<void*>(&on_query),reinterpret_cast<void**>(&m_query))!=MH_OK){MH_RemoveHook(job);m_job=nullptr;return false;}
        m_provider=std::move(provider);m_manager=std::move(manager);m_enabled=std::move(enabled);m_base=base;m_caller=base?base+game_build::rva(0x1ef97de):0;
        if(MH_EnableHook(query)==MH_OK && MH_EnableHook(job)==MH_OK)return true;
        for(auto target:{job,query}){MH_DisableHook(target);MH_RemoveHook(target);}m_job=nullptr;m_query=nullptr;return false;
    }
    bool install_scene_sites_in_game(uintptr_t base,const std::filesystem::path& path){
        // Each site must still be the expected native call into the query routine.
        constexpr std::array<game_build::Code,4> calls{game_build::hashed(0x1fb2714,5,"f26224e88d214bbf4b66d94050a858b42b4560cf6bfc4861121f9afc017530a7"),
            game_build::hashed(0x1fb27b7,5,"50db19fa01007d06e468b9af15dfaa3694957428de443f60381915c4830a69b7"),
            game_build::hashed(0x1e2d9bf,5,"69d992a09cbc6a4f6442c7b455c1d675776010316382d956c89e817245b4c399"),
            game_build::hashed(0x1e208be,5,"96fc21042b4058d0c23bcecb527d0a3d3e003696a9aa0c50d826e6514f4e3353")};
        for(const auto& bytes:calls){const auto rva=bytes.reference;std::array<uint8_t,5> site{};if(!game_build::rva(rva) || !ordinary_scene_scope::read(base+game_build::rva(rva),site) || !game_build::code(rva,site,bytes))return false;}
        return install_scene_sites({[base](uintptr_t caller){return site_for(base,caller);},
            []()->std::optional<ordinary_scene_scope::Scope>{const auto s=NativeRenderProbe::current_scene();const auto p=NativeRenderProbe::current_ordinary_pair();if(!s || !p)return {};return ordinary_scene_scope::Scope{*s,*p};},
            [base](Site site){uintptr_t manager{};ordinary_scene_scope::read(base+game_build::rva(manager_global(site)),manager);return manager;},
            [path,next=std::chrono::steady_clock::time_point{},value=false]()mutable{
                const auto now=std::chrono::steady_clock::now();if(now<next)return value;next=now+std::chrono::seconds(1);
                std::ifstream file{path};std::string line;if(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();value=unions_volumetrics(line);}return value;
            }});
    }
    bool install_in_game(uintptr_t base,const std::filesystem::path& path){
        constexpr game_build::Code a=game_build::hashed(0x1ef8dd0,16,"0660b5c4b841e981298e966381a19b793acd0bd4521376e0f151803e411f2e34");
        constexpr game_build::Code b=game_build::hashed(0x2093860,16,"fbba0029409769c8a551c4c80318670aae683236acb7d8298e877cc1b244045c");
        std::array<uint8_t,16> x{},y{};std::array<uint8_t,5> call{};
        if(!path.is_absolute() || !ordinary_scene_scope::read(base+game_build::rva(0x1ef8dd0),x) || !game_build::code(0x1ef8dd0,x,a) || !ordinary_scene_scope::read(base+game_build::rva(0x2093860),y) || !game_build::code(0x2093860,y,b) ||
           !ordinary_scene_scope::read(base+game_build::rva(0x1ef97d9),call) || !game_build::code(0x1ef97d9,call,game_build::hashed(0x1ef97d9,5,"bfafd7bb6e1829118a37debe32cef7efb93aac7327e3e03f313f9eb8f71cfe22")))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(0x1ef8dd0)),reinterpret_cast<void*>(base+game_build::rva(0x2093860)),
            [base](uintptr_t owner,uintptr_t list){return pair_for(base,owner,list);},
            [base]{uintptr_t manager{};ordinary_scene_scope::read(base+game_build::rva(0x4ff0bd0),manager);return manager;},
            [path,next=std::chrono::steady_clock::time_point{},value=Mode::native]()mutable{
                const auto now=std::chrono::steady_clock::now();if(now<next)return value;next=now+std::chrono::seconds(1);
                std::ifstream file{path};std::string line;if(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();value=parse(line);}return value;
            },base);
    }
    std::array<uint64_t,3> stats()const{std::lock_guard lock{m_mutex};return m_counts;}
    // Diagnostic hold (LightVisibilityHold.h): a one-number control file, frames to hold a light's last
    // visible union result; 0 = off. Polled once a second from the light job.
    void configure_hold(const std::filesystem::path& path){std::lock_guard lock{m_mutex};if(path.is_absolute())m_hold_path=path;}
    // Candidate fix control (light_clamp.txt "native" | "clamp"), polled once a second from the light job.
    void configure_clamp(const std::filesystem::path& path){std::lock_guard lock{m_mutex};if(path.is_absolute())m_clamp_path=path;}
    bool clamp_configured()const{std::lock_guard lock{m_mutex};return !m_clamp_path.empty();}
    std::pair<bool,std::array<uint64_t,2>> clamp_stats()const{std::lock_guard lock{m_mutex};return {m_clamp,m_clamp_counts};}
    static std::optional<bool> parse_clamp(std::string line){if(!line.empty() && line.back()=='\r')line.pop_back();if(line=="clamp")return true;if(line=="native")return false;return std::nullopt;}
    bool hold_configured()const{std::lock_guard lock{m_mutex};return !m_hold_path.empty();}
    std::pair<LightVisibilityHold::Setting,std::array<LightVisibilityHold::Stats,2>> hold_stats()const{std::lock_guard lock{m_mutex};return {m_hold_setting,{m_hold.stats(0),m_hold.stats(1)}};}
private:
    // manager: the light manager global, read once per job (a system call) instead of once per light query.
    struct Active {Pair pair;Mode mode{};uintptr_t manager{};};
    static void on_job(uintptr_t owner,uintptr_t list){
        auto& self=get();const auto previous=s_active;s_active.reset();
        try{const auto pair=self.m_provider(owner,list);if(pair && NativeCameraEntries::get().current(*pair)){std::lock_guard lock{self.m_mutex};s_active=Active{*pair,self.m_enabled(),self.m_manager()};++self.m_counts[0];
            if(!self.m_clamp_path.empty()){const auto now=std::chrono::steady_clock::now();if(now>=self.m_clamp_next){self.m_clamp_next=now+std::chrono::seconds(1);
                std::ifstream file{self.m_clamp_path};std::string line;if(std::getline(file,line))if(const auto c=parse_clamp(line))self.m_clamp=*c;}}
            if(!self.m_hold_path.empty()){const auto now=std::chrono::steady_clock::now();if(now>=self.m_hold_next){self.m_hold_next=now+std::chrono::seconds(1);
                std::ifstream file{self.m_hold_path};std::string line;if(std::getline(file,line))if(const auto setting=LightVisibilityHold::parse(line))self.m_hold_setting=*setting;}}
            if(self.m_counts[0]%600==0)self.m_hold.prune(self.m_counts[0],1200);}}catch(...){}
        struct Restore{std::optional<Active> value;~Restore(){s_active=value;}} restore{previous};
        const bool logged=self.m_list_log && s_active.has_value();
        if(logged){std::lock_guard lock{self.m_mutex};self.m_job_facts.clear();}
        self.m_job(owner,list);
        if(s_active.has_value())try{self.share_lists(owner,list);}catch(...){}
        if(s_active.has_value())try{self.copy_list(owner);}catch(...){}
        if(logged)try{self.log_lists(owner,list);}catch(...){}
    }
    // Prototype "decide once" for lights (copy_lights.txt "copy"): view 1's unshadowed light output
    // becomes view 0's (array lm+0x8580+v*0x1ef08, 0x210-byte desc copies, count lm+0x27480+v*0x1ef08,
    // at most 0xf0). Lights only the right eye's outer edge needs are then missing in slot 1.
    void copy_list(uintptr_t lm){
        bool copy{};{std::lock_guard lock{m_mutex};
            if(m_copy_path.empty())return;
            const auto now=std::chrono::steady_clock::now();
            if(now>=m_copy_next){m_copy_next=now+std::chrono::seconds(1);std::ifstream file{m_copy_path};std::string line;
                if(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();if(line=="copy")m_copy=true;else if(line=="native")m_copy=false;}}
            copy=m_copy;}
        if(!copy || !lm)return;
        int32_t count{};if(!ordinary_scene_scope::read(lm+0x27480,count) || count<0 || count>0xf0){std::lock_guard lock{m_mutex};++m_copy_counts[1];return;}
        std::memcpy(reinterpret_cast<void*>(lm+0x8580+0x1ef08),reinterpret_cast<const void*>(lm+0x8580),size_t(count)*0x210);
        *reinterpret_cast<int32_t*>(lm+0x27480+0x1ef08)=count;
        std::lock_guard lock{m_mutex};++m_copy_counts[0];
    }
    // Decide once for lights (StereoLightList.h, docs/DECIDE_ONCE_STEREO.md), after the native job in an
    // authenticated pair: "shared" writes one list, every light either eye kept, to both views; "copy"
    // gives view 1 view 0's list. Lights in a view's shadow list never enter its unshadowed list, so
    // the union is only safe while both views shadow the same lights (NativeOrdinaryShadowLights
    // aligns them); otherwise this frame falls back to the copy and counts it.
    void share_lists(uintptr_t lm,uintptr_t list){
        using ordinary_scene_scope::read;
        const auto mode=stereo_light_list::Mode(m_stereo_mode.load(std::memory_order_relaxed));
        if(mode!=stereo_light_list::Mode::shared && mode!=stereo_light_list::Mode::copy)return;
        constexpr size_t STRIDE=0x1ef08,FIRST=0x8580,COUNT=0x27480,SHADOW=0x50,SHADOW_LIST=0x44;
        std::array<int32_t,2> n{};
        for(size_t v=0;v<2;++v)if(!lm || !read(lm+COUNT+v*STRIDE,n[v]) || n[v]<0 || n[v]>int32_t(stereo_light_list::CAP)){std::lock_guard lock{m_mutex};++m_stereo_counts[3];return;}
        std::array<std::array<uint8_t,SHADOW_LIST>,2> shadow{};
        const bool aligned=read(lm+SHADOW,shadow[0]) && read(lm+SHADOW+SHADOW_LIST,shadow[1]) && shadow[0]==shadow[1];
        if(mode==stereo_light_list::Mode::copy || !aligned){
            std::memcpy(reinterpret_cast<void*>(lm+FIRST+STRIDE),reinterpret_cast<const void*>(lm+FIRST),size_t(n[0])*stereo_light_list::ENTRY);
            *reinterpret_cast<int32_t*>(lm+COUNT+STRIDE)=n[0];
            std::lock_guard lock{m_mutex};++m_stereo_counts[mode==stereo_light_list::Mode::copy?1:2];return;
        }
        std::array<std::vector<uint8_t>,2> lists;std::array<std::array<float,3>,2> eye{};
        for(size_t v=0;v<2;++v){lists[v].resize(size_t(n[v])*stereo_light_list::ENTRY);
            SIZE_T got{};if((n[v] && (!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(lm+FIRST+v*STRIDE),lists[v].data(),lists[v].size(),&got) || got!=lists[v].size())) ||
               !read(list+8+v*0xa60+0x64,eye[v])){std::lock_guard lock{m_mutex};++m_stereo_counts[3];return;}}
        dump_lists(lists,eye);
        const auto merged=stereo_light_list::merge(lists[0].data(),size_t(n[0]),lists[1].data(),size_t(n[1]),eye[0],eye[1]);
        for(size_t v=0;v<2;++v){
            if(merged.count)std::memcpy(reinterpret_cast<void*>(lm+FIRST+v*STRIDE),merged.entries.data(),merged.entries.size());
            *reinterpret_cast<int32_t*>(lm+COUNT+v*STRIDE)=int32_t(merged.count);
        }
        std::lock_guard lock{m_mutex};++m_stereo_counts[0];m_stereo_counts[4]+=merged.from_second;m_stereo_counts[5]+=merged.capped;m_stereo_counts[6]+=merged.differing_copies;
        m_stereo_counts[7]=std::max<uint64_t>(m_stereo_counts[7],merged.count);
    }
    // Diagnostic: with light_dump.txt holding N in the configured folder, the next N jobs' raw view lists
    // and eye positions go to light_dump_<job>.bin (header: two int32 counts, six floats; then view 0's
    // entries, then view 1's). The folder is polled once a second; the file is emptied after use.
    void dump_lists(const std::array<std::vector<uint8_t>,2>& lists,const std::array<std::array<float,3>,2>& eye){
        std::filesystem::path folder;int remaining{};
        {std::lock_guard lock{m_mutex};if(m_dump_folder.empty())return;
            const auto now=std::chrono::steady_clock::now();
            if(!m_dump_remaining && now>=m_dump_next){m_dump_next=now+std::chrono::seconds(1);std::ifstream file{m_dump_folder/"light_dump.txt"};int value{};
                if(file>>value && value>0 && value<=32){m_dump_remaining=value;file.close();std::ofstream{m_dump_folder/"light_dump.txt",std::ios::trunc};}}
            if(!m_dump_remaining)return;remaining=m_dump_remaining--;folder=m_dump_folder;}
        std::ofstream out{folder/("light_dump_"+std::to_string(m_counts[0])+"_"+std::to_string(remaining)+".bin"),std::ios::binary};
        const int32_t counts[2]{int32_t(lists[0].size()/stereo_light_list::ENTRY),int32_t(lists[1].size()/stereo_light_list::ENTRY)};
        out.write(reinterpret_cast<const char*>(counts),sizeof counts);
        for(const auto& e:eye)out.write(reinterpret_cast<const char*>(e.data()),12);
        for(const auto& l:lists)out.write(reinterpret_cast<const char*>(l.data()),std::streamsize(l.size()));
    }
    std::filesystem::path m_dump_folder;int m_dump_remaining{};std::chrono::steady_clock::time_point m_dump_next{};
public:
    void configure_light_dump(const std::filesystem::path& folder){std::lock_guard lock{m_mutex};if(folder.is_absolute())m_dump_folder=folder;}
    void set_stereo_mode(stereo_light_list::Mode mode){m_stereo_mode.store(int(mode),std::memory_order_relaxed);}
    stereo_light_list::Mode stereo_mode()const{return stereo_light_list::Mode(m_stereo_mode.load(std::memory_order_relaxed));}
    // [shared, copied, fell back to copy (shadow lists differ), refused, right-eye-only lights added, lights over the cap, copies that differed, largest list]
    std::array<uint64_t,8> stereo_stats()const{std::lock_guard lock{m_mutex};return m_stereo_counts;}
private:
    std::atomic<int> m_stereo_mode{int(stereo_light_list::Mode::capsort)};std::array<uint64_t,8> m_stereo_counts{};
public:
    void configure_copy(const std::filesystem::path& path){std::lock_guard lock{m_mutex};if(path.is_absolute())m_copy_path=path;}
    bool copy_configured()const{std::lock_guard lock{m_mutex};return !m_copy_path.empty();}
    std::pair<bool,std::array<uint64_t,2>> copy_stats()const{std::lock_guard lock{m_mutex};return {m_copy,m_copy_counts};}
private:
    std::filesystem::path m_copy_path;bool m_copy{};std::array<uint64_t,2> m_copy_counts{};std::chrono::steady_clock::time_point m_copy_next{};
    // After the job: for every light queried in it, its flag, list membership and range facts per view.
    void log_lists(uintptr_t lm,uintptr_t list){
        using ordinary_scene_scope::read;
        std::lock_guard lock{m_mutex};
        std::array<std::vector<std::array<float,3>>,2> kept;
        for(int v=0;v<2;++v){int32_t count{};if(!read(lm+0x27480+size_t(v)*0x1ef08,count) || count<0 || count>0xf0)continue;
            for(int32_t i=0;i<count;++i){std::array<float,3> p{};if(read(lm+0x8580+size_t(v)*0x1ef08+size_t(i)*0x210+0x30,p))kept[v].push_back(p);}}
        std::array<std::array<float,3>,2> camera{};for(int v=0;v<2;++v)read(list+8+size_t(v)*0xa60+0x64,camera[v]);
        for(auto& f:m_job_facts){
            uintptr_t desc{};uint8_t flag{},shadow{};std::array<float,3> p{};
            if(!read(f.light+0x18,desc) || !desc || !read(f.light+0x90+size_t(f.view),flag) || !read(desc+0x30,p))continue;
            read(desc+0x6c,f.type);read(desc+0x20b,shadow);read(desc+0x104,f.range2);
            f.flag_after=flag!=0;f.shadow=shadow!=0;
            const auto& c=camera[size_t(f.view)];f.dist2=(p[0]-c[0])*(p[0]-c[0])+(p[1]-c[1])*(p[1]-c[1])+(p[2]-c[2])*(p[2]-c[2]);
            f.in_list=std::find(kept[size_t(f.view)].begin(),kept[size_t(f.view)].end(),p)!=kept[size_t(f.view)].end();
            f.in_cache=f.shadow && m_in_cache && m_cache && m_in_cache(m_cache,f.light);
        }
        auto lines=m_probe.job(m_job_facts);
        if(!lines.empty()){
            // Passed the occlusion test this frame (flag set): about the candidate count the cap sorts at 0x1ef9b4f.
            std::array<size_t,2> passed{};for(const auto& f:m_job_facts)if(f.flag_after && f.view>=0 && f.view<2)++passed[size_t(f.view)];
            std::array<int,2> skip{-1,-1};for(int v=0;v<2;++v){uint8_t flags{};if(read(list+8+size_t(v)*0xa60,flags))skip[size_t(v)]=(flags>>6)&1;}
            lines.insert(lines.begin()+1,"LightListCount: view0="+std::to_string(kept[0].size())+" view1="+std::to_string(kept[1].size())+" passed0="+std::to_string(passed[0])+
                " passed1="+std::to_string(passed[1])+" skip_cull_bit6="+std::to_string(skip[0])+"/"+std::to_string(skip[1])+" queried="+std::to_string(m_job_facts.size())+" (240 is the cap)");
        }
        for(auto& line:lines)if(m_list_lines.size()<64)m_list_lines.push_back(std::move(line));
    }
public:
    // Diagnostic light-list log (LightListProbe.h). in_cache: 0x1e8a900(cache, light), read-only.
    void configure_list_log(uintptr_t cache,bool(*in_cache)(uintptr_t,uintptr_t)){std::lock_guard lock{m_mutex};m_list_log=true;m_cache=cache;m_in_cache=in_cache;}
    std::vector<std::string> take_list_lines(){std::lock_guard lock{m_mutex};auto out=std::move(m_list_lines);m_list_lines.clear();return out;}
private:
    static int32_t on_query(uintptr_t manager,uintptr_t object,int8_t eye){
        const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());auto& self=get();
        // Construct restoration before fallible pre-query state, so destruction
        // of that state is covered as well as all post-query observers.
        struct RestoreError {DWORD value;~RestoreError(){SetLastError(value);}} restore_error{GetLastError()};
        NativeConsoleQueryAdmissionProbe::Query admission{manager,object,eye,caller};const auto native=self.m_query(manager,object,eye);restore_error.value=GetLastError();
        {const auto query_error=GetLastError();NativeObjectVisibilityProbe::query(manager,object,eye,native,caller);SetLastError(query_error);}
        if(SceneQueryDependencyTiming::active()){
            const auto query_error=GetLastError();
            SceneQueryDependencyTiming::consume(manager,object,eye,native,caller-self.m_base,PerformanceTrace::now_ms());
            SetLastError(query_error);
        }
        try{if(s_active && s_active->mode!=Mode::native && eye>=0 && eye<2 && object && manager &&
            (!self.m_base || caller==self.m_caller) && manager==s_active->manager && NativeCameraEntries::get().current(s_active->pair)){
                // The native query reads a completed occlusion result. Keep a light
                // if either eye sees it; preserve missing/hidden results in both.
                const auto other=s_active->mode==Mode::force_visible?4:self.m_query(manager,object,int8_t(1-eye));
                if(!NativeCameraEntries::get().current(s_active->pair))return native;
                std::lock_guard lock{self.m_mutex};++self.m_counts[size_t(eye)+1];
                // Raw per-eye result classes are counted when the hold diagnostic is configured; the hold
                // fills this eye's missing (-1) result from its last real one (LightVisibilityHold.h).
                const auto own=self.m_hold_path.empty()?native:self.m_hold.apply(object,eye,native,self.m_counts[0],self.m_hold_setting);
                if(self.m_list_log && self.m_job_facts.size()<8192){LightListProbe::Facts f;f.light=object;f.view=eye;f.own=native;f.other=other;self.m_job_facts.push_back(f);}
                const auto result=s_active->mode==Mode::force_visible?4:(std::max)(own,other);
                // Candidate fix (clamp): a result that passes (2 and more) reads as 4, so the per-frame pixel
                // count no longer reorders the view's capped light ranking; occluded (0, 1, -1) still drops.
                if(self.m_clamp){if(result>=2)++self.m_clamp_counts[size_t(eye)];return result>=2?4:result;}
                return result;
        }}catch(...){}
        try{if(const auto shared=self.scene_union(manager,object,eye,native,caller))return *shared;}catch(...){}
        const auto admitted=admission.finish(native);if(admitted!=native)return admitted;return NativeObjectVisibilityCausalProbe::query(manager,object,eye,native,caller);
    }
    // Union a volumetric site's query across the two eyes of the authenticated current Scene.
    // The query eye must be that Scene's own slot; the mode is frozen per dispatch.
    std::optional<int32_t> scene_union(uintptr_t manager,uintptr_t object,int8_t eye,int32_t native,uintptr_t caller){
        if(!m_scene_ready.load(std::memory_order_acquire))return {};
        const auto& scene=m_scene;
        const auto site=scene.site(caller);if(!site)return {};
        const auto index=size_t(*site);
        const auto scope=scene.scope();
        if(eye<0 || eye>1 || !object || !manager || !scope || ordinary_scene_scope::inspect(*scope).mask || scope->scene.slot!=unsigned(eye) ||
           manager!=scene.manager(*site) || !NativeCameraEntries::get().current(scope->pair)){
            std::lock_guard lock{m_mutex};++m_scene_counts[3+index];return {};
        }
        bool enabled{};
        {std::lock_guard lock{m_mutex};
            if(scope->scene.dispatch_sequence>m_scene_dispatch){m_scene_dispatch=scope->scene.dispatch_sequence;m_scene_enabled=m_scene.enabled();}
            enabled=scope->scene.dispatch_sequence==m_scene_dispatch && m_scene_enabled;}
        if(!enabled)return {};
        const auto result=(std::max)(native,m_query(manager,object,int8_t(1-eye)));
        if(!NativeCameraEntries::get().current(scope->pair))return {};
        std::lock_guard lock{m_mutex};++m_scene_counts[index];if(result!=native)++m_scene_counts[6+index];return result;
    }
    SceneSites m_scene;std::atomic<bool> m_scene_ready{};std::array<uint64_t,9> m_scene_counts{};uint64_t m_scene_dispatch{};bool m_scene_enabled{};
    Job m_job{};Query m_query{};Provider m_provider;std::function<uintptr_t()> m_manager;std::function<Mode()> m_enabled;uintptr_t m_base{},m_caller{};
    mutable std::mutex m_mutex;std::array<uint64_t,3> m_counts{};
    bool m_list_log{};uintptr_t m_cache{};bool(*m_in_cache)(uintptr_t,uintptr_t){};std::vector<LightListProbe::Facts> m_job_facts;LightListProbe m_probe;std::vector<std::string> m_list_lines;
    std::filesystem::path m_clamp_path;bool m_clamp{};std::array<uint64_t,2> m_clamp_counts{};std::chrono::steady_clock::time_point m_clamp_next{};
    LightVisibilityHold m_hold;std::filesystem::path m_hold_path;LightVisibilityHold::Setting m_hold_setting{};std::chrono::steady_clock::time_point m_hold_next{};
    inline static thread_local std::optional<Active> s_active;
};
