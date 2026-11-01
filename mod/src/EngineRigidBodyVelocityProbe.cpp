#include "EngineRigidBodyVelocityProbe.h"
#include "EngineBodyProbe.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <intrin.h>

EngineRigidBodyVelocityProbe& EngineRigidBodyVelocityProbe::get(){static auto* p=new EngineRigidBodyVelocityProbe;return *p;}
void EngineRigidBodyVelocityProbe::bind(uintptr_t base,Api api){std::scoped_lock lock{m_mutex};m_base=base;m_api=std::move(api);m_state={};}
bool EngineRigidBodyVelocityProbe::install_in_game(HMODULE module){
    if(m_target)return true;
    const auto base=reinterpret_cast<uintptr_t>(module);if(!base)return false;
    const auto reader=[](uintptr_t p,void* out,size_t n){SIZE_T got{};return p && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,n,&got) && got==n;};
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!reader(base,&dos,sizeof dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || !reader(base+dos.e_lfanew,&nt,sizeof nt) || nt.Signature!=IMAGE_NT_SIGNATURE || !game_build::known(nt.FileHeader.TimeDateStamp))return false;
    const auto stamp=nt.FileHeader.TimeDateStamp;const auto method_rva=game_build::rva(stamp,0xf4fe30),api_rva=game_build::rva(stamp,0x4e08e58),table_rva=game_build::rva(stamp,0x35ea010);
    if(!method_rva || !api_rva || !table_rva)return false;
    const auto target=base+method_rva;
    constexpr game_build::Code expected=game_build::hashed(0xf4fe30,13,"57136c6d3cca5ebb08e675f434de6b90790e5f56a3ea3d8434a71e7f9b9120ea");
    std::array<unsigned char,13> prologue{};if(!reader(target,prologue.data(),prologue.size()) || !game_build::code(stamp,0xf4fe30,prologue,expected))return false;
    uintptr_t api{},table{},method{};
    if(!reader(base+api_rva,&api,8) || !reader(api,&table,8) || table!=base+table_rva || !reader(table+0x50,&method,8) || method!=target)return false;
    bind(base,{reader,[base,reader](uintptr_t cell)->std::optional<Vector>{
        Vector v{};return reader(cell,v.data(),sizeof v)?std::optional{v}:std::nullopt;
    },[base,reader](uintptr_t handle){
        // Exact body user-data getter RVA f503d0 is a single [handle+a0] load.
        uintptr_t packed{};if(!reader(handle+0xa0,&packed,8))return uintptr_t{};
        return (packed&15)==2?packed>>4:0;
    }});
    return install(reinterpret_cast<void*>(target));
}
bool EngineRigidBodyVelocityProbe::install(void* target){
    if(m_target)return true;if(!target || !m_base || !m_api.read || !m_api.vector || !m_api.user_data)return false;
    auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&m_original))!=MH_OK)return false;
    m_target=target;if(MH_EnableHook(target)!=MH_OK){uninstall();return false;}return true;
}
void EngineRigidBodyVelocityProbe::uninstall(){if(m_target){MH_DisableHook(m_target);MH_RemoveHook(m_target);}m_target=nullptr;m_original=nullptr;}
void EngineRigidBodyVelocityProbe::hook(uintptr_t api,uintptr_t body,uintptr_t value){
    auto& self=get();
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(self.m_observe)try{self.observe(body,value,caller);}catch(...){}
    std::optional<Vector> replacement;
    try{if(self.m_override)replacement=self.m_override(body,value,caller);}catch(...){}
    self.m_original(api,body,replacement?reinterpret_cast<uintptr_t>(replacement->data()):value);
}
void EngineRigidBodyVelocityProbe::observe(uintptr_t body,uintptr_t value,uintptr_t caller){
    std::scoped_lock lock{m_mutex};++m_state.calls;
    if(m_state.samples.size()>=256){++m_state.overflow;return;}
    const auto handle=body;const auto velocity=m_api.vector(value);
    if(!handle || !velocity || !std::ranges::all_of(*velocity,[](float v){return std::isfinite(v) && std::abs(v)<=10000;})){++m_state.rejected;return;}
    if(std::ranges::all_of(*velocity,[](float v){return std::abs(v)<.0001f;})){++m_state.rejected;return;}
    Sample s;s.serial=m_state.calls;s.body=body;s.handle=handle;s.velocity=*velocity;
    const auto data=m_api.user_data(handle);
    if(!data){++m_state.rejected;return;}
    // PhysSystem::SetOwner replaces SetGeom's initial weak link with the exact
    // owning Iactor property holder. This is projectile identity, not its thrower.
    s.owner_props=q(q(data+0x18));s.system=q(q(data+0x20));
    s.system_owner=q(q(s.system+0x150));s.caller=caller>=m_base?caller-m_base:0;
    const auto props=s.owner_props,name=q(q(props+8)+0x10)+0x30;
    std::array<unsigned char,16> text{};
    if(read(name,text)){
        uint32_t length{};uintptr_t bytes{};
        if(text[15]&1){std::memcpy(&bytes,text.data(),8);std::memcpy(&length,text.data()+8,4);}
        else if((text[15]>>1)<=15){length=15-(text[15]>>1);bytes=name;}
        if(length && length<=128){s.name.resize(length);if(!m_api.read(bytes,s.name.data(),length))s.name.clear();}
    }
    // Keep the first call and changes in native context or direction. Repeated
    // ballistic updates must not exhaust capture before the next launch.
    if(!m_state.samples.empty()){
        const auto& last=m_state.samples.back();float delta{};
        for(size_t i=0;i<3;++i)delta=std::max(delta,std::abs(last.velocity[i]-s.velocity[i]));
        if(last.handle==s.handle && last.owner_props==s.owner_props && last.name==s.name && delta<2.f)return;
    }
    uint32_t count{};
    if(read(props+0x20,count) && count<=128){
        const auto slots=q(props+0x18);
        for(uint32_t i=0;i<count;++i){Property p;p.address=q(slots+i*8);if(p.address && read(p.address,p.words))s.properties.push_back(p);}
    }
    m_state.samples.push_back(std::move(s));
}
EngineRigidBodyVelocityProbe::Snapshot EngineRigidBodyVelocityProbe::snapshot()const{std::scoped_lock lock{m_mutex};return m_state;}
