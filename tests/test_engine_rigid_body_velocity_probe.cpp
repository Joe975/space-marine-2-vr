#include "EngineRigidBodyVelocityProbe.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
volatile unsigned native_calls{};uintptr_t native_api{},native_body{},native_value{};
EngineRigidBodyVelocityProbe::Vector native_vector{};
__declspec(noinline) void native_velocity(uintptr_t api,uintptr_t body,uintptr_t value){native_api=api;native_body=body;native_value=value;if(value!=0x40000)std::memcpy(native_vector.data(),reinterpret_cast<void*>(value),sizeof native_vector);native_calls=native_calls+1;}
int main(){try{
    constexpr uintptr_t base=0x10000000,body=0x20000,handle=body,cell=0x40000;
    std::map<uintptr_t,unsigned char> memory;
    const auto bytes=[&](uintptr_t p,const void* data,size_t n){for(size_t i=0;i<n;++i)memory[p+i]=static_cast<const unsigned char*>(data)[i];};
    const auto put=[&]<class T>(uintptr_t p,T value){bytes(p,&value,sizeof value);};
    const auto reader=[&](uintptr_t p,void* out,size_t n){for(size_t i=0;i<n;++i){auto it=memory.find(p+i);if(it==memory.end())return false;static_cast<unsigned char*>(out)[i]=it->second;}return true;};
    put(body,base+0x3600b78);put(body+0xe0,handle);
    put(0x50018,uintptr_t{0x60000});put(0x60000,uintptr_t{0x70000});put(0x50020,uintptr_t{0x80000});put(0x80000,uintptr_t{0x90000});
    put(0x70008,uintptr_t{0xb0000});put(0xb0010,uintptr_t{0xc0000});
    put(0x90150,uintptr_t{0xd0000});put(0xd0000,uintptr_t{0x70000});
    std::array<unsigned char,16> name{};std::memcpy(name.data(),"grenade_frag",12);name[15]=6;bytes(0xc0030,name.data(),name.size());
    auto& probe=EngineRigidBodyVelocityProbe::get();std::optional<EngineRigidBodyVelocityProbe::Vector> velocity=EngineRigidBodyVelocityProbe::Vector{1,2,3};
    probe.bind(base,{reader,[&](auto p){require(p==cell,"reads the exact original velocity pointer");return velocity;},[&](auto p){return p==handle?uintptr_t{0x50000}:0;}});
    const auto before=memory;
    require(probe.install(reinterpret_cast<void*>(&native_velocity)),"fixture installs the real production hook");
    native_velocity(base,body,cell);require(native_calls==1 && native_api==base && native_body==body && native_value==cell,"probe passes original arguments through exactly once");
    auto s=probe.snapshot();require(s.samples.size()==1 && s.samples[0].velocity==*velocity && s.samples[0].name=="grenade_frag" && s.samples[0].owner_props==0x70000 && s.samples[0].system==0x90000 && s.samples[0].system_owner==0x70000,"native setter observation resolves direct Iactor ownership without confusing projectile and thrower");
    velocity->at(1)=std::numeric_limits<float>::quiet_NaN();native_velocity(base,body,cell);
    require(native_calls==2 && probe.snapshot().rejected==1,"invalid telemetry cannot suppress the native call");
    velocity.reset();native_velocity(base,body,cell);velocity=EngineRigidBodyVelocityProbe::Vector{1,2,3};native_velocity(base,body+8,cell);require(native_calls==4 && probe.snapshot().rejected==3,"unreadable vector and unrelated body fail closed for telemetry only");
    velocity=EngineRigidBodyVelocityProbe::Vector{};native_velocity(base,body,cell);require(native_calls==5 && probe.snapshot().samples.size()==1,"stationary initialization cannot fill the bounded moving-body capture");velocity=EngineRigidBodyVelocityProbe::Vector{1,2,3};
    for(int i=0;i<20;++i)native_velocity(base,body,cell);
    require(probe.snapshot().samples.size()==1,"repeated ballistic velocities do not consume bounded context capture");
    for(int i=0;i<300;++i){velocity->at(0)=float(i*3+4);native_velocity(base,body,cell);}
    s=probe.snapshot();require(s.samples.size()==256 && s.overflow==45 && native_calls==325 && memory==before,"bounded diagnostics never mutate memory or limit native gameplay calls");
    probe.set_launch_override([&](auto b,auto v,auto caller)->std::optional<EngineRigidBodyVelocityProbe::Vector>{require(b==body && v==cell && caller!=0,"launch callback gets exact original context");return EngineRigidBodyVelocityProbe::Vector{7,8,9};});
    native_velocity(base,body,cell);require(native_calls==326 && native_body==body && native_api==base && native_value!=cell && native_vector==EngineRigidBodyVelocityProbe::Vector{7,8,9},"the actual native call consumes the scoped temporary replacement exactly once");
    probe.set_launch_override([](auto,auto,auto)->std::optional<EngineRigidBodyVelocityProbe::Vector>{throw std::runtime_error("probe failure");});
    native_velocity(base,body,cell);require(native_calls==327 && native_value==cell,"callback failure preserves the untouched native call exactly once");
    // Default grenade aim runs the override without sample capture (no per-call reads).
    probe.set_observe(false);const auto counted=probe.snapshot().calls;
    probe.set_launch_override([&](auto,auto,auto)->std::optional<EngineRigidBodyVelocityProbe::Vector>{return EngineRigidBodyVelocityProbe::Vector{4,5,6};});
    native_velocity(base,body,cell);require(native_calls==328 && native_vector==EngineRigidBodyVelocityProbe::Vector{4,5,6} && probe.snapshot().calls==counted,"observation off: override still reaches native, nothing sampled");
    probe.set_launch_override({});native_velocity(base,body,cell);require(native_calls==329 && native_value==cell,"observation and override off: plain passthrough");
    probe.set_observe(true);
    probe.set_launch_override({});probe.uninstall();native_velocity(base,body,cell);require(native_calls==330 && probe.snapshot().calls==327,"uninstall restores the native function");
    std::cout<<"rigid body velocity observation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
