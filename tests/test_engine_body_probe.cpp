#include "synthetic_code.h"
#include "EngineBodyProbe.h"
#include "game_build_ported.h"
#include "ChainswordRevRequest.h"
#include "VrFreshnessTestAccess.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <limits>
namespace {
void require(bool value,const char* message) { if (!value) { throw std::runtime_error{message}; } }
template<class T> void write(void* object,size_t offset,const T& value) { std::memcpy(static_cast<uint8_t*>(object)+offset,&value,sizeof(T)); }
uint64_t updates{},worlds{};
void* observed_body{}; void* observed_args{};
std::chrono::steady_clock::time_point read_time{};
const EngineBodyProbe* clock_body{};const VrControllerInput* clock_input{};
unsigned body_clock_reads{},input_clock_reads{};
std::chrono::steady_clock::time_point read_clock(){
    if(clock_body)require(VrFreshnessTestAccess::held(*clock_body),"actual actor clock must execute under its publication mutex");
    ++body_clock_reads;return read_time;
}
std::chrono::steady_clock::time_point input_clock(){
    require(clock_input && VrFreshnessTestAccess::held(*clock_input),"rev callback input clock executes under its own publication mutex");
    ++input_clock_reads;return read_time;
}
__declspec(noinline) void update(void* body,void* arguments) { ++updates; observed_body = body; observed_args = arguments; }
__declspec(noinline) uint8_t world(void* tracker,float* output) { ++worlds; output[12] = 7; return tracker ? 3 : 0; }
}
int main() {
    try {
        auto& probe = EngineBodyProbe::get();
        require(probe.install(reinterpret_cast<void*>(&update),reinterpret_cast<void*>(&world),0x140000000,0x5000000),"real native detours must install");
        std::array<uint8_t,0x660> body{};
        std::array<uint8_t,0x180> entity{},tracker{};
        std::array<uintptr_t,1> actor{123},args{};
        std::array<float,16> requested{},out{};
        std::array<uintptr_t,10> table{};
        table[9] = 0x140001234;
        write(body.data(),0,uintptr_t(0x140002222)); write(body.data(),8,reinterpret_cast<uintptr_t>(entity.data()));
        write(body.data(),0x130,uintptr_t(1234)); write(body.data(),0x138,reinterpret_cast<uintptr_t>(actor.data()));
        write(entity.data(),0xc8,std::array<float,3>{1,2,3}); write(entity.data(),0xb8,std::array<float,3>{0,0,-1});
        requested[12] = 4; args[0] = reinterpret_cast<uintptr_t>(requested.data());
        write(tracker.data(),0,reinterpret_cast<uintptr_t>(table.data()));
        const auto before_body = body;
        const auto before_entity = entity, before_tracker = tracker;
        void(*volatile tick)(void*,void*) = update;
        uint8_t(*volatile transform)(void*,float*) = world;
        tick(body.data(),args.data()); require(updates == 1 && probe.snapshot().updates == 0,"disabled probe must preserve original and collect nothing");
        probe.enable(true); tick(body.data(),args.data());
        require(updates == 2 && observed_body == body.data() && observed_args == args.data(),"body update must retain original ABI and both pointers");
        auto state = probe.snapshot(); require(state.bodies.size() == 1 && state.bodies[0].position[0] == 1 && state.bodies[0].requested_position[0] == 4 && state.bodies[0].actor == 123,"collect actual body/actor/entity and requested post-update movement");
        tick(body.data(),args.data()); require(probe.snapshot().bodies[0].calls == 2,"repeat calls must update same body, not duplicate it");
        require(transform(tracker.data(),out.data()) == 3 && worlds == 1 && out[12] == 7,"tracker detour must preserve native result including non-boolean byte value and output");
        probe.observe_tracker(0x140000000+0x13a2838,tracker.data(),out.data(),true);
        state = probe.snapshot(); require(state.trackers.size() == 1 && state.trackers[0].world_method_rva == 0x1234 && state.trackers[0].world[12] == 7,"TPS caller must expose root identity and method RVA without invoking unknown methods");
        probe.observe_tracker(0x140000000+0x123,tracker.data(),out.data(),true);
        probe.observe_tracker(1,tracker.data(),out.data(),true);
        require(probe.snapshot().trackers.size() == 1 && probe.snapshot().tracker_calls == 1,"unrelated/outside callers must stay excluded");
        probe.observe_body(reinterpret_cast<void*>(1),args.data());
        write(entity.data(),0xc8,std::array<float,3>{std::numeric_limits<float>::quiet_NaN(),0,0});
        probe.observe_body(body.data(),args.data());
        write(entity.data(),0xc8,std::array<float,3>{1,2,3});
        require(probe.snapshot().rejected == 2 && probe.snapshot().bodies[0].calls == 2,"invalid pointers and non-finite transforms must reject without corrupting last sample");
        require(body == before_body && entity == before_entity && tracker == before_tracker,"collection must never mutate physics/entity/tracker memory");
        table[9]=0x140000000+0x134c2e0;
        write(tracker.data(),0xd0,reinterpret_cast<uintptr_t>(actor.data()));
        probe.observe_tracker(0x140000000+0x13a4476,tracker.data(),out.data(),true);
        const auto received=probe.snapshot().trackers.back().received;
        clock_body=&probe;VrFreshnessTestAccess::clock(probe,read_clock);
        read_time=received+std::chrono::milliseconds(1);
        require(probe.active_actor_current(read_clock)==123 && probe.active_actor()==123,"current owner reads use publication-locked freshness");
        VrControllerInput input(input_clock);clock_input=&input;input.enable(true);
        input.update(VrControllerSample{},0,true,received);
        VrControllerSample rev_sample;rev_sample.chainsword_rev=.8f;input.update(rev_sample,0,true,received);
        const auto body_before=body_clock_reads,input_before=input_clock_reads;
        const auto rev_request=read_chainsword_rev_request(probe,input);
        require(rev_request.actor==123 && rev_request.force==.8f && body_clock_reads==body_before+1 && input_clock_reads==input_before+1,
            "production rev request callback consumes each source's locked current read exactly once");
        read_time=received-std::chrono::milliseconds(1);
        require(!probe.active_actor_current(read_clock),"genuinely future owner stays unavailable");
        read_time=received+std::chrono::milliseconds(101);
        require(!probe.active_actor_current(read_clock),"stale owner stays unavailable");
        read_time=received+std::chrono::milliseconds(1);input.update(rev_sample,0,false,received);
        require(!read_chainsword_rev_request(probe,input).force,"production rev request releases immediately on focus loss");
        clock_input=nullptr;clock_body=nullptr;VrFreshnessTestAccess::clock(probe,std::chrono::steady_clock::now);
        table[9]=0x140001234;
        write(tracker.data(),0xd0,uintptr_t{});
        std::array<std::array<uint8_t,0x660>,70> many_bodies{};
        for (auto& candidate:many_bodies) { candidate = body; probe.observe_body(candidate.data(),args.data()); }
        require(probe.snapshot().bodies.size() == 64 && probe.snapshot().overflow == 7,"candidate collection must stay bounded in crowded scenes");
        require(!probe.snapshot().evicted,"a crowded scene of live bodies evicts nothing");
        {   // 26 Oct playtest, 11:19: after enough level loads all 32 tracker rows held earlier levels'
            // trackers, the new player's tracker was dropped, active_actor() read 0 for good and the
            // headset stayed flat (owned readiness: actor_missing). A full table now gives the least
            // recently seen row to the new tracker once that row is a second old (BoundedTable.h).
            static std::chrono::steady_clock::time_point fake{std::chrono::seconds(500)};
            VrFreshnessTestAccess::clock(probe,[]{return fake;});
            std::array<std::array<uint8_t,0x180>,40> old_level{};
            for(auto& t:old_level){t=tracker;probe.observe_tracker(0x140000000+0x13a4476,t.data(),out.data(),true);}
            const auto full=probe.snapshot();
            require(full.trackers.size()==32 && full.overflow>7 && !full.evicted,"a level's worth of live trackers fills the table; live rows are never evicted");
            fake+=std::chrono::seconds(2); // the next level: none of those rows is seen again
            auto player=tracker;table[9]=0x140000000+0x134c2e0;std::array<uintptr_t,1> new_actor{456};
            write(player.data(),0xd0,reinterpret_cast<uintptr_t>(new_actor.data()));
            probe.observe_tracker(0x140000000+0x13a4476,player.data(),out.data(),true);
            require(probe.snapshot().evicted==1 && probe.snapshot().trackers.size()==32,"the new level's tracker takes the stalest row");
            require(probe.active_actor(fake+std::chrono::milliseconds(1))==456,"the new player is the active actor again");
            // The readiness-stall guard's rediscovery: every row goes; the next frame's objects come back.
            probe.forget();require(probe.snapshot().trackers.empty() && probe.snapshot().bodies.empty() && probe.forgets()==1 && !probe.active_actor(fake+std::chrono::milliseconds(1)),"forget drops every row");
            probe.observe_tracker(0x140000000+0x13a4476,player.data(),out.data(),true);
            require(probe.snapshot().trackers.size()==1 && probe.active_actor(fake+std::chrono::milliseconds(1))==456,"the player is learned again on its next update");
            table[9]=0x140001234;VrFreshnessTestAccess::clock(probe,std::chrono::steady_clock::now);
        }
        auto update_bytes=synthetic_at<16>(0x5623d0);
        auto world_bytes=synthetic_at<16>(0x171d2f0);
        require(probe.matches_build(0x6ac3e716,update_bytes,world_bytes) && !probe.matches_build(0,update_bytes,world_bytes),"native layout requires correct executable build");
        update_bytes[15]^=1; require(!probe.matches_build(0x6ac3e716,update_bytes,world_bytes),"changed update prologue must fail closed");
        update_bytes[15]^=1; world_bytes[15]^=1; require(!probe.matches_build(0x6ac3e716,update_bytes,world_bytes),"changed tracker prologue must fail closed");
        require(probe.matches_build(game_build_ported::EXE,game_build_ported::code(EngineBodyProbe::BODY_UPDATE_RVA),game_build_ported::code(EngineBodyProbe::TRACKER_WORLD_RVA)),"the 2026-10-21 body update and tracker world must match");
        probe.uninstall(); tick(body.data(),args.data()); require(updates == 4,"uninstall must restore original update");
        std::cout << "passive native body/tracker ABI, collection, isolation and build guards passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
