#include "synthetic_code.h"
#include "EngineWeaponProbe.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <algorithm>
#include <thread>
namespace {
void require(bool v,const char* message) {if(!v) throw std::runtime_error(message);}
template<class T> void put(void* p,size_t offset,const T& value) {std::memcpy(static_cast<uint8_t*>(p)+offset,&value,sizeof(value));}
template<class T> T get(void* p,size_t offset) {T v{};std::memcpy(&v,static_cast<uint8_t*>(p)+offset,sizeof(v));return v;}
uint64_t ray_calls{},world_calls{},body_calls{};uintptr_t active{};bool corrupt{};
void* nested{};
uint64_t aim_calls{};int aim_reads{3};void* nested_aim{};
__declspec(noinline) void body(void*,void*) {++body_calls;}
__declspec(noinline) uint8_t world(void*,float* output) {
    ++world_calls;auto matrix=vr_camera::IDENTITY;matrix[12]=3;matrix[13]=2;
    if(corrupt) matrix[0]=NAN;
    std::memcpy(output,matrix.data(),sizeof(matrix));return 3;
}
__declspec(noinline) void ray(void* object) {
    ++ray_calls;if(reinterpret_cast<uintptr_t>(object)<4096) return;
    if(nested) {auto* next=nested;nested=nullptr;void(*volatile invoke)(void*)=ray;invoke(next);}
    const auto scene=get<uintptr_t>(object,8);const auto tracker=get<uintptr_t>(reinterpret_cast<void*>(scene),0x20);
    vr_camera::Matrix transform{};uint8_t(*volatile query)(void*,float*)=world;
    require(query(reinterpret_cast<void*>(tracker),transform.data())==3,"fixture preserves tracker result ABI");
    const auto length=get<float>(object,0x30);
    std::array<float,3> target{};for(int i=0;i<3;++i) target[i]=transform[12+i]+transform[8+i]*length;
    put(object,0x10c,target);
}
__declspec(noinline) void aim(void* interface,void* arguments) {
    ++aim_calls;
    if(reinterpret_cast<uintptr_t>(interface)<4096) return;
    if(nested_aim) {auto* next=nested_aim;nested_aim=nullptr;void(*volatile invoke)(void*,void*)=aim;invoke(next,arguments);}
    auto* object=static_cast<uint8_t*>(interface)-0x20;
    auto* tracker=get<void*>(object,0x48);vr_camera::Matrix output{};
    for(int index=0;index<aim_reads;++index) {uint8_t(*volatile query)(void*,float*)=world;require(query(tracker,output.data())==3,"aim tracker result is preserved");}
    put(object,0xe0,uint8_t{1});
}
struct Object {
    std::array<uint8_t,0x120> control{};std::array<uint8_t,0x138> scene{};std::array<uintptr_t,28> tracker{};
    std::array<uintptr_t,10> vtable{};uintptr_t owner{0xdead},parent{};
    explicit Object(uintptr_t parent_scene):parent{parent_scene} {
        vtable[9]=0x1418e4e60;tracker[0]=reinterpret_cast<uintptr_t>(vtable.data());
        tracker[0xd0/8]=reinterpret_cast<uintptr_t>(&parent);
        put(control.data(),8,reinterpret_cast<uintptr_t>(scene.data()));put(control.data(),0x30,200.0f);
        put(scene.data(),0x20,reinterpret_cast<uintptr_t>(tracker.data()));put(scene.data(),0x38,reinterpret_cast<uintptr_t>(&owner));
    }
};
struct AimObject {
    std::array<uint8_t,0xf8> object{};std::array<uint8_t,0xf0> controller{},actor{};
    std::array<uint8_t,0x60> geometry{};std::array<uint8_t,0x138> entity{};
    uintptr_t controller_link{},geometry_link{};uint32_t properties{};
    explicit AimObject(void* tracker) {
        controller_link=reinterpret_cast<uintptr_t>(controller.data());geometry_link=reinterpret_cast<uintptr_t>(geometry.data());
        put(object.data(),0x10,reinterpret_cast<uintptr_t>(&controller_link));put(object.data(),0x48,reinterpret_cast<uintptr_t>(tracker));put(object.data(),0x64,uint8_t{1});
        put(controller.data(),8,reinterpret_cast<uintptr_t>(entity.data()));put(controller.data(),0xe0,reinterpret_cast<uintptr_t>(&geometry_link));
        put(actor.data(),0x38,reinterpret_cast<uintptr_t>(entity.data()));put(actor.data(),0xd8,geometry_link);put(actor.data(),0x20,reinterpret_cast<uintptr_t>(&properties));
        put(geometry.data(),0x58,reinterpret_cast<uintptr_t>(&properties));
    }
};
}
int main() {try {
    auto& probe=EngineWeaponProbe::get();auto& bodies=EngineBodyProbe::get();
    require(!probe.install(reinterpret_cast<void*>(&ray),0x140000000,[]{return active;}),"probe requires shared native tracker detour");
    require(bodies.install(reinterpret_cast<void*>(&body),reinterpret_cast<void*>(&world),0x140000000,0x5000000),"tracker fixture hooks install");
    require(probe.install(reinterpret_cast<void*>(&ray),0x140000000,[]{return active;}),"actual native ray hook installs");
    std::array<uint8_t,0x40> player{},other_player{};
    std::array<uint8_t,0x138> root{},other_root{};
    put(player.data(),0x38,reinterpret_cast<uintptr_t>(root.data()));put(other_player.data(),0x38,reinterpret_cast<uintptr_t>(other_root.data()));
    active=reinterpret_cast<uintptr_t>(player.data());
    Object ancestor{reinterpret_cast<uintptr_t>(root.data())},middle{reinterpret_cast<uintptr_t>(ancestor.scene.data())};
    Object local{reinterpret_cast<uintptr_t>(middle.scene.data())},npc{reinterpret_cast<uintptr_t>(other_root.data())};void(*volatile tick)(void*)=ray;
    tick(local.control.data());require(ray_calls==1 && probe.snapshot().calls==0,"disabled hook remains passive");
    auto expected=local.control;const auto scene=local.scene;const auto tracker=local.tracker;
    probe.enable(true);tick(local.control.data());
    auto state=probe.snapshot();require(ray_calls==2 && world_calls==2 && state.calls==1 && state.worlds==1,"original ray and tracker run exactly once");
    const auto& sample=state.targets.at(0);
    require(sample.owned && sample.actor==active && sample.active_actor==active && sample.control==reinterpret_cast<uintptr_t>(local.control.data()) && sample.root_scene==reinterpret_cast<uintptr_t>(root.data()) && sample.parent_depth==3,"verified parent trackers reach the fresh player's exact root after three links");
    require(sample.scene_owner==npc.owner && sample.actor!=sample.scene_owner,"shared scene-container identity is not player ownership");
    require(sample.world[12]==3 && sample.target==std::array<float,3>{3,2,200} && sample.world_method_rva==0x18e4e60 && sample.max_distance==200,"observe actual ray source and published target after native collision path");
    require(local.control==expected && local.scene==scene && local.tracker==tracker,"probe never writes control/scene/tracker memory");
    tick(npc.control.data());state=probe.snapshot();require(state.targets.size()==2 && !state.targets[1].owned && !state.targets[1].actor,"foreign root with the same container is not local");
    active=0;tick(local.control.data());require(!probe.snapshot().targets[0].owned,"absent fresh player ownership is not retained");active=reinterpret_cast<uintptr_t>(player.data());
    middle.parent=reinterpret_cast<uintptr_t>(local.scene.data());tick(local.control.data());require(!probe.snapshot().targets[0].owned,"cyclic hierarchy fails closed");middle.parent=reinterpret_cast<uintptr_t>(ancestor.scene.data());
    middle.vtable[9]=0x1418e4e61;tick(local.control.data());require(!probe.snapshot().targets[0].owned,"unverified parent tracker ABI is not followed");middle.vtable[9]=0x1418e4e60;
    middle.parent=1;tick(local.control.data());require(!probe.snapshot().targets[0].owned,"unreadable parent link fails closed");middle.parent=reinterpret_cast<uintptr_t>(ancestor.scene.data());
    active=reinterpret_cast<uintptr_t>(other_player.data());tick(local.control.data());require(!probe.snapshot().targets[0].owned,"actor change invalidates previous ownership");active=reinterpret_cast<uintptr_t>(player.data());
    const auto before=probe.snapshot().worlds;vr_camera::Matrix other{};
    uint8_t(*volatile query)(void*,float*)=world;query(local.tracker.data(),other.data());
    require(probe.snapshot().worlds==before,"unscoped tracker reads are excluded");
    nested=npc.control.data();tick(local.control.data());state=probe.snapshot();
    require(state.targets[0].owned && !state.targets[1].owned && state.targets[0].tracker==reinterpret_cast<uintptr_t>(local.tracker.data()),"nested ray scope restores outer tracker/ownership");
    corrupt=true;tick(local.control.data());corrupt=false;tick(reinterpret_cast<void*>(1));
    require(probe.snapshot().rejected==2,"bad pointers and malformed transforms reject observation without blocking native original");
    std::vector<std::unique_ptr<Object>> crowded;
    for(int i=0;i<34;++i) {crowded.push_back(std::make_unique<Object>(reinterpret_cast<uintptr_t>(other_root.data())));tick(crowded.back()->control.data());}
    Object newest{reinterpret_cast<uintptr_t>(root.data())};tick(newest.control.data());state=probe.snapshot();
    const auto fresh=std::ranges::find(state.targets,reinterpret_cast<uintptr_t>(newest.control.data()),&EngineWeaponProbe::Sample::control);
    require(state.targets.size()==32 && state.overflow>0 && fresh!=state.targets.end() && fresh->owned,"new local weapon survives a full candidate buffer by displacing an unowned entry");
    require(probe.matches_build(0x6ac3e716,SYNTH(EngineWeaponProbe::PROLOGUE)) && !probe.matches_build(0,SYNTH(EngineWeaponProbe::PROLOGUE)),"targeting layout is executable-build guarded");
    auto changed=SYNTH(EngineWeaponProbe::PROLOGUE);changed[15]^=1;require(!probe.matches_build(0x6ac3e716,changed),"changed targeting prologue fails closed");
    // Feed the exact committed pose into the real ray/tracker detours. The fixture's
    // original native ray still computes its collision target; only its temporary source changes.
    auto& driven=probe.aim_driver();driven.enable(true);driven.allow(true,7);
    auto gun=vr_camera::IDENTITY;gun[0]=gun[10]=0;gun[2]=-1;gun[8]=1;gun[12]=10;gun[13]=20;gun[14]=30;
    driven.publish({active,7,1,std::chrono::steady_clock::now(),gun});
    const auto untouched_scene=local.scene;tick(local.control.data());
    require(get<std::array<float,3>>(local.control.data(),0x10c)==std::array<float,3>{210,20,30} && probe.snapshot().ray_overrides==1,"native collision ray uses committed gun origin/forward instead of camera direction");
    require(local.scene==untouched_scene,"ray override never modifies persistent SceneTransform memory");
    Object alias{reinterpret_cast<uintptr_t>(local.scene.data())};vr_camera::Matrix target_world{};
    query(alias.tracker.data(),target_world.data());
    require(target_world[12]==210 && target_world[13]==20 && target_world[14]==30 && target_world[8]==1 && probe.snapshot().target_overrides==1,"native target-sync aliases get original collision result with the same barrel direction");
    Object unrelated{reinterpret_cast<uintptr_t>(npc.scene.data())};query(unrelated.tracker.data(),target_world.data());
    require(target_world[12]==3 && target_world[13]==2 && target_world[8]==0,"unrelated scene trackers stay native even in the same container");
    driven.allow(false,7);tick(local.control.data());require(get<std::array<float,3>>(local.control.data(),0x10c)==std::array<float,3>{3,2,200},"tracking/focus loss restores native collision source immediately");
    query(alias.tracker.data(),target_world.data());require(target_world[12]==3 && target_world[13]==2,"target-sync override releases with collision source");
    // The crosshair reads this: a lone ray that misses the gun's pose must not throw it to the game's aim.
    const auto held=probe.owned_target(std::chrono::steady_clock::now());
    require(held && held->steered && held->point==std::array<float,3>{210,20,30},"a steered hit stands over the newer unsteered ray for 100 ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(110));tick(local.control.data());
    const auto released=probe.owned_target(std::chrono::steady_clock::now());
    require(released && !released->steered && released->point==std::array<float,3>{3,2,200},"after 100 ms the game's own ray is the target again");
    driven.enable(false);
    require(probe.install_aim(reinterpret_cast<void*>(&aim)),"real CtrlAim update detour installs");
    AimObject local_aim{local.tracker.data()},npc_aim{npc.tracker.data()};
    active=reinterpret_cast<uintptr_t>(local_aim.actor.data());
    auto input=vr_camera::IDENTITY;input[12]=7;auto* input_pointer=input.data();
    auto expected_aim=local_aim.object;put(expected_aim.data(),0xe0,uint8_t{1});
    const auto controller_before=local_aim.controller,actor_before=local_aim.actor;
    void(*volatile animate)(void*,void*)=aim;animate(local_aim.object.data()+0x20,&input_pointer);state=probe.snapshot();
    require(aim_calls==1 && state.aim_calls==1 && state.aim_owned==1 && state.aim && state.aim->actor==active && state.aim->controller==local_aim.controller_link,"CtrlAim uses its interface adjustment and exact native controller/entity/geometry ownership");
    require(state.aim->input_target==input && state.aim->target_valid && state.aim->direction_only && state.aim->aimed && state.aim->read_count==3 && state.aim->reads[0].valid && state.aim->reads[0].tracker==reinterpret_cast<uintptr_t>(local.tracker.data()),"capture actual input target and scoped native aiming reads");
    require(local_aim.object==expected_aim && local_aim.controller==controller_before && local_aim.actor==actor_before && input[12]==7,"aim probe preserves input and native object writes exactly");
    nested_aim=npc_aim.object.data()+0x20;animate(local_aim.object.data()+0x20,&input_pointer);state=probe.snapshot();
    require(aim_calls==3 && state.aim_owned==2 && state.aim->control==reinterpret_cast<uintptr_t>(local_aim.object.data()) && state.aim->read_count==3,"nested unrelated aim updates restore local read scope");
    local_aim.geometry_link=1;animate(local_aim.object.data()+0x20,&input_pointer);require(!probe.snapshot().aim,"lost geometry ownership clears the current local aim sample");local_aim.geometry_link=reinterpret_cast<uintptr_t>(local_aim.geometry.data());
    aim_reads=10;animate(local_aim.object.data()+0x20,&input_pointer);require(probe.snapshot().aim->read_count==8,"native aim read capture stays bounded without limiting original work");
    active=0;animate(local_aim.object.data()+0x20,&input_pointer);require(!probe.snapshot().aim,"fresh player loss clears passive native aim ownership");
    const auto observed=probe.snapshot().calls;probe.uninstall();tick(local.control.data());require(probe.snapshot().calls==observed,"uninstall restores original ray");
    const auto observed_aim=probe.snapshot().aim_calls;animate(local_aim.object.data()+0x20,&input_pointer);require(probe.snapshot().aim_calls==observed_aim,"uninstall restores native aim update");
    bodies.uninstall();std::cout<<"passive native weapon ray, exact ownership and nested tracker scopes passed\n";return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
