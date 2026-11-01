#include "NativeGrenadeLaunch.h"
#include <iostream>
#include <map>
#include <stdexcept>
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
int main(){try{
    constexpr uintptr_t base=0x10000000,body=0x20000,data=0x30000,props=0x40000,actor=0x50000,player_props=0x60000,player_vm=0x70000,equipment=0x80000,vm=0x90000,value=0xa0000,slots=0xb0000;
    std::map<uintptr_t,unsigned char> memory;
    auto put=[&]<class T>(uintptr_t p,T v){const auto* b=reinterpret_cast<const unsigned char*>(&v);for(size_t i=0;i<sizeof v;++i)memory[p+i]=b[i];};
    auto reader=[&](uintptr_t p,void* out,size_t size){for(size_t i=0;i<size;++i){auto it=memory.find(p+i);if(it==memory.end())return false;static_cast<unsigned char*>(out)[i]=it->second;}return true;};
    uintptr_t next=0x100000;
    auto name=[&](uintptr_t p,std::string_view n){const auto t=next;next+=0x100;const auto d=next;next+=0x100;const auto bytes=next;next+=0x100;put(p+8,t);put(t+0x10,d);std::array<unsigned char,16> s{};uint32_t length=uint32_t(n.size());std::memcpy(s.data(),&bytes,8);std::memcpy(s.data()+8,&length,4);s[15]=1;put(d+0x30,s);for(size_t i=0;i<n.size();++i)put(bytes+i,n[i]);};
    name(props,"grenade_frag_client");name(equipment,"prop_equipment_client");
    put(body+0xa0,(data<<4)|2ull);put(data+0x18,uintptr_t{0xc0000});put(0xc0000,props);
    put(props+0x18,slots);put(props+0x20,uint32_t{1});put(slots,equipment);put(equipment+0x20,props);put(equipment+0x18,vm);put(equipment+0x10,uintptr_t{0xd0000});put(0xd0000,equipment);put(vm+0x18,uintptr_t{0xd0000});
    put(actor+0x20,player_props);put(player_props+0x10,player_vm);put(player_props+0x30,uintptr_t{0xe0000});put(player_vm+0x18,uintptr_t{0xe0000});put(0xe0000,player_props);put(vm+0xc0,(player_vm<<3)|6ull);
    const NativeGrenadeLaunch::Vector speed{0,3,4};put(value,speed);const auto original=memory;
    NativeGrenadeLaunch candidate;uintptr_t active=actor;candidate.bind(base,reader,[&]{return active;});const auto now=NativeGrenadeLaunch::Clock::now();
    VrWeaponAim::Pose pose{actor,5,1,now,vr_camera::IDENTITY,0};
    candidate.publish(pose,now);
    require(!candidate.rewrite(body,value,base+0x126c0b6,now),"flight updates never redirect");
    auto output=candidate.rewrite(body,value,base+0x12aed3e,now);require(output && std::abs(output->at(2)-5)<.0001f,"first launch preserves native speed and uses frozen throw direction");
    require(!candidate.rewrite(body,value,base+0x12aed3e,now),"one throw consumes exactly one initial launch");
    for(int yaw:{90,180}){++pose.sequence;pose.world=vr_arm::turn({0,0,1},{yaw==90?1.f:0.f,0,yaw==180?-1.f:0.f});candidate.publish(pose,now);output=candidate.rewrite(body,value,base+0x12aed3e,now);require(output && std::abs(output->at(yaw==90?0:2)-(yaw==90?5.f:-5.f))<.0001f,"reused native addresses accept a distinct throw in the new heading");}
    ++pose.sequence;candidate.publish(pose,now);put(vm+0xc0,uintptr_t{(0xf0000<<3)|6});require(!candidate.rewrite(body,value,base+0x12aed3e,now),"NPC/other player grenade cannot consume local throw");put(vm+0xc0,(player_vm<<3)|6ull);
    put(vm+0x18,uintptr_t{0xf0000});require(!candidate.rewrite(body,value,base+0x12aed3e,now),"recycled projectile VM identity refused");put(vm+0x18,uintptr_t{0xd0000});
    put(0xe0000,uintptr_t{actor});require(!candidate.rewrite(body,value,base+0x12aed3e,now),"stale player native identity refused");put(0xe0000,player_props);
    put(equipment+0x20,uintptr_t{actor});require(!candidate.rewrite(body,value,base+0x12aed3e,now),"equipment must belong to this exact projectile");put(equipment+0x20,props);
    put(props+0x20,uint32_t{2});put(slots+8,equipment);require(!candidate.rewrite(body,value,base+0x12aed3e,now),"ambiguous equipment references refused");put(props+0x20,uint32_t{1});memory.erase(slots+8);for(int i=1;i<8;++i)memory.erase(slots+8+i);
    active=actor+1;require(!candidate.rewrite(body,value,base+0x12aed3e,now),"actor handoff cancels ownership");active=actor;
    require(!candidate.rewrite(body,value,base+0x12aed3e,now+std::chrono::milliseconds(101)),"stale controls publication refused");
    require(!candidate.rewrite(body,value,base+0x12aed3e,now-std::chrono::milliseconds(1)),"future release refused");
    candidate.publish(pose,now+std::chrono::milliseconds(751));require(!candidate.rewrite(body,value,base+0x12aed3e,now+std::chrono::milliseconds(751)),"expired animation/throw release refused");
    candidate.publish(std::nullopt,now);require(!candidate.rewrite(body,value,base+0x12aed3e,now),"STOP/focus loss cancels pending release");
    candidate.publish(pose,now);output=candidate.rewrite(body,value,base+0x12aed3e,now);require(output && memory==original,"failed candidates never consume the valid request or write native memory");
    auto old=pose;old.sequence=1;candidate.publish(old,now);require(!candidate.rewrite(body,value,base+0x12aed3e,now),"an older release cannot replay after a newer throw");
    ++pose.epoch;pose.sequence=1;candidate.publish(pose,now);require(candidate.rewrite(body,value,base+0x12aed3e,now).has_value(),"a new tracking epoch may restart its gesture sequence");
    candidate.publish(old,now);require(!candidate.rewrite(body,value,base+0x12aed3e,now),"an older tracking epoch cannot replay after recovery");
    require(candidate.snapshot().redirected==5,"exactly five explicitly owned launches redirected");
    // The hand sets the launch speed as a fraction of the native one (native speed 5 here).
    for(const auto [scale,expected]:{std::pair{.5f,2.5f},std::pair{1.5f,7.5f},std::pair{std::nanf(""),5.f},std::pair{10.f,5.f},std::pair{.01f,5.f}}){
        ++pose.sequence;pose.speed_scale=scale;pose.world=vr_camera::IDENTITY;candidate.publish(pose,now);
        output=candidate.rewrite(body,value,base+0x12aed3e,now);
        require(output && std::abs(std::hypot((*output)[0],(*output)[1],(*output)[2])-expected)<1e-4f && (*output)[2]>0,"launch speed follows the hand within the sane band; anything else keeps the native speed");
    }
    pose.speed_scale=1;
    require(candidate.snapshot().redirected==10,"each scaled throw redirected once");
    {const auto history=candidate.snapshot().history;
    require(history.size()==10 && history.front().serial==1 && history.back().serial==10,"every redirect is kept for the log, in order");
    require(std::abs(history[1].after[0]-5)<.0001f && history[1].before==speed,"history keeps the native vector and its replacement");}
    for(int i=0;i<20;++i){++pose.sequence;candidate.publish(pose,now);require(candidate.rewrite(body,value,base+0x12aed3e,now).has_value(),"repeat throw");}
    require(candidate.snapshot().history.size()==NativeGrenadeLaunch::HISTORY && candidate.snapshot().history.back().serial==30,"history stays bounded and keeps the newest");
    // 26 Oct: krak, shock and melta throws kept the game's gaze-camera aim (frag only).
    for(const auto* thrown:{"grenade_krak_client","grenade_shock_client","melta_charge_client"}){
        name(props,thrown);++pose.sequence;pose.world=vr_arm::turn({0,0,1},{1,0,0});candidate.publish(pose,now);
        output=candidate.rewrite(body,value,base+0x12aed3e,now);
        require(output && std::abs((*output)[0]-5)<.0001f,"every player throwable follows the hand");
    }
    {const auto refused=candidate.snapshot().refused;
    for(const auto* other:{"grenade_cultist_client","melta_charge_siege_client","grenade_frag"}){
        name(props,other);++pose.sequence;candidate.publish(pose,now);
        require(!candidate.rewrite(body,value,base+0x12aed3e,now),"an enemy grenade, an objective charge or a near name is never redirected");
    }
    const auto s=candidate.snapshot();
    require(s.refused==refused+3 && s.last_refused_type=="grenade_frag","each is counted as refused, with the type that was refused");
    require(s.refused==s.refused_pose+s.refused_type+s.refused_owner && s.refused_type==3 && s.refused_pose>0 && s.refused_owner>0,"every refusal has one reason");}
    std::cout<<"native grenade launch ownership, lifetime and directions passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
