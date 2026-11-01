#include "EngineChainswordRev.h"
#include "EngineBodyProbe.h"
#include "GameBuild.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <MinHook.h>

EngineChainswordRev& EngineChainswordRev::get(){static auto* value=new EngineChainswordRev;return *value;}
bool EngineChainswordRev::install_in_game(HMODULE module,std::function<Request()> request){
    if(m_target)return true;if(!request || !bind_in_game(module))return false;
    const auto update=game_build::rva(0x1880e90);if(!update)return false;
    const auto target=reinterpret_cast<uintptr_t>(module)+update;
    constexpr game_build::Code expected=game_build::hashed(0x1880e90,13,"bf1249181d4f31e8f9076a79adbf9906e711d4a7249a5a16f987b5d7e718c2f0");
    std::array<unsigned char,expected.size()> got{};if(!m_api.read(target,got.data(),got.size()) || !game_build::code(0x1880e90,got,expected))return false;
    return install(reinterpret_cast<void*>(target),std::move(request));
}
bool EngineChainswordRev::install(void* target,std::function<Request()> request){
    if(m_target)return true;if(!target || !m_state.bound || !request)return false;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&update_hook),reinterpret_cast<void**>(&m_original))!=MH_OK)return false;
    m_request=std::move(request);m_target=target;
    if(MH_EnableHook(m_target)!=MH_OK){uninstall();return false;}return true;
}
void EngineChainswordRev::uninstall(){if(m_target){MH_DisableHook(m_target);MH_RemoveHook(m_target);}m_target=nullptr;m_original=nullptr;m_request={};std::scoped_lock lock{m_mutex};m_audio.reset();}
void EngineChainswordRev::update_hook(uintptr_t property){
    auto& self=get();self.m_original(property);
    // Serialize command queues with this exact property's native animation job.
    // Physics body jobs may run concurrently and must not enqueue animation work.
    if(self.m_request)self.native_update(property,self.m_request(),Clock::now());
}
void EngineChainswordRev::native_update(uintptr_t property,const Request& request,Clock::time_point now){
    uintptr_t release_actor{};
    {std::scoped_lock lock{m_mutex};
        if(!m_state.bound)return;
        // Every native prop animation update lands here. Idle with the trigger released is
        // the common case: return before any ownership reads.
        if(!m_state.property && !rev_held(request.force,false))return;
        if(m_state.property==property && (!request.actor || request.actor!=m_state.actor || !rev_held(request.force,true))){
            m_audio.update(false);
            // A live native update establishes that the old property still exists.
            // Only touch it if it still belongs to its original actor.
            if(owned(property,m_state.actor))release_actor=m_state.actor;
            else {m_state.property=m_state.actor=0;++m_state.refused;return;}
        }else if(!owned(property,request.actor))return;
        else if(m_state.property && m_state.property!=property){
            // Another of the actor's chainsword props is updating. Commands for the revving
            // prop wait for its own update; a revving prop that no longer belongs to the actor
            // (the weapon was replaced) is dropped so the rev moves to this one.
            if(owned(m_state.property,m_state.actor))return;
            m_audio.update(false);m_state.property=m_state.actor=0;++m_state.refused;
        }
    }
    if(release_actor){tick(release_actor,release_actor,0,now);return;}
    // This update's property is the actor's chainsword (checked above): a rev starts on it.
    tick(request.actor,request.actor,request.force,now,property);
}
void EngineChainswordRev::bind_audio(EngineChainswordAudio::Api api){std::scoped_lock lock{m_mutex};m_audio.bind(std::move(api));}
void EngineChainswordRev::release_audio_unless(uintptr_t actor,float request){std::scoped_lock lock{m_mutex};if(!actor || actor!=m_state.actor || !rev_held(request,true))m_audio.update(false);}
EngineChainswordAudio::Snapshot EngineChainswordRev::audio_snapshot()const{std::scoped_lock lock{m_mutex};return m_audio.snapshot();}
void EngineChainswordRev::bind(uintptr_t base,Api api){std::scoped_lock lock{m_mutex};m_audio.reset();m_base=base;m_api=std::move(api);m_state={};m_state.bound=base && m_api.read && m_api.exists && m_api.start && m_api.stop && m_api.time;}
bool EngineChainswordRev::bind_in_game(HMODULE module){
    const auto base=reinterpret_cast<uintptr_t>(module);if(!base)return false;
    const auto reader=[](uintptr_t p,void* out,size_t size){SIZE_T n{};return p && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,size,&n) && n==size;};
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!reader(base,&dos,sizeof dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>4096 || !reader(base+dos.e_lfanew,&nt,sizeof nt) || nt.Signature!=IMAGE_NT_SIGNATURE || !game_build::supported(nt.FileHeader.TimeDateStamp) || nt.OptionalHeader.SizeOfImage<0x4f17760)return false;
    constexpr std::array<game_build::Code,4> signatures{
        game_build::hashed(0x1881a30,8,"f7b38d35bd0ef6200fdc101d1fd5be63d0c6b9c1d4af1a327142c791402ed3a3"),
        game_build::hashed(0x1881a70,8,"f27e61cde60a4601d859892e0a5b5d231b72545798c27aafdbf19b716a906402"),
        game_build::hashed(0x1881cb0,8,"bb89e4bbb1e6d3321166a942820fc6527308fcd7caf8029490b53eed4a9c0df8"),
        game_build::hashed(0x18827e0,8,"95d8c2e06c58d0e8002fce9b83de7a1f8a22dfa406211327354fb496544293a2")};
    for(const auto& want:signatures){std::array<unsigned char,8> got{};const auto at=game_build::rva(want.reference);if(!at || !reader(base+at,got.data(),got.size()) || !game_build::code(want.reference,got,want))return false;}
    const auto exists=reinterpret_cast<bool(*)(uintptr_t,const String*)>(base+game_build::rva(0x1881a30));
    const auto start=reinterpret_cast<void(*)(uintptr_t,const String*,float,const String*,bool)>(base+game_build::rva(0x1881a70));
    const auto stop=reinterpret_cast<void(*)(uintptr_t,const String*)>(base+game_build::rva(0x1881cb0));
    const auto time=reinterpret_cast<float(*)(uintptr_t)>(base+game_build::rva(0x18827e0));
    // PropAnim SetPlayRate: movss xmm0,[min]; comiss xmm1,xmm0; ja; movss [rcx+28],xmm0.
    // Prop-wide rate (rcx=prop, xmm1=rate), clamped natively. Optional: unverified bytes disable it.
    std::function<void(uintptr_t,float)> rate;
    {constexpr game_build::Code head=game_build::hashed(0x1882910,4,"5fbc9b81f6e319baf016b8b3a0916fef0a49a0856c91cf3ba6c5f2e019bb33fe");constexpr game_build::Code tail=game_build::hashed(0x1882918,10,"c0914e47bb08901d732d5064351b32c7040a0154f2cdf9626275ac4fdb5fab02");
     std::array<unsigned char,4> h{};std::array<unsigned char,10> t{};
     const auto set_rate=game_build::rva(0x1882910),set_tail=game_build::rva(0x1882918);
     if(set_rate && set_tail && reader(base+set_rate,h.data(),h.size()) && reader(base+set_tail,t.data(),t.size()) && game_build::code(0x1882910,h,head) && game_build::code(0x1882918,t,tail))
        rate=[set=reinterpret_cast<void(*)(uintptr_t,float)>(base+set_rate)](uintptr_t p,float value){set(p,value);};}
    bind(base,{reader,[exists](auto p,const auto& s){return exists(p,&s);},[start](auto p,const auto& s,float blend,const auto& mode,bool immediate){start(p,&s,blend,&mode,immediate);},[stop](auto p,const auto& s){stop(p,&s);},time,rate});
    // These are actual named exports in this verified executable, not inferred VM
    // methods. The native bank contains both events (FNV IDs 7fdc32f6/8ef50d76).
    const auto exported=[&](const char* name,uintptr_t rva){const auto p=GetProcAddress(module,name);const auto at=game_build::rva(rva);return at && reinterpret_cast<uintptr_t>(p)==base+at?p:nullptr;};
    const auto ready=reinterpret_cast<bool(*)()>(exported("?IsInitialized@SoundEngine@AK@@YA_NXZ",0x31678c0));
    const auto reg=reinterpret_cast<int(*)(uint64_t)>(exported("?RegisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_K@Z",0x316be80));
    const auto post=reinterpret_cast<uint32_t(*)(const char*,uint64_t,uint32_t,void*,void*,uint32_t,void*,uint32_t)>(exported("?PostEvent@SoundEngine@AK@@YAIPEBD_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z",0x3169490));
    const auto stop_id=reinterpret_cast<void(*)(uint32_t,int,int)>(exported("?StopPlayingID@SoundEngine@AK@@YAXIHW4AkCurveInterpolation@@@Z",0x3170c40));
    const auto unreg=reinterpret_cast<int(*)(uint64_t)>(exported("?UnregisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_K@Z",0x3171de0));
    const auto playing=reinterpret_cast<int(*)(uint64_t,uint32_t*,uint32_t*)>(exported("?GetPlayingIDsFromGameObject@Query@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEAIPEAI@Z",0x3172f00));
    const auto listeners=reinterpret_cast<int(*)(uint64_t,uint64_t*,uint32_t*)>(exported("?GetListeners@Query@SoundEngine@AK@@YA?AW4AKRESULT@@_KPEA_KAEAI@Z",0x3172c80));
    if(ready && reg && post && stop_id && unreg)bind_audio({ready,[reg](auto id){return reg(id)==1;},[post](const char* event,auto id){return post(event,id,0,nullptr,nullptr,0,nullptr,0);},[stop_id](auto id){stop_id(id,80,4);},[unreg](auto id){unreg(id);},
        [playing](auto object,auto id){std::array<uint32_t,16> ids{};uint32_t count=uint32_t(ids.size());return playing && playing(object,&count,ids.data())==1 && count<=ids.size() && std::find(ids.begin(),ids.begin()+std::min<size_t>(count,ids.size()),id)!=ids.begin()+std::min<size_t>(count,ids.size());},
        [listeners](auto object){uint32_t count{};return listeners && listeners(object,nullptr,&count)<=2 && count<=16?int(count):-1;},
        // The chainsword banks expose no rev RTPC, so intensity is our own emitter's gain
        // toward each of its actual listeners (never a game-owned object or bus).
        [listeners,volume=reinterpret_cast<int(*)(uint64_t,uint64_t,float)>(exported("?SetGameObjectOutputBusVolume@SoundEngine@AK@@YA?AW4AKRESULT@@_K0M@Z",0x316ef10))](auto object,float gain){
            if(!listeners || !volume)return false;std::array<uint64_t,16> ids{};uint32_t count=uint32_t(ids.size());
            if(listeners(object,ids.data(),&count)!=1 || !count || count>ids.size())return false;
            bool ok=true;for(uint32_t i=0;i<count;++i)ok=volume(object,ids[i],gain)==1 && ok;return ok;}});
    return true;
}
std::optional<uintptr_t> EngineChainswordRev::geometry_property(uintptr_t props)const{
    const auto ids=q(m_base+game_build::rva(0x4e0e470));int count{};if(!read(m_base+game_build::rva(0x4e0e478),count) || count<=0 || count>256)return {};
    uint16_t type{},slot{};if(!read(ids+(count-1)*2,type) || !read(q(q(props+0x28)+0x10)+type*2,slot))return {};
    uint32_t size{};if(!read(props+0x20,size) || size>256 || slot>=size)return {};
    const auto prop=q(q(props+0x18)+slot*8);return prop?std::optional{prop}:std::nullopt;
}
bool EngineChainswordRev::owned(uintptr_t prop,uintptr_t actor)const{
    if(!actor || q(prop)!=m_base+game_build::rva(0x363fd10))return false;
    const auto props=q(prop+0x20),geometry=q(prop+0x30),player_geometry=q(actor+0xd8);
    if(!props || !geometry || !player_geometry || q(geometry+0x58)!=props)return false;
    // Same native actor-name getter as PropAnimBase; bounded small-string read.
    const auto name=q(q(props+8)+0x10)+0x30;String text{std::string_view{}};
    if(!read(name,text))return false;const auto flag=text.bytes[15];
    size_t length=flag&1?0:15-(flag>>1);uint32_t large{};uintptr_t data{};
    if(flag&1){std::memcpy(&large,text.bytes.data()+8,4);std::memcpy(&data,text.bytes.data(),8);length=large;}else data=name;
    constexpr std::string_view wanted="wpn_melee_chainsword";std::array<char,wanted.size()> buffer{};
    if(length!=wanted.size() || !m_api.read(data,buffer.data(),buffer.size()) || std::string_view{buffer.data(),buffer.size()}!=wanted)return false;
    const auto geom=geometry_property(props);if(!geom || q(*geom+0xd8)!=geometry)return false;
    const auto tracker=q(q(*geom+0x38)+0x20);
    if(q(q(tracker)+0x48)!=m_base+game_build::rva(0x134c2e0))return false;
    const auto packed=q(tracker+0xd8);
    return (packed&0xffffffffffffull)==player_geometry && static_cast<int16_t>(packed>>48)>=0 && q(geometry+0x68)!=0;
}
std::pair<EngineChainswordRev::Found,uintptr_t> EngineChainswordRev::find(uintptr_t actor)const{
    const auto manager=q(m_base+game_build::rva(0x4f17758)),array=q(manager+0x940);uint32_t count{},capacity{};
    // Bound the entries walked, not the allocation: capacity only grows, and once a big level
    // (the Battle Barge) took it past 256 the old capacity bound refused every rev for good.
    if(!read(manager+0x948,count) || !read(manager+0x94c,capacity) || count>capacity || count>MAX_PROPS)return {Found::table,0};
    uintptr_t result{};
    for(uint32_t i=0;i<count;++i){const auto prop=q(array+i*16);if(owned(prop,actor)){if(result && result!=prop)return {Found::ambiguous,0};result=prop;}}
    return {result?Found::prop:Found::missing,result};
}
void EngineChainswordRev::tick(uintptr_t actor,uintptr_t active,float request,Clock::time_point,uintptr_t updating){
    std::scoped_lock lock{m_mutex};if(!m_state.bound)return;
    if(m_state.property && (actor!=m_state.actor || actor!=active || !owned(m_state.property,actor))){m_audio.update(false);m_state.property=0;m_state.actor=0;++m_state.refused;}
    const bool down=rev_held(request,m_state.property!=0) && actor && actor==active;
    const String chain{"chain_anim"},stop{"chain_anim_stop"},cycle{"cycle"},clamp{"clamp"};
    if(down && !m_state.property){
        // The prop whose native update is running needs no search when it is the actor's
        // chainsword. Otherwise (a call from outside the hook) the global list is searched.
        auto [found,prop]=updating && owned(updating,actor)?std::pair{Found::prop,updating}:find(actor);
        if(!prop){++m_state.refused;auto& r=m_state.reasons;++(found==Found::table?r.table:found==Found::ambiguous?r.ambiguous:r.missing);return;}
        if(!m_api.exists(prop,chain) || !m_api.exists(prop,stop)){++m_state.refused;++m_state.reasons.animation;return;}
        m_api.start(prop,chain,0,cycle,false);m_state.actor=actor;m_state.property=prop;++m_state.starts;m_audio.update(true);
    }else if(!down && m_state.property){
        m_audio.update(false);
        // Restore native speed first so the authored wind-down plays at its own pace.
        if(m_api.rate){m_api.rate(m_state.property,1);m_state.play_rate=1;}
        m_api.stop(m_state.property,chain);m_api.start(m_state.property,stop,0,clamp,false);m_state.property=0;m_state.actor=0;++m_state.stops;
    }
    if(m_state.property){
        m_audio.update(down);m_audio.intensity(rev_amount(request));
        // Trigger depth sets teeth speed every native update while revving.
        if(m_api.rate){m_state.play_rate=rev_rate(request);m_api.rate(m_state.property,m_state.play_rate);}
        const auto t=m_api.time(m_state.property);if(std::isfinite(t))m_state.base_layer_time=t;
    }
}
EngineChainswordRev::Snapshot EngineChainswordRev::snapshot()const{std::scoped_lock lock{m_mutex};return m_state;}
