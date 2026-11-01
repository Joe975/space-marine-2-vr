#include "EnginePlayerOpacity.h"
#include "EngineBodyProbe.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <array>
#include <cmath>
#include <algorithm>
#include <intrin.h>
namespace {
constexpr game_build::Code PROLOGUE=game_build::hashed(0x1343f10,41,"68800fd586ab062781bb1ac60097535a61e836aa553ab7233b5aa80e39fbe74b");
constexpr game_build::Code GETTER_PROLOGUE=game_build::hashed(0x1d95680,26,"5abac0323202d75b353b52d94ce325ca8b024e518784f1afd2c7c1e409e7beb8");
constexpr game_build::Code DRAW_PROLOGUE=game_build::hashed(0x1d8b0f0,24,"2a0d7277af1a42d327f43f5029fc95b39ec0b132e3e64e772a828def54efaeda");
constexpr game_build::Code PARAMS_PROLOGUE=game_build::hashed(0x20bd700,29,"6b77abce9644b771ebe74388bc20cdeb1c086b9341fb01331dfa79b5843fe509");
constexpr game_build::Code REFRESH_PROLOGUE=game_build::hashed(0x1d7fa70,16,"6e0eed4cae2a5655a76e02c9c163b847bb67a77cd3967bb05df77c90f026b37b");
bool read(uintptr_t base,size_t offset,uintptr_t& out) {
    SIZE_T count{};return base && base<=UINTPTR_MAX-offset-sizeof(out) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+offset),&out,sizeof(out),&count) && count==sizeof(out);
}
}
EnginePlayerOpacity& EnginePlayerOpacity::get() { static auto* value=new EnginePlayerOpacity;return *value; }
bool EnginePlayerOpacity::matches_build(uint32_t stamp,std::span<const uint8_t> bytes) { return game_build::code(stamp,SETTER_RVA,bytes,PROLOGUE); }
bool EnginePlayerOpacity::matches_getter(uint32_t stamp,std::span<const uint8_t> bytes) { return game_build::code(stamp,GETTER_RVA,bytes,GETTER_PROLOGUE); }
bool EnginePlayerOpacity::matches_draw(uint32_t stamp,std::span<const uint8_t> bytes) { return game_build::code(stamp,DRAW_RVA,bytes,DRAW_PROLOGUE); }
bool EnginePlayerOpacity::matches_params(uint32_t stamp,std::span<const uint8_t> bytes) { return game_build::code(stamp,PARAMS_RVA,bytes,PARAMS_PROLOGUE); }
bool EnginePlayerOpacity::matches_refresh(uint32_t stamp,std::span<const uint8_t> bytes) { return game_build::code(stamp,REFRESH_RVA,bytes,REFRESH_PROLOGUE); }
bool EnginePlayerOpacity::install_in_game(HMODULE module,bool opacity) {
    if(installed()) return true;
    const auto base=reinterpret_cast<uint8_t*>(module);if(!base) return false;
    const auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);const auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;
    const auto setter=game_build::rva(stamp,SETTER_RVA),getter=game_build::rva(stamp,GETTER_RVA),draw=game_build::rva(stamp,DRAW_RVA),params=game_build::rva(stamp,PARAMS_RVA),refresh=game_build::rva(stamp,REFRESH_RVA);
    if(!setter || !getter || !draw || !params || !refresh) return false;
    if(nt->OptionalHeader.SizeOfImage<getter+GETTER_PROLOGUE.size() || !matches_build(stamp,{base+setter,PROLOGUE.size()}) || !matches_getter(stamp,{base+getter,GETTER_PROLOGUE.size()}) || !matches_draw(stamp,{base+draw,DRAW_PROLOGUE.size()})) return false;
    m_game_base=reinterpret_cast<uintptr_t>(base);m_draw_caller=game_build::rva(stamp,0x1e78931);
    if(nt->OptionalHeader.SizeOfImage<params+PARAMS_PROLOGUE.size() || !matches_params(stamp,{base+params,PARAMS_PROLOGUE.size()}))return false;
    if(!matches_refresh(stamp,{base+refresh,REFRESH_PROLOGUE.size()}))return false;
    return install(opacity?base+setter:nullptr,[](auto now){return EngineBodyProbe::get().active_actor(now);},opacity?base+getter:nullptr,opacity?base+draw:nullptr,base+params,base+refresh);
}
bool EnginePlayerOpacity::install(void* target,std::function<uintptr_t(Clock::time_point)> actor,void* getter,void* draw,void* params,void* refresh) {
    if(installed()) return true;
    if((!target && !params) || !actor) return false;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    m_actor=std::move(actor);
    if(target) {
        if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&m_original))!=MH_OK) return false;
        m_target=target;
    }
    if(getter) {
        if(MH_CreateHook(getter,reinterpret_cast<void*>(&getter_hook),reinterpret_cast<void**>(&m_getter))!=MH_OK) { uninstall();return false; }
        m_getter_target=getter;
    }
    if(draw) {
        if(MH_CreateHook(draw,reinterpret_cast<void*>(&draw_hook),reinterpret_cast<void**>(&m_draw))!=MH_OK) {uninstall();return false;}
        m_draw_target=draw;
    }
    if(params) {
        if(MH_CreateHook(params,reinterpret_cast<void*>(&params_hook),reinterpret_cast<void**>(&m_params))!=MH_OK) {uninstall();return false;}
        m_params_target=params;
    }
    if(refresh) {
        if(!params || MH_CreateHook(refresh,reinterpret_cast<void*>(&refresh_hook),reinterpret_cast<void**>(&m_refresh))!=MH_OK) {uninstall();return false;}
        m_refresh_target=refresh;
    }
    if((target && MH_EnableHook(target)!=MH_OK) || (getter && MH_EnableHook(getter)!=MH_OK) || (draw && MH_EnableHook(draw)!=MH_OK) || (params && MH_EnableHook(params)!=MH_OK) || (refresh && MH_EnableHook(refresh)!=MH_OK)) { uninstall();return false; }
    {std::scoped_lock lock{m_mutex};m_params_state.installed=params!=nullptr;}
    { std::scoped_lock lock{m_mutex};m_state.getter_installed=getter!=nullptr; }
    {std::scoped_lock lock{m_mutex};m_draw_state.installed=draw!=nullptr;}
    m_installed=true;allow(false);return true;
}
void EnginePlayerOpacity::enable(bool enabled) { std::scoped_lock lock{m_mutex};m_state.enabled=enabled; }
void EnginePlayerOpacity::allow(bool allowed) {
    uintptr_t instance{};const auto actor=m_actor?m_actor(Clock::now()):0;
    if(actor) read(actor,0xd8,instance);m_candidate=instance;
    std::scoped_lock lock{m_mutex};m_state.allowed=allowed;
}
void EnginePlayerOpacity::uninstall() {
    enable(false);allow(false);
    for(void* target:{m_target,m_getter_target,m_draw_target,m_params_target,m_refresh_target}) if(target) { MH_DisableHook(target);MH_RemoveHook(target); }
    m_params_target=m_refresh_target=nullptr;
    m_target=m_getter_target=m_draw_target=nullptr;m_installed=false;m_candidate=0;
    { std::scoped_lock lock{m_mutex};m_state.getter_installed=false;m_draw_state.installed=false;m_params_state.installed=false;m_params_state.enabled=false;m_params_state.all_objects=false;m_distance_cache.clear(); }
}
EnginePlayerOpacity::Snapshot EnginePlayerOpacity::snapshot() const { std::scoped_lock lock{m_mutex};auto result=m_state;result.reads=m_reads.load();return result; }
float EnginePlayerOpacity::select(void* object,float native) {
    std::scoped_lock lock{m_mutex};++m_state.calls;
    const auto actor=reinterpret_cast<uintptr_t>(object);
    const auto selected=m_actor(Clock::now());
    uintptr_t instance{},props{},instance_props{},entity{};
    if(!actor || actor!=selected || !read(actor,0xd8,instance) || !instance || !read(actor,0x20,props) || !props ||
       !read(instance,0x58,instance_props) || props!=instance_props || !read(actor,0x38,entity) || !entity) return native;
    ++m_state.owned;m_state.actor=actor;m_state.instance=instance;m_state.native=native;
    const bool valid=std::isfinite(native) && native>=0 && native<=1;
    if(!valid) ++m_state.invalid;
    // Never call a setter from Present or write a saved pointer: every native call carries
    // the current original value and owns its renderer propagation on its native thread.
    const bool override=m_state.enabled && m_state.allowed && valid;
    m_state.effective=override?1.0f:native;
    if(override) ++m_state.overrides;else ++m_state.released;
    return m_state.effective;
}
void EnginePlayerOpacity::hook(void* object,float value) {
    auto& self=get();float effective=value;
    try { effective=self.select(object,value); } catch(...) {}
    self.m_original(object,effective);
}
float EnginePlayerOpacity::select_instance(void* object,float native) {
    ++m_reads;
    const auto instance=reinterpret_cast<uintptr_t>(object);
    // The getter sees hundreds of thousands of unrelated instances per second. Avoid
    // locking the body probe and opacity state for each NPC/scenery read.
    if(!instance || instance!=m_candidate.load()) return native;
    std::scoped_lock lock{m_mutex};const auto actor=m_actor(Clock::now());
    uintptr_t owned_instance{},props{},instance_props{},entity{};
    if(!actor || !instance || !read(actor,0xd8,owned_instance) || owned_instance!=instance || !read(actor,0x20,props) || !props ||
       !read(instance,0x58,instance_props) || props!=instance_props || !read(actor,0x38,entity) || !entity) return native;
    ++m_state.owned_reads;m_state.actor=actor;m_state.instance=instance;m_state.render_native=native;
    const bool valid=std::isfinite(native) && native>=0 && native<=1;
    if(valid) {m_state.render_min_native=std::min(m_state.render_min_native,native);if(native<1) ++m_state.native_faded_reads;}
    const bool override=m_state.enabled && m_state.allowed && valid;
    if(override && native<1) ++m_state.changed_reads;
    m_state.render_effective=override?1.0f:native;if(override) ++m_state.read_overrides;
    return m_state.render_effective;
}
float EnginePlayerOpacity::getter_hook(void* instance) {
    auto& self=get();const float native=self.m_getter(instance);
    try { return self.select_instance(instance,native); } catch(...) { return native; }
}
EnginePlayerOpacity::DrawSnapshot EnginePlayerOpacity::draw_snapshot() const {std::scoped_lock lock{m_mutex};return m_draw_state;}
EnginePlayerOpacity::ParamsSnapshot EnginePlayerOpacity::params_snapshot() const {std::scoped_lock lock{m_mutex};auto result=m_params_state;result.tracked=m_distance_cache.size();result.swept=m_distance_cache.swept();result.evicted=m_distance_cache.evicted();return result;}
void EnginePlayerOpacity::keep_distance_opaque(bool enabled) {distance_control(enabled?"opaque":"native");}
bool EnginePlayerOpacity::distance_control(std::string_view mode) {
    if(mode!="native" && mode!="opaque" && mode!="all")return false;
    std::scoped_lock lock{m_mutex};m_params_state.enabled=mode!="native";m_params_state.all_objects=mode=="all";return true;
}
void EnginePlayerOpacity::refresh_hook(void* context,uint64_t handle) {
    auto& self=get();
    try {
        const bool owned=self.owns_draw(handle);bool rebuild{};
        // Most draws reuse a cache already tracked: note it under a shared lock, so the render workers
        // drawing meshes don't queue on one another (MOD_REVIEW_2026-10-29). Anything else is exclusive.
        const bool tracked=[&]{
            std::shared_lock read{self.m_mutex};
            return (owned || self.m_params_state.all_objects) && self.m_params_state.enabled && self.m_state.allowed && self.m_distance_cache.touch(handle,Clock::now());
        }();
        if(!tracked) {
            std::scoped_lock lock{self.m_mutex};
            const bool wanted=(owned || self.m_params_state.all_objects) && self.m_params_state.enabled && self.m_state.allowed;
            const auto now=Clock::now();
            auto* entry=self.m_distance_cache.find(handle);
            if(entry) entry->seen=now;
            if(wanted) {
                // One native rebuild per newly covered cache, never per draw.
                rebuild=!entry && self.m_distance_cache.room(owned,now);
            } else if(entry) {
                rebuild=entry->modified;
                if(!rebuild)self.m_distance_cache.erase(handle);
            }
        }
        if(rebuild && static_cast<int16_t>(handle>>48)>=0) {
            const auto base=static_cast<uintptr_t>(handle&0xffffffffffffULL);
            uintptr_t indices{},meshes{},resource{};int16_t index{-1};uint32_t flags{};SIZE_T count{};
            // Resolve the current native call's mesh exactly as 0x1d7fa70 does.
            // Clear only its GPU-parameter valid bit; native code owns rebuild.
            if(read(base,0x30,indices) && read(base,0x40,meshes) && indices && meshes &&
               ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(indices+(handle>>48)*2),&index,2,&count) && count==2 && index>=0) {
                const auto mesh=meshes+static_cast<uintptr_t>(index)*0x40;
                if(read(mesh,0,resource) && resource && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(mesh+0x20),&flags,4,&count) && count==4) {
                    flags&=~4u;
                    if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(mesh+0x20),&flags,4,&count) || count!=4) {
                        std::scoped_lock lock{self.m_mutex};++self.m_params_state.failures;
                    }
                }
            }
        }
    }catch(...){}
    self.m_refresh(context,handle);
}
void EnginePlayerOpacity::params_hook(uint32_t* output,uint64_t handle,void* material,void* runtime,void* resource) {
    auto& self=get();
    self.m_params(output,handle,material,runtime,resource);
    try {
        const bool owned=self.owns_draw(handle);
        {
            std::scoped_lock lock{self.m_mutex};self.m_distance_cache.erase(handle);
            ++self.m_params_state.builds;
            if(!owned && !self.m_params_state.all_objects)return;
        }
        if(static_cast<int16_t>(handle>>48)<0 || (handle&0xffffffffffffULL)<0x70)return;
        std::array<uint32_t,14> values{};SIZE_T count{};
        if(!output || !ReadProcessMemory(GetCurrentProcess(),output,values.data(),sizeof(values),&count) || count!=sizeof(values))return;
        const auto flags=values[13];float scale{},offset{},padding{};
        std::memcpy(&scale,&values[8],4);std::memcpy(&offset,&values[9],4);std::memcpy(&padding,&values[10],4);
        std::scoped_lock lock{self.m_mutex};auto& state=self.m_params_state;
        if(owned)++state.owned;
        ++state.modes[flags&3];state.flags=flags;
        state.scale=scale;state.offset=offset;state.padding=padding;
        // Native geometry hashedOpacityDistanceOffset reaches GPU offset 0x24.
        // Suppress only POI distance fading. Preserve material alpha, scalar
        // opacity and feature selection, including intentional transparency.
        const bool valid=std::isfinite(scale) && scale>0 && scale<10000 && std::isfinite(offset) && std::abs(offset)<100 && std::isfinite(padding) && padding>0 && padding<10000;
        if(state.enabled && self.m_state.allowed && (owned || state.all_objects)) {
            // Reserve restoration bookkeeping before changing cached GPU data.
            auto* entry=self.m_distance_cache.admit(handle,owned,Clock::now()).first;
            if(!entry) {++state.capacity_skips;return;}
            if(!valid) {++state.invalid;return;}
            const float effective=-100.0f;
            if(WriteProcessMemory(GetCurrentProcess(),output+9,&effective,4,&count) && count==4) {++state.changed;entry->modified=true;}
            else {++state.failures;self.m_distance_cache.erase(handle);}
        }
    }catch(...){}
}
bool EnginePlayerOpacity::owns_draw(uint64_t handle) const {
    // Renderer builds this handle from instance+0x70 and a nonnegative mesh index.
    if(static_cast<int16_t>(handle>>48)<0) return false;
    const auto base=static_cast<uintptr_t>(handle & 0xffffffffffffULL);
    const auto instance=m_candidate.load();if(!instance || base<0x70 || base-0x70!=instance) return false;
    const auto actor=m_actor(Clock::now());uintptr_t selected{},props{},instance_props{},entity{};
    return actor && read(actor,0xd8,selected) && selected==instance && read(actor,0x20,props) && props &&
        read(instance,0x58,instance_props) && props==instance_props && read(actor,0x38,entity) && entity;
}
void EnginePlayerOpacity::draw_hook(void* list,uint64_t handle,uint32_t index,uint32_t flags,void* skeleton,uint8_t* p6,uint8_t* p7,uint8_t* p8,uint8_t* p9,uint8_t* hashed,uint8_t* p11,uint8_t* p12) {
    auto& self=get();bool changed=false;
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    try {
        // Only the verified per-instance renderer caller, not other draw producers.
        if((!self.m_game_base || caller-self.m_game_base==self.m_draw_caller) && self.owns_draw(handle)) {
            std::array<uint8_t,2> option{};SIZE_T count{};
            const bool valid=hashed && ReadProcessMemory(GetCurrentProcess(),hashed,option.data(),option.size(),&count) && count==option.size() && option[0]==1 && option[1]<=1;
            std::scoped_lock lock{self.m_mutex};++self.m_draw_state.owned;
            if(valid && option[1]==1 && self.m_state.enabled && self.m_state.allowed) {
                const uint8_t disabled=0;
                changed=WriteProcessMemory(GetCurrentProcess(),hashed+1,&disabled,1,&count) && count==1;
                if(changed) ++self.m_draw_state.bypassed;else ++self.m_draw_state.failures;
            }
        }
    } catch(...) {}
    // The option belongs to this native call's stack, not the mesh resource. The engine
    // consumes its presence flag normally. Restore only the temporary value afterward.
    self.m_draw(list,handle,index,flags,skeleton,p6,p7,p8,p9,hashed,p11,p12);
    if(changed) {
        const uint8_t original=1;SIZE_T count{};
        if(!WriteProcessMemory(GetCurrentProcess(),hashed+1,&original,1,&count) || count!=1) {
            std::scoped_lock lock{self.m_mutex};++self.m_draw_state.failures;self.m_state.enabled=false;
        }
    }
}
