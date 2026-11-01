#include "EngineParryProbe.h"
#include "GameBuild.h"
#include "VrParryEvents.h"
#include "VrTrace.h"
#include <MinHook.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

namespace {
constexpr uintptr_t POST_NAME_RVA=0x3169490,POST_ID_RVA=0x3169190,FIRE_RVA=0xe3e350,GET_POSITION_RVA=0x3172f30;
constexpr game_build::Code POST_NAME=game_build::hashed(0x3169490,16,"646e4954a4523e09b98d0d09b43f9a076351219453fe02cd88016a94ba3442d9");
constexpr game_build::Code POST_ID=game_build::hashed(0x3169190,16,"61dcf010537cb21124e1aa1adb133281cee143434cf8c0d93f465633abb26b26");
constexpr game_build::Code FIRE=game_build::hashed(0xe3e350,16,"c18b47348df7a95c3a588b21105374df91494a216c8b0af55377e77f363061ea");
constexpr game_build::Code GET_POSITION=game_build::hashed(0x3172f30,16,"4ef11297de5479598d2b08c05bcc25bb1bc876055e22d5216d2630772f45be8a");
constexpr const char* GET_POSITION_EXPORT="?GetPosition@Query@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEAVAkWorldTransform@@@Z";
// AK::SoundEngine::Query::GetPosition(game object, AkWorldTransform&): 48 bytes out, AK_Success = 1.
using GetPosition=int(*)(uint64_t,void*);
std::atomic<GetPosition> g_get_position{};
constexpr const char* POST_NAME_EXPORT="?PostEvent@SoundEngine@AK@@YAIPEBD_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z";
constexpr const char* POST_ID_EXPORT="?PostEvent@SoundEngine@AK@@YAII_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z";
// The name overload may post through the ID one: count each sound once.
thread_local int t_posting{};

// An engine string (SSO, 16 bytes): heap when bit 0 of byte +0xf is set (pointer +0, length +8),
// otherwise inline with length 0xf - (flag >> 1). Copies at most size-1 characters; false on a fault.
bool read_string(uintptr_t at,char* out,size_t size) {
    __try {
        const auto flag=*reinterpret_cast<const uint8_t*>(at+0xf);
        const char* text{};size_t length{};
        if(flag&1) { text=*reinterpret_cast<const char* const*>(at);length=*reinterpret_cast<const size_t*>(at+8); }
        else { text=reinterpret_cast<const char*>(at);length=size_t(0xf-(flag>>1)); }
        if(!text || length>4096) return false;
        length=length<size-1?length:size-1;
        std::memcpy(out,text,length);out[length]=0;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The event's owner: h = ev+8, controller = *h, wrapper = controller+0xe8, animINST = wrapper+0x10.
// The animINST's name is a string at +0x308; its evaluated root (C0[0]) translation is the position.
bool read_attacker(uintptr_t ev,uintptr_t& instance,char* name,size_t size,float* position) {
    __try {
        const auto h=*reinterpret_cast<const uintptr_t*>(ev+8);if(!h) return false;
        const auto controller=*reinterpret_cast<const uintptr_t*>(h);if(!controller) return false;
        const auto wrapper=*reinterpret_cast<const uintptr_t*>(controller+0xe8);if(!wrapper) return false;
        instance=*reinterpret_cast<const uintptr_t*>(wrapper+0x10);if(!instance) return false;
        if(!read_string(instance+0x308,name,size)) name[0]=0;
        const auto c0=*reinterpret_cast<const uintptr_t*>(instance+0xc0);
        if(c0>=0x10000 && c0<0x800000000000) std::memcpy(position,reinterpret_cast<const void*>(c0+0x30),3*sizeof(float));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// A posted event name, copied (at most size-1 characters); false on a fault or a longer name.
bool copy_name(const char* name,char* out,size_t size) {
    __try {
        if(!name) return false;
        size_t i=0;for(;i+1<size && name[i];++i) out[i]=name[i];
        out[i]=0;return name[i]==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The sound object's Wwise transform (48 bytes, logged raw as hex: the decoding is left to the
// analysis, scripts/parry_timeline.py), or nothing when the query fails or faults.
bool read_position(uint64_t object,std::array<uint8_t,48>& out) {
    const auto query=g_get_position.load();if(!query) return false;
    __try { return query(object,out.data())==1; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void to_hex(uint64_t object,char (&hex)[97]) {
    std::array<uint8_t,48> transform{};hex[0]=0;
    if(read_position(object,transform)) for(size_t i=0;i<transform.size();++i) std::snprintf(hex+2*i,3,"%02x",transform[i]);
}
// The last indicator's sound object (the attacker's) and when: an outcome within 3 s also logs where
// that attacker is at the moment of the parry or hit.
std::atomic<uint64_t> g_indicator_object{};std::atomic<int64_t> g_indicator_at{};
void note_sound(const vr_parry::Sound& s,uint64_t object) {
    char hex[97]{},attacker[97]{};to_hex(object,hex);
    const auto now=std::chrono::steady_clock::now().time_since_epoch().count();
    if(s.label.starts_with("indicator")) { g_indicator_object.store(object);g_indicator_at.store(now); }
    else if(const auto last=g_indicator_object.load();last && now-g_indicator_at.load()<=std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(3)).count()) to_hex(last,attacker);
    char detail[384];std::snprintf(detail,sizeof detail,"%.*s %.*s object=%llx transform=%s%s%s",int(s.label.size()),s.label.data(),int(s.name.size()),s.name.data(),static_cast<unsigned long long>(object),
        hex[0]?hex:"-",attacker[0]?" attacker_transform=":"",attacker);
    vr_trace::Recorder::get().note("parry_sound",detail);
}
// The first distinct sounds posted (name, or ID in hex), bounded, so a session shows what the game
// actually posts even when it is not in VrParryEvents' list.
void note_first_sound(std::atomic<int>& seen,std::mutex& mutex,std::vector<uint32_t>& ids,uint32_t id,const char* name) {
    if(seen.load()>=64) return;
    {std::lock_guard lock{mutex};for(auto x:ids) if(x==id) return;if(ids.size()>=64) return;ids.push_back(id);}
    ++seen;char detail[96];
    if(name) std::snprintf(detail,sizeof detail,"%s",name);else std::snprintf(detail,sizeof detail,"id=%08x",id);
    vr_trace::Recorder::get().note("sound_posted",detail);
}
}

EngineParryProbe& EngineParryProbe::get() { static auto* value=new EngineParryProbe;return *value; }

bool EngineParryProbe::install_in_game(HMODULE module) {
    if(installed()) return true;
    auto* base=reinterpret_cast<uint8_t*>(module);if(!base) return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;
    const auto name_at=game_build::rva(stamp,POST_NAME_RVA),id_at=game_build::rva(stamp,POST_ID_RVA),fire_at=game_build::rva(stamp,FIRE_RVA);
    if(!name_at || !id_at || !fire_at) return false;
    if(reinterpret_cast<uint8_t*>(GetProcAddress(module,POST_NAME_EXPORT))!=base+name_at || reinterpret_cast<uint8_t*>(GetProcAddress(module,POST_ID_EXPORT))!=base+id_at) return false;
    if(!game_build::code(stamp,POST_NAME_RVA,{base+name_at,POST_NAME.size()},POST_NAME) || !game_build::code(stamp,POST_ID_RVA,{base+id_at,POST_ID.size()},POST_ID) ||
       !game_build::code(stamp,FIRE_RVA,{base+fire_at,FIRE.size()},FIRE)) return false;
    // Optional: the sound objects' positions (the attacker's bearing). A mismatch only drops them.
    if(const auto at=game_build::rva(stamp,GET_POSITION_RVA);at && reinterpret_cast<uint8_t*>(GetProcAddress(module,GET_POSITION_EXPORT))==base+at &&
       game_build::code(stamp,GET_POSITION_RVA,{base+at,GET_POSITION.size()},GET_POSITION)) g_get_position.store(reinterpret_cast<GetPosition>(base+at));
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    void* targets[3]{base+name_at,base+id_at,base+fire_at};
    void* detours[3]{reinterpret_cast<void*>(&post_name_hook),reinterpret_cast<void*>(&post_id_hook),reinterpret_cast<void*>(&fire_hook)};
    void** originals[3]{reinterpret_cast<void**>(&m_post_name),reinterpret_cast<void**>(&m_post_id),reinterpret_cast<void**>(&m_fire)};
    int created=0;
    for(;created<3;++created) if(MH_CreateHook(targets[created],detours[created],originals[created])!=MH_OK) break;
    bool ok=created==3;
    for(int i=0;ok && i<3;++i) ok=MH_EnableHook(targets[i])==MH_OK;
    if(!ok) { for(int i=0;i<created;++i) { MH_DisableHook(targets[i]);MH_RemoveHook(targets[i]); } m_post_name=nullptr;m_post_id=nullptr;m_fire=nullptr;return false; }
    return true;
}

uint32_t EngineParryProbe::post_name_hook(const char* name,uint64_t object,uint32_t flags,void* callback,void* cookie,uint32_t externals,void* sources,uint32_t playing) {
    auto& self=get();++self.m_sound_calls;
    std::optional<vr_parry::Sound> s;
    char copy[64]{};
    if(copy_name(name,copy,sizeof copy)) { s=vr_parry::sound(copy);note_first_sound(self.m_sounds_seen,self.m_sounds_mutex,self.m_sound_ids,vr_parry::fnv1(copy),copy); }
    ++t_posting;const auto result=self.m_post_name(name,object,flags,callback,cookie,externals,sources,playing);--t_posting;
    if(s) { ++self.m_sounds;note_sound(*s,object); }
    return result;
}

uint32_t EngineParryProbe::post_id_hook(uint32_t id,uint64_t object,uint32_t flags,void* callback,void* cookie,uint32_t externals,void* sources,uint32_t playing) {
    auto& self=get();
    const auto s=t_posting?std::nullopt:vr_parry::sound(id);
    if(!t_posting) { ++self.m_sound_calls;note_first_sound(self.m_sounds_seen,self.m_sounds_mutex,self.m_sound_ids,id,nullptr); }
    const auto result=self.m_post_id(id,object,flags,callback,cookie,externals,sources,playing);
    if(s) { ++self.m_sounds;note_sound(*s,object); }
    return result;
}

void EngineParryProbe::fire_hook(void* ev,void* track) {
    auto& self=get();++self.m_event_calls;
    char event[64]{};
    const auto e=reinterpret_cast<uintptr_t>(ev);
    const bool named=e && read_string(e+0x30,event,sizeof event);
    const auto kind=named?vr_parry::indication(event):std::nullopt;
    // The first distinct event names, to confirm the name's offset live (bounded, trace only).
    if(named && self.m_names_seen.load()<32) {
        const auto h=vr_parry::fnv1(event);bool fresh=true;
        {std::lock_guard lock{self.m_names_mutex};for(auto x:self.m_names) if(x==h) fresh=false;if(fresh && self.m_names.size()<32) self.m_names.push_back(h);else fresh=false;}
        if(fresh) { ++self.m_names_seen;vr_trace::Recorder::get().note("morpheme_event_name",event); }
    }
    if(kind) {
        uintptr_t instance{};char actor[64]{};float position[3]{};
        if(read_attacker(e,instance,actor,sizeof actor,position)) {
            ++self.m_indications;
            char detail[256];std::snprintf(detail,sizeof detail,"%.*s actor=%s inst=%llx pos=[%.3f,%.3f,%.3f]",int(kind->size()),kind->data(),actor,
                static_cast<unsigned long long>(instance),position[0],position[1],position[2]);
            vr_trace::Recorder::get().note("attack_event",detail);
        }
    }
    self.m_fire(ev,track);
}
