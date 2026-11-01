#include "synthetic_code.h"
#include "EngineArmProbe.h"
#include "VrFreshnessTestAccess.h"
#include "VrCameraPose.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
void require(bool value,const char* why) { if (!value) { throw std::runtime_error{why}; } }
template<class T> void write(void* object,size_t offset,const T& value) { std::memcpy(static_cast<uint8_t*>(object)+offset,&value,sizeof(T)); }
uint64_t calls{},published{}; void* observed_controller{}; void* observed_arguments{};
EngineArmProbe::Matrix* native_pose{}; void* native_wrapper{};
EngineArmProbe* clock_probe{};EngineArmProbe::Clock::time_point read_time{};unsigned clock_reads{};
EngineBodyProbe* clock_body{};EngineArmProbe::Clock::time_point body_time{};
uint64_t body_updates{},body_worlds{};
EngineArmProbe::Clock::time_point arm_clock(){
    require(clock_probe && VrFreshnessTestAccess::held(*clock_probe),"actual arm snapshot clock must execute under the native publication mutex");
    ++clock_reads;return read_time;
}
EngineArmProbe::Clock::time_point body_clock(){
    require(clock_body && VrFreshnessTestAccess::held(*clock_body),"current native actor resolver must use Body's publication-locked clock");return body_time;
}
__declspec(noinline) void body_update(void*,void*) {++body_updates;}
__declspec(noinline) uint8_t body_world(void*,float*) {++body_worlds;return 1;}
__declspec(noinline) uint64_t publish(void* wrapper) { ++published; if (wrapper==native_wrapper) { (*native_pose)[12]=42; } SetLastError(0x81bc);return 0xabcdef1200000001ull; }
__declspec(noinline) void update(void* controller,void* arguments) {
    ++calls; observed_controller=controller; observed_arguments=arguments;
    uint64_t(*volatile native_publish)(void*)=publish; native_publish(native_wrapper);
}
struct Rig {
    std::array<uint8_t,0x230> controller{};
    std::array<uint8_t,0x2900> wrapper{};
    std::array<uint8_t,0x100> actor{},entity{},network{},definition{},root{},rig_attribute{},rig{},formats{},layout{};
    uintptr_t root_link{};
    std::array<uint8_t,256> attributes{};
    std::array<uint8_t,0x200> instance{};
    std::array<uint8_t,0x60> properties{};
    std::array<uintptr_t,2> definition_context{};
    std::array<uint8_t,0x40> hierarchy{},names{},bind_attribute{},bind_buffer{};
    std::array<int32_t,3> parents{-1,0,1};
    std::array<uint32_t,3> offsets{0,5,11};
    const char* text="ROOT\0ARM1L\0HANDL\0";
    std::array<uintptr_t,2> channels{};
    std::array<std::array<float,4>,3> bind_positions{},bind_rotations{{{0,0,0,1},{0,0,0,1},{0,0,0,1}}};
    uintptr_t instance_link{};
    std::array<uint8_t,32> record{};
    std::array<uintptr_t,2> map{};
    std::array<int16_t,3> indices{2,-1,0};
    std::array<EngineArmProbe::Matrix,3> pose{};
    Rig() {
        instance_link=reinterpret_cast<uintptr_t>(instance.data());
        write(actor.data(),0x38,reinterpret_cast<uintptr_t>(entity.data()));
        write(actor.data(),0xd8,reinterpret_cast<uintptr_t>(instance.data()));
        write(actor.data(),0x20,reinterpret_cast<uintptr_t>(properties.data()));
        write(instance.data(),0x58,reinterpret_cast<uintptr_t>(properties.data()));
        write(controller.data(),8,reinterpret_cast<uintptr_t>(entity.data()));
        write(controller.data(),0xe0,reinterpret_cast<uintptr_t>(&instance_link));
        write(controller.data(),0xe8,reinterpret_cast<uintptr_t>(wrapper.data()));
        write(controller.data(),0x1ea,uint8_t(1));
        definition_context[1]=reinterpret_cast<uintptr_t>(definition.data());
        write(wrapper.data(),0,reinterpret_cast<uintptr_t>(definition_context.data()));
        // Native +8 is packed handle/time data; it is not the network definition pointer.
        write(wrapper.data(),8,uint64_t(0x3e19999a0000036a));
        write(wrapper.data(),0x10,reinterpret_cast<uintptr_t>(instance.data()));
        write(wrapper.data(),0x2858,reinterpret_cast<uintptr_t>(network.data()));
        write(network.data(),0,reinterpret_cast<uintptr_t>(definition.data()));
        root_link=reinterpret_cast<uintptr_t>(root.data());
        write(definition.data(),0xb0,reinterpret_cast<uintptr_t>(&root_link));
        write(root.data(),0x90,reinterpret_cast<uintptr_t>(formats.data()));
        write(formats.data(),4,uint8_t(1));
        write(formats.data(),8,reinterpret_cast<uintptr_t>(layout.data()));
        write(layout.data(),0x15,uint8_t(2));
        write(root.data(),0x48,reinterpret_cast<uintptr_t>(attributes.data()));
        write(attributes.data(),9*16,reinterpret_cast<uintptr_t>(rig_attribute.data()));
        write(rig_attribute.data(),0x10,reinterpret_cast<uintptr_t>(rig.data()));
        write(rig.data(),0x30,reinterpret_cast<uintptr_t>(hierarchy.data()));
        write(hierarchy.data(),0,uint32_t(3)); write(hierarchy.data(),8,reinterpret_cast<uintptr_t>(parents.data()));
        write(rig.data(),0x40,reinterpret_cast<uintptr_t>(names.data()));
        write(names.data(),0,uint32_t(3)); write(names.data(),8,reinterpret_cast<uintptr_t>(offsets.data())); write(names.data(),0x10,reinterpret_cast<uintptr_t>(text));
        write(rig.data(),0x48,reinterpret_cast<uintptr_t>(bind_attribute.data())); write(bind_attribute.data(),0x10,reinterpret_cast<uintptr_t>(bind_buffer.data()));
        channels={reinterpret_cast<uintptr_t>(bind_positions.data()),reinterpret_cast<uintptr_t>(bind_rotations.data())}; write(bind_buffer.data(),0x20,reinterpret_cast<uintptr_t>(channels.data()));
        write(entity.data(),0x98,EngineArmProbe::Matrix{1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1});
        write(wrapper.data(),0x2870,reinterpret_cast<uintptr_t>(record.data()));
        write(wrapper.data(),0x2880,int32_t(0));
        write(record.data(),4,uint16_t(7));
        map={reinterpret_cast<uintptr_t>(indices.data()),3};
        write(record.data(),8,reinterpret_cast<uintptr_t>(map.data()));
        write(instance.data(),0xd0,reinterpret_cast<uintptr_t>(pose.data()));
        for (auto& matrix:pose) { matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
    }
};
}
int main() {
    try {
        auto& probe=EngineArmProbe::get(); Rig player,npc;
        auto actor=reinterpret_cast<uintptr_t>(player.actor.data());
        auto& body=EngineBodyProbe::get();bool production_resolvers{};unsigned explicit_reads{},current_reads{};EngineArmProbe::Clock::time_point explicit_time{};
        require(probe.install(reinterpret_cast<void*>(&update),reinterpret_cast<void*>(&publish),[&](auto now){
            if(!production_resolvers)return actor;++explicit_reads;explicit_time=now;return body.active_actor(now);
        },[&]{if(!production_resolvers)return actor;++current_reads;return body.active_actor();}),"native update/publication hooks install");
        native_wrapper=player.wrapper.data(); native_pose=&player.pose[0];
        void(*volatile tick)(void*,void*)=update; uint64_t(*volatile final_pose)(void*)=publish;
        auto before=player.controller;
        tick(player.controller.data(),reinterpret_cast<void*>(123));
        require(calls==1 && probe.snapshot().updates==0,"disabled observer passes through without collecting");
        probe.enable(true);
        tick(player.controller.data(),reinterpret_cast<void*>(456));
        auto state=probe.snapshot();
        require(calls==2 && observed_controller==player.controller.data() && observed_arguments==reinterpret_cast<void*>(456),"retain both native update arguments");
        require(state.pose && state.pose->owner.actor==actor && state.pose->owner.network==reinterpret_cast<uintptr_t>(player.network.data()),"exact actor/entity association reaches live network");
        require(state.pose->animset==7 && state.pose->bones.size()==2 && state.pose->bones[0].rig_index==0 && state.pose->bones[0].skin_index==2 && state.pose->bones[1].local[12]==42,"capture native final writes and actual signed rig-to-skin map, excluding unmapped bone");
        require(state.pose->rig==reinterpret_cast<uintptr_t>(player.rig.data()),"resolve the native animset-specific rig attribute");
        require(state.pose->layout && state.pose->layout->names[2]=="HANDL" && state.pose->layout->parents[2]==1 && state.pose->layout->bind_locals[0][15]==1 && state.pose->world_valid && state.pose->world[12]==10,"read owned native rig names, hierarchy, bind pose and world transform");
        for(bool throwing:{false,true}) {
            probe.set_pose_observer([throwing](const auto&){SetLastError(0xbeef);if(throwing)throw std::runtime_error{"fixture observer exception"};});
            const auto native_count=published;const auto result=final_pose(native_wrapper);const auto native_error=GetLastError();
            require(result==0xabcdef1200000001ull&&published==native_count+1&&native_error==0x81bc,"actual publication hook calls native once and preserves its return and LastError through observer work and exceptions");
        }
        probe.set_pose_observer({});
        {
            // Inject publication/clock interleaving while the actual source
            // mutex is held. No sleeps or scheduling-duration assumptions.
            const auto before_read=state.pose->received;clock_probe=&probe;
            VrFreshnessTestAccess::clock(probe,arm_clock);read_time=before_read;
            auto publication=VrFreshnessTestAccess::lock(probe);
            std::promise<void> started;auto entered=started.get_future();
            auto reader=std::async(std::launch::async,[&]{started.set_value();return probe.snapshot();});
            entered.get();
            const auto published=before_read+std::chrono::milliseconds(1);
            VrFreshnessTestAccess::received_locked(probe,published);read_time=published+std::chrono::milliseconds(1);publication.unlock();
            const auto current=reader.get();
            require(current.pose && current.pose->received==published && clock_reads==1,"actual default arm snapshot reads the contended publication with a post-lock clock");
            require(!probe.snapshot(before_read).pose,"explicit pre-publication timestamp still rejects a future pose");
            require(probe.snapshot_current(arm_clock).pose && clock_reads==2,"explicit current API uses the same real locked source read");
            read_time=published-std::chrono::milliseconds(1);require(!probe.snapshot().pose,"genuinely future publication remains unavailable on the current API");
            read_time=published+std::chrono::milliseconds(101);require(!probe.snapshot_current(arm_clock).pose,"stale publication remains unavailable on the current API");
            VrFreshnessTestAccess::clock(probe,EngineArmProbe::Clock::now);clock_probe=nullptr;
        }
        {
            constexpr uintptr_t base=0x140000000;
            require(body.install(reinterpret_cast<void*>(&body_update),reinterpret_cast<void*>(&body_world),base,0x5000000),"production-style actor resolver installs real Body collection fixture");body.enable(true);
            const auto time=EngineArmProbe::Clock::now();probe.observe_controller(player.controller.data(),time);probe.observe_publication(player.wrapper.data(),1,time);
            const auto published_pose=*probe.snapshot(time).pose;
            std::array<uint8_t,0x180> tracker{};std::array<uintptr_t,10> table{};table[9]=base+0x134c2e0;
            write(tracker.data(),0,reinterpret_cast<uintptr_t>(table.data()));write(tracker.data(),0xd0,reinterpret_cast<uintptr_t>(&actor));
            body.observe_tracker(base+0x13a4476,tracker.data(),published_pose.world.data(),true);
            const auto received=body.snapshot().trackers.back().received;
            // Hook installation and host scheduling are not part of the
            // freshness scenario. Keep the native pose/owner, but control its
            // publication timestamp under the actual mutex (as above).
            const auto pose_time=received-std::chrono::milliseconds(1);
            {auto publication=VrFreshnessTestAccess::lock(probe);VrFreshnessTestAccess::received_locked(probe,pose_time);}
            const auto pose=*probe.snapshot(pose_time).pose;
            require(received-pose.received==std::chrono::milliseconds(1),"resolver fixture has an exact1ms publication gap independent of hook installation or host scheduling");
            production_resolvers=true;clock_body=&body;VrFreshnessTestAccess::clock(body,body_clock);
            clock_probe=&probe;VrFreshnessTestAccess::clock(probe,arm_clock);read_time=received+std::chrono::milliseconds(2);
            body_time=received+std::chrono::milliseconds(1);
            require(!probe.snapshot(pose.received).pose && explicit_time==pose.received,"explicit snapshot preserves requested time and rejects the newer Body publication");
            require(!probe.current(pose,pose.received) && explicit_time==pose.received,"explicit current(pose,time) preserves exact resolver timestamp semantics");
            const auto before_explicit=explicit_reads,before_current=current_reads;
            require(probe.snapshot().pose && probe.snapshot_current(arm_clock).pose && probe.current(pose) && probe.current_current(pose,arm_clock),"both current APIs use independently current native actor ownership");
            require(explicit_reads==before_explicit && current_reads==before_current+4,"current snapshot/pose validation cannot invoke the explicit-time resolver");
            body_time=received+std::chrono::milliseconds(101);
            require(!probe.snapshot_current(arm_clock).pose && !probe.current_current(pose,arm_clock),"genuinely stale current Body owner remains unavailable");
            require(probe.snapshot(received).pose && explicit_time==received && probe.current(pose,received),"explicit reads keep their valid requested time even when the current Body clock is stale");
            body_time=received-std::chrono::milliseconds(1);
            require(!probe.snapshot_current(arm_clock).pose && !probe.current_current(pose,arm_clock),"genuinely future current Body owner remains unavailable");
            production_resolvers=false;VrFreshnessTestAccess::clock(probe,EngineArmProbe::Clock::now);clock_probe=nullptr;
            VrFreshnessTestAccess::clock(body,EngineArmProbe::Clock::now);clock_body=nullptr;body.uninstall();
        }
        uint64_t callbacks{};probe.set_pose_observer([&](const auto& captured){++callbacks;require(probe.current(captured,captured.received) && captured.bones[1].local[12]==42,"native observer runs after final publication without holding collection lock");});
        tick(player.controller.data(),nullptr);require(callbacks==1,"native nested publication reaches registered pose adapter");probe.set_pose_observer({});
        require(final_pose(native_wrapper)==0xabcdef1200000001ull,"publication preserves the entire native result, not just boolean byte");
        require(player.controller==before,"observer never modifies animation control memory");
        const auto t=EngineArmProbe::Clock::now();
        probe.observe_controller(npc.controller.data(),t); probe.observe_publication(npc.wrapper.data(),1,t);
        require(probe.snapshot(t).pose && probe.snapshot(t).pose->owner.actor==actor,"NPC publication/update cannot replace player");
        require(!probe.snapshot(t+std::chrono::milliseconds(101)).pose && !probe.snapshot(t-std::chrono::seconds(1)).pose,"stale and future poses are unavailable");
        probe.observe_publication(player.wrapper.data(),0,t);
        require(!probe.snapshot(t).pose,"failed native publication releases output");
        probe.observe_controller(player.controller.data(),t); probe.observe_publication(player.wrapper.data(),1,t);
        write(player.controller.data(),0x1e9,uint8_t(1)); probe.observe_controller(player.controller.data(),t);
        require(!probe.snapshot(t).pose,"ragdoll player is excluded");
        write(player.controller.data(),0x1e9,uint8_t(0));
        write(player.controller.data(),8,reinterpret_cast<uintptr_t>(npc.entity.data())); probe.observe_controller(player.controller.data(),t);
        probe.observe_publication(player.wrapper.data(),1,t); require(!probe.snapshot(t).pose,"actor and entity must agree");
        write(player.controller.data(),8,reinterpret_cast<uintptr_t>(player.entity.data()));
        player.definition_context[1]=reinterpret_cast<uintptr_t>(npc.definition.data());
        probe.observe_controller(player.controller.data(),t); probe.observe_publication(player.wrapper.data(),1,t);
        require(!probe.snapshot(t).pose,"definition context and live network must agree"); player.definition_context[1]=reinterpret_cast<uintptr_t>(player.definition.data());
        player.instance_link=reinterpret_cast<uintptr_t>(npc.instance.data());
        probe.observe_controller(player.controller.data(),t); probe.observe_publication(player.wrapper.data(),1,t);
        require(!probe.snapshot(t).pose,"same entity cannot authorize another geometry instance"); player.instance_link=reinterpret_cast<uintptr_t>(player.instance.data());
        write(player.instance.data(),0x58,reinterpret_cast<uintptr_t>(npc.properties.data()));
        probe.observe_controller(player.controller.data(),t); probe.observe_publication(player.wrapper.data(),1,t);
        require(!probe.snapshot(t).pose,"geometry and actor property ownership must agree"); write(player.instance.data(),0x58,reinterpret_cast<uintptr_t>(player.properties.data()));
        probe.observe_controller(player.controller.data(),t); actor=reinterpret_cast<uintptr_t>(npc.actor.data());
        probe.observe_publication(player.wrapper.data(),1,t); require(!probe.snapshot(t).pose,"camera ownership changes invalidate old output immediately");
        actor=reinterpret_cast<uintptr_t>(player.actor.data());
        probe.observe_controller(player.controller.data(),t);
        auto duplicate=player.controller;
        probe.observe_controller(duplicate.data(),t); probe.observe_controller(player.controller.data(),t); probe.observe_publication(player.wrapper.data(),1,t);
        require(!probe.snapshot(t).pose,"ambiguous live controllers remain excluded after the next update");
        auto later=t+std::chrono::milliseconds(200);
        probe.observe_controller(player.controller.data(),later); probe.observe_publication(player.wrapper.data(),1,later);
        require(probe.snapshot(later).pose.has_value(),"unambiguous fresh owner recovers");
        player.indices[0]=4096; probe.observe_publication(player.wrapper.data(),1,later);
        require(!probe.snapshot(later).pose,"unbounded skin mapping rejected"); player.indices[0]=2;
        write(player.wrapper.data(),0x2880,int32_t(-1)); probe.observe_publication(player.wrapper.data(),1,later);
        require(!probe.snapshot(later).pose,"invalid active animset rejected"); write(player.wrapper.data(),0x2880,int32_t(0));
        player.pose[2][0]=std::numeric_limits<float>::quiet_NaN(); probe.observe_publication(player.wrapper.data(),1,later);
        require(!probe.snapshot(later).pose,"invalid local pose rejects whole capture"); player.pose[2][0]=1;
        write(player.map.data(),8,uint32_t(1025)); probe.observe_publication(player.wrapper.data(),1,later);
        require(!probe.snapshot(later).pose,"bone count bounded"); write(player.map.data(),8,uint32_t(3));
        probe.observe_controller(reinterpret_cast<void*>(1),later); // safe read failure
        actor=0; require(!probe.snapshot(later).pose,"missing camera owner never retains an accessible pose");
        probe.enable(false); actor=reinterpret_cast<uintptr_t>(player.actor.data());
        require(!probe.snapshot(later).pose,"off mode releases cached output");
        probe.enable(true); player.parents[1]=2;
        probe.observe_controller(player.controller.data(),later); probe.observe_publication(player.wrapper.data(),1,later);
        require(probe.snapshot(later).pose && !probe.snapshot(later).pose->layout,"cyclic native hierarchy cannot authorize an arm solve");
        player.parents[1]=0; player.offsets[2]=70000;
        probe.observe_publication(player.wrapper.data(),1,later);
        require(!probe.snapshot(later).pose->layout,"native name offsets are bounded"); player.offsets[2]=11;
        probe.observe_publication(player.wrapper.data(),1,later);
        require(probe.snapshot(later).pose->layout!=nullptr,"valid rig metadata recovers");
        {
            // 30 Oct 20:46 (docs/RAVENER_ATTACK_2026-10-30.md): a Ravener attack scaled the player's
            // entity world by about 1e-4, rigid() refused it and owned stereo stood down for 8 s.
            const float c=0.6f,s=0.8f,k=0.9997f;
            write(player.entity.data(),0x98,EngineArmProbe::Matrix{k*s,0,-k*c,0, 0,k,0,0, k*c,0,k*s,0, 10,20,30,1});
            const auto scaled_before=probe.snapshot(later).world_scaled;
            probe.observe_publication(player.wrapper.data(),1,later);
            auto scaled=probe.snapshot(later);
            require(scaled.pose && scaled.pose->world_valid && std::abs(scaled.pose->world_scale-k)<1e-5f && scaled.world_scaled==scaled_before+1,
                "a uniformly scaled player world (a Ravener attack) is accepted with its scale measured");
            require(vr_camera::rigid(scaled.pose->world) && std::abs(scaled.pose->world[8]-c)<1e-5f && std::abs(scaled.pose->world[10]-s)<1e-5f && scaled.pose->world[12]==10,
                "the accepted world has the scale divided out and keeps its rotation and translation");
            const auto refused_before=scaled.world_refused;
            write(player.entity.data(),0x98,EngineArmProbe::Matrix{1,0,0,0, 0.2f,1,0,0, 0,0,1,0, 10,20,30,1});
            probe.observe_publication(player.wrapper.data(),1,later);
            auto sheared=probe.snapshot(later);
            require(sheared.pose && !sheared.pose->world_valid && sheared.world_refused==refused_before+1 && sheared.refused_world && (*sheared.refused_world)[4]==0.2f,
                "a sheared world is still refused, and the refused matrix is kept for the log");
            write(player.entity.data(),0x98,EngineArmProbe::Matrix{0.5f,0,0,0, 0,0.5f,0,0, 0,0,0.5f,0, 10,20,30,1});
            probe.observe_publication(player.wrapper.data(),1,later);
            require(!probe.snapshot(later).pose->world_valid,"a scale far from one is refused");
            write(player.entity.data(),0x98,EngineArmProbe::Matrix{1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1});
            probe.observe_publication(player.wrapper.data(),1,later);
            require(probe.snapshot(later).pose->world_valid && probe.snapshot(later).pose->world_scale==1,"a rigid world reads as before, scale one");
        }
        auto u=synthetic_at<16>(0xe46dc0);
        auto p=synthetic_at<16>(0xe31800);
        require(probe.matches_build(0x6ac3e716,u,p) && !probe.matches_build(0,u,p),"exact executable build required");
        p[3]^=1; require(!probe.matches_build(0x6ac3e716,u,p),"changed publication code rejected");
        probe.uninstall(); auto old_calls=calls; tick(player.controller.data(),nullptr); require(calls==old_calls+1,"teardown restores native code");
        std::cout << "owned final arm pose observation, native ABI, mapping, lifecycle and exclusion passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
