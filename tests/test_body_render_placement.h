#pragma once
#include "fixtures/body_render_placement.h"
#include <condition_variable>
#include <thread>

namespace body_placement_tests {
inline void check(bool value,const char* why) { if(!value) throw std::runtime_error{why}; }
inline bool close(const vr_arm::Matrix& a,const vr_arm::Matrix& b,float tolerance=2e-4f) {
    for(size_t i=0;i<16;++i) if(std::abs(a[i]-b[i])>tolerance) return false;return true;
}
inline size_t index(const char* name) {
    return std::find(body_fixture::names.begin(),body_fixture::names.end(),std::string_view{name})-body_fixture::names.begin();
}
inline void out_of_order_control() {
    using namespace body_fixture;
    auto layout=std::make_shared<EngineArmProbe::RigLayout>();layout->names.assign(names.begin(),names.end());layout->parents.assign(parents.begin(),parents.end());layout->bind_locals.assign(locals.begin(),locals.end());
    EngineArmDriver driver;std::array<vr_arm::Matrix,407> buffer{};const auto start=EngineArmDriver::Clock::now();
    EngineArmProbe::Pose pose;pose.owner.actor=7;pose.owner.instance=8;pose.layout=layout;pose.world=root;pose.world_valid=true;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());pose.received=start;
    for(size_t i=0;i<names.size();++i)if(mapping[i]>=0){buffer[mapping[i]]=locals[i];pose.bones.push_back({static_cast<uint32_t>(i),mapping[i],locals[i]});}
    const auto native=buffer;const auto path=std::filesystem::temp_directory_path()/"sm2vr_body_control_order.txt";
    std::mutex gate_mutex;std::condition_variable gate;bool entered=false,release=false,locked=false;std::atomic<unsigned> reads{};
    driver.configure({[](const auto&,auto){return true;},[&](auto){locked=true;},[&](auto){locked=false;},[&](uintptr_t address,const auto& value){check(locked,"out-of-order control still uses native lock");std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;},
        [&](const std::filesystem::path& requested)->vr_body_placement::ControlRead {
            check(requested==path,"production poll uses configured control path");
            const auto read=++reads;
            if(read==1){std::unique_lock lock{gate_mutex};entered=true;gate.notify_all();gate.wait(lock,[&]{return release;});}
            return {vr_body_placement::Settings{read==1?.2f:0.f},vr_body_placement::ControlStatus::Valid};
        }});
    driver.enable(true);driver.follow_weapon(true);driver.allow(true,3);driver.set_body_placement({.2f});driver.set_body_placement_control(path);
    EngineArmDriver::Targets tracked;tracked.actor=7;tracked.reference=1;tracked.epoch=3;tracked.units_per_metre=.9f;tracked.captured=start;
    for(size_t hand=0;hand<2;++hand){auto wrist=vr_camera::IDENTITY;vr_arm::position(wrist,targets[hand]);tracked.wrists[hand]=wrist;}
    driver.targets(tracked);std::exception_ptr delayed_error;
    std::jthread delayed{[&]{try{driver.apply(pose,start);}catch(...){delayed_error=std::current_exception();}}};
    struct Release {std::mutex& mutex;std::condition_variable& gate;bool& released;~Release(){{std::scoped_lock lock{mutex};released=true;}gate.notify_all();}} release_on_exit{gate_mutex,gate,release};
    bool started;{std::unique_lock lock{gate_mutex};started=gate.wait_for(lock,std::chrono::seconds{2},[&]{return entered;});}
    if(started){
        auto newer=pose;newer.received=start+std::chrono::milliseconds{1500};tracked.captured=newer.received;driver.targets(tracked);driver.apply(newer,newer.received);
    }
    {std::scoped_lock lock{gate_mutex};release=true;}gate.notify_all();delayed.join();if(delayed_error)std::rethrow_exception(delayed_error);
    check(started && reads==2,"newer production poll completes while older same-path read is blocked");
    // No third read is due yet. A late old result must not change the setting
    // used by this next fresh, actual mapped publication.
    buffer=native;auto next=pose;next.received=start+std::chrono::milliseconds{1600};tracked.captured=next.received;driver.targets(tracked);driver.apply(next,next.received);
    check(buffer[mapping[index("CENTRE")]]==native[mapping[index("CENTRE")]],"late old control result cannot re-enable mapped body placement after newer zero");
    const auto state=driver.snapshot();check(state.body_back_metres==0 && state.body_applied_back_metres==0 && state.body_control_revision==1 && state.body_control_polls==2 && state.body_control_superseded==1 && reads==2,
        "superseded reads cannot overwrite a newer acknowledged control value or revision");
}
inline void live_control() {
    using namespace body_fixture;
    const auto path=std::filesystem::temp_directory_path()/("sm2vr_body_control_"+std::to_string(GetCurrentProcessId())+".txt");
    const auto trace_path=std::filesystem::temp_directory_path()/("sm2vr_body_control_"+std::to_string(GetCurrentProcessId())+".jsonl");
    std::error_code error;std::filesystem::remove(path,error);
    struct Cleanup {std::filesystem::path path,trace;~Cleanup(){std::error_code e;std::filesystem::remove(path,e);std::filesystem::remove(trace,e);}} cleanup{path,trace_path};
    auto layout=std::make_shared<EngineArmProbe::RigLayout>();layout->names.assign(names.begin(),names.end());layout->parents.assign(parents.begin(),parents.end());layout->bind_locals.assign(locals.begin(),locals.end());
    EngineArmDriver driver;std::array<vr_arm::Matrix,407> buffer{};const auto start=EngineArmDriver::Clock::now();
    EngineArmProbe::Pose pose;pose.owner.actor=7;pose.owner.instance=8;pose.layout=layout;pose.world=root;pose.world_valid=true;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
    for(size_t i=0;i<names.size();++i)if(mapping[i]>=0){buffer[mapping[i]]=locals[i];pose.bones.push_back({static_cast<uint32_t>(i),mapping[i],locals[i]});}
    const auto native=buffer;bool locked{};driver.configure({[](const auto&,auto){return true;},[&](auto){locked=true;},[&](auto){locked=false;},[&](uintptr_t address,const auto& value){check(locked,"live control never bypasses native publication lock");std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;}});
    driver.enable(true);driver.follow_weapon(true);driver.allow(true,3);
    EngineArmDriver::Targets tracked;tracked.actor=7;tracked.reference=1;tracked.epoch=3;tracked.units_per_metre=.9f;
    for(size_t hand=0;hand<2;++hand){auto wrist=vr_camera::IDENTITY;vr_arm::position(wrist,targets[hand]);tracked.wrists[hand]=wrist;}
    std::vector<vr_arm::Matrix> original;check(vr_arm::hierarchy(locals,parents,root,original),"live control captured hierarchy");
    const auto write=[&](const std::string& value){std::ofstream out{path,std::ios::binary|std::ios::trunc};out<<value;out.close();check(bool(out),"live control fixture writes");};
    check(vr_trace::Recorder::get().open(trace_path,10,start),"live control trace opens");
    std::optional<vr_arm::Matrix> gun_reference;
    const auto step=[&](int ms,float expected){
        const auto now=start+std::chrono::milliseconds{ms};buffer=native;pose.received=now;tracked.captured=now;driver.targets(tracked);
        driver.apply(pose,now);vr_trace::Recorder::get().tick(now);
        auto published=layout->bind_locals;for(size_t i=0;i<names.size();++i)if(mapping[i]>=0)published[i]=buffer[mapping[i]];
        std::vector<vr_arm::Matrix> world;check(vr_arm::hierarchy(published,parents,root,world),"live control mapped publication reconstructs");
        auto centre=original[index("CENTRE")];vr_arm::position(centre,vr_arm::add(vr_arm::position(centre),{0,0,expected*.9f}));
        check(close(world[index("CENTRE")],centre),"real apply polls control file and changes actual mapped CENTRE output");
        for(size_t hand=0;hand<2;++hand)check(close(world[index(hand?"HANDR":"HANDL")],*tracked.wrists[hand]),"live control preserves exact wrist targets");
        if(!gun_reference)gun_reference=world[index("Gun_pose")];
        check(close(world[index("Gun_pose")],*gun_reference) && close(world[index("Trajectory")],original[index("Trajectory")]),"live control preserves gun and native camera/contact branch");
        check(driver.snapshot().body_applied_back_metres==expected && !locked,"actual committed offset is observable");
    };
    step(0,0);check(driver.snapshot().body_control_polls==0,"default no-file path never polls");
    check(!driver.set_body_placement_control("relative_body_placement.txt"),"relative control path refused");
    check(driver.set_body_placement_control(path),"absolute per-run control configured");
    step(100,0);check(driver.snapshot().body_control_status==vr_body_placement::ControlStatus::Unavailable,"missing file retains default and reports unavailable");
    write("0.2\r\n");step(600,0);check(driver.snapshot().body_control_polls==1,"control polling throttles to one second");
    step(1100,.2f);check(driver.snapshot().body_control_revision==1,"accepted control acknowledges revision");
    write("0\n");step(1600,.2f);step(2100,0);check(driver.snapshot().body_control_revision==2,"explicit zero resets native body placement");
    write("0.5\n");step(3100,0);check(driver.snapshot().body_control_status==vr_body_placement::ControlStatus::Invalid,"out of range rejected");
    write("0.2\n");step(4100,.2f);
    int time=5100;
    for(const std::string& bad:{"nan\n","-0.1\n","0.2 extra\n","0.2\n0\n",""," 0.2\n"}){write(bad);step(time,.2f);time+=1000;check(driver.snapshot().body_control_status==vr_body_placement::ControlStatus::Invalid,"malformed control retains last valid applied value");}
    write("0.2"+std::string(61,'0'));step(time,.2f);time+=1000;check(driver.snapshot().body_control_status==vr_body_placement::ControlStatus::Oversized,"bounded read refuses a valid-looking numeric truncation");
    std::filesystem::remove(path);step(time,.2f);time+=1000;check(driver.snapshot().body_control_status==vr_body_placement::ControlStatus::Unavailable,"deleted control does not silently switch pose");
    write("0.4\n");step(time,.4f);time+=1000;write("0\n");step(time,0);time+=1000;
    driver.set_body_placement_control({});check(driver.set_body_placement({.3f}),"startup-only settings remain supported");step(time,.3f);
    check(driver.snapshot().body_control_rejected==10 && driver.snapshot().body_control_polls==15,"every bounded invalid/missing read is counted without polling disabled controls");
    vr_trace::Recorder::get().close();std::ifstream input{trace_path};std::string all{std::istreambuf_iterator<char>{input},{}};
    check(all.find("\"applied_back_metres\":0.2000,\"control_revision\":1,\"control_status\":\"valid\"")!=std::string::npos &&
          all.find("\"applied_back_metres\":0.0000,\"control_revision\":2,\"control_status\":\"valid\"")!=std::string::npos,
          "production trace stamps actual committed on/off values and acknowledged control revision");
}
inline void run() {
    using namespace body_fixture;
    const std::vector<std::string> strings(names.begin(),names.end());
    check(vr_body_placement::parse_config({}).settings.back_metres==0,"body placement defaults off");
    check(vr_body_placement::parse_config(std::string{"0.2"}).settings.back_metres==.2f,"strict body placement config");
    for(const char* bad:{"","-0.2","0.5","nan","inf","0.2 trailing"," 0.2","0.2m"}) {
        const auto config=vr_body_placement::parse_config(std::string{bad});
        check(!config.error.empty() && config.settings.back_metres==0,"malformed placement disabled");
    }
    auto duplicate_mapping=mapping;duplicate_mapping[index("Trajectory")]=mapping[index("CENTRE")];
    vr_body_placement::Refusal why{};
    check(!vr_body_placement::place(locals,parents,strings,duplicate_mapping,root,.9f,{.2f},&why) && why==vr_body_placement::Refusal::Mapping,"aliased CENTRE skin slot rejected with bounded mapping reason");
    auto extra_root_branch=parents;extra_root_branch[index("FOOT1L")]=0;
    check(!vr_body_placement::place(locals,extra_root_branch,strings,mapping,root,.9f,{.2f}),"unexpected root branch cannot split the body");
    // A native parent can pass the unchanged strict guard without being perfectly
    // orthonormal. The old complete parent/inverse round trip doubled this error.
    auto parent_fixture=locals;parent_fixture[0][0]=parent_fixture[0][10]=1.000035f;
    std::vector<vr_arm::Matrix> near_world;
    check(vr_arm::hierarchy(parent_fixture,parents,root,near_world),"captured layout accepts near-rigid native parent");
    const auto round_trip=vr_camera::multiply(near_world[index("CENTRE")],vr_camera::inverse(near_world[0]));
    check(!vr_camera::rigid(round_trip),"old full-matrix round trip rejects an otherwise accepted parent fixture");
    const auto translated=vr_body_placement::place(parent_fixture,parents,strings,mapping,root,.9f,{.2f},&why);
    check(translated && why==vr_body_placement::Refusal::None,"translation-only placement accepts near-rigid parent without relaxing guard");
    for(size_t component=0;component<16;++component) if(component<12 || component==15)
        check(translated->locals[index("CENTRE")][component]==parent_fixture[index("CENTRE")][component],"CENTRE nontranslation components remain byte-exact");
    for(float yaw:{0.f,1.5707963f,3.1415926f}) for(float units:{.9f,1.7f}) {
        const auto yaw_matrix=vr_upright::rotation({0,1,0},yaw);
        auto actor=vr_camera::multiply(root,yaw_matrix);
        const auto result=vr_body_placement::place(locals,parents,strings,mapping,actor,units,{.2f});
        check(result.has_value(),"actual mapped native rig accepts CENTRE placement");
        std::vector<vr_arm::Matrix> before,after;
        check(vr_arm::hierarchy(locals,parents,actor,before) && vr_arm::hierarchy(result->locals,parents,actor,after),"captured hierarchy reconstructs");
        const vr_arm::Vector offset{-actor[8]*.2f*units,0,-actor[10]*.2f*units};
        check(vr_arm::length(vr_arm::sub(offset,result->world_offset))<1e-6f,"placement uses yawed actor basis and units exactly once");
        for(size_t i=0;i<names.size();++i) {
            bool body=false;for(int32_t j=static_cast<int32_t>(i);j>=0;j=parents[j]) if(static_cast<size_t>(j)==index("CENTRE")) body=true;
            auto expected=before[i];if(body) vr_arm::position(expected,vr_arm::add(vr_arm::position(expected),offset));
            check(close(after[i],expected),"every body descendant shifts rigidly; actor/Trajectory/camera/contact target branch unchanged");
            if(i!=index("CENTRE")) check(result->locals[i]==locals[i],"placement changes only CENTRE local");
        }
        for(size_t component=0;component<16;++component) if(component<12 || component==15)
            check(result->locals[index("CENTRE")][component]==locals[index("CENTRE")][component],"ordinary CENTRE nontranslation components remain byte-exact");
        // Drive the real mapped publication, including its ownership guard, native lock,
        // weapon attachment and shot-pose observer. Expected hand poses do not move.
        auto layout=std::make_shared<EngineArmProbe::RigLayout>();layout->names=strings;
        layout->parents.assign(parents.begin(),parents.end());layout->bind_locals.assign(locals.begin(),locals.end());
        const auto now=EngineArmDriver::Clock::now();
        EngineArmProbe::Pose pose;pose.owner.actor=7;pose.owner.instance=8;pose.layout=layout;pose.world=actor;pose.world_valid=true;pose.received=now;
        std::array<vr_arm::Matrix,407> buffer{};
        for(size_t i=0;i<names.size();++i) if(mapping[i]>=0) {buffer[mapping[i]]=locals[i];pose.bones.push_back({static_cast<uint32_t>(i),mapping[i],locals[i]});}
        const auto native=buffer;pose.buffer=reinterpret_cast<uintptr_t>(buffer.data());
        EngineArmDriver driver;bool locked=false,current=true;size_t writes{},fail_write{};
        driver.configure({[&](const auto&,auto){return current;},[&](auto){locked=true;},[&](auto){locked=false;},[&](uintptr_t address,const auto& value){
            check(locked,"body placement shares native publication lock");++writes;if(fail_write && writes==fail_write)return false;
            std::memcpy(reinterpret_cast<void*>(address),value.data(),sizeof(value));return true;
        }});
        driver.enable(true);driver.allow(true,3);driver.follow_weapon(true);
        EngineArmDriver::Targets tracked;tracked.actor=7;tracked.reference=1;tracked.epoch=3;tracked.captured=now;tracked.units_per_metre=units;
        for(size_t hand=0;hand<2;++hand) {auto wrist=vr_camera::IDENTITY;vr_arm::position(wrist,targets[hand]);tracked.wrists[hand]=vr_camera::multiply(wrist,yaw_matrix);}
        VrWeaponAim::Pose published;driver.set_weapon_observer([&](const auto& weapon){published=weapon;});
        driver.targets(tracked);driver.apply(pose,now);
        check(driver.snapshot().body_placement_updates==0 && buffer[mapping[index("CENTRE")]]==native[mapping[index("CENTRE")]],"default native body placement preserved");
        check(published.actor==7,"captured loadout publishes tracked shot pose");const auto baseline_shot=published.world;
        buffer=native;writes=0;check(driver.set_body_placement({.2f}),"candidate accepted");
        driver.targets(tracked);driver.apply(pose,now);
        auto changed=layout->bind_locals;for(size_t i=0;i<names.size();++i) if(mapping[i]>=0)changed[i]=buffer[mapping[i]];
        check(vr_arm::hierarchy(changed,parents,actor,after),"published mapped body reconstructs");
        for(size_t hand=0;hand<2;++hand)check(close(after[index(hand?"HANDR":"HANDL")],*tracked.wrists[hand]),"placement preserves exact tracked wrist position and orientation");
        check(published.actor==7 && close(published.world,baseline_shot),"rendered gun/shot world pose remains unchanged by body placement");
        check(close(after[index("Gun_pose")],published.world),"rendered weapon and shot endpoint agree after placement");
        for(const char* name:{"CENTRE","FOOT1L","FOOT1R","melee_weapon_pivot"}) {
            auto expected=before[index(name)];vr_arm::position(expected,vr_arm::add(vr_arm::position(expected),offset));
            check(close(after[index(name)],expected),"pelvis, feet and holstered melee shift together without waist shear");
        }
        for(const char* name:{"CharacterWorldSpaceTM","Trajectory","cam_root","cam_anim","foot_targ_L","foot_targ_R"})
            check(close(after[index(name)],before[index(name)]),"native actor/camera/contact branch unchanged");
        check(driver.snapshot().body_placement_updates==1 && !locked,"placement counted only after successful atomic commit");
        // Missing CENTRE mapping, invalid parent transform, stale ownership and a
        // partial native write must never publish a partially moved body.
        auto missing=pose;std::erase_if(missing.bones,[](const auto& bone){return bone.rig_index==index("CENTRE");});
        buffer=native;writes=0;driver.apply(missing,now);check(buffer==native && writes==0,"unmapped CENTRE rejects whole batch before writes");
        auto singular_layout=std::make_shared<EngineArmProbe::RigLayout>(*layout);singular_layout->bind_locals[0]={};
        auto singular=pose;singular.layout=singular_layout;driver.apply(singular,now);check(buffer==native && writes==0,"singular CENTRE parent rejects before writes");
        auto no_centre=std::make_shared<EngineArmProbe::RigLayout>(*layout);no_centre->names[index("CENTRE")]="missing";
        auto absent=pose;absent.layout=no_centre;driver.apply(absent,now);check(buffer==native && writes==0,"unknown body hierarchy rejects before writes");
        auto gun_unmapped=pose;std::erase_if(gun_unmapped.bones,[](const auto& bone){return bone.rig_index==index("Gun_pose");});
        const auto rejected_before=driver.snapshot().body_placement_rejected;
        driver.apply(gun_unmapped,now);
        check(buffer==native && writes==0 && !published.actor && driver.snapshot().body_placement_rejected==rejected_before+1,"unmapped gun refuses body/arm placement and shot before writes");
        auto gun_unknown_layout=std::make_shared<EngineArmProbe::RigLayout>(*layout);gun_unknown_layout->names[index("Gun_pose")]="unknown_gun";
        auto gun_unknown=pose;gun_unknown.layout=gun_unknown_layout;driver.apply(gun_unknown,now);
        check(buffer==native && writes==0 && !published.actor && driver.snapshot().body_placement_rejected==rejected_before+2,"unknown gun refuses entire placement batch");
        auto gun_ancestry_layout=std::make_shared<EngineArmProbe::RigLayout>(*layout);gun_ancestry_layout->parents[index("GUN_ANIM")]=static_cast<int32_t>(index("CENTRE"));
        auto gun_ancestry=pose;gun_ancestry.layout=gun_ancestry_layout;driver.apply(gun_ancestry,now);
        check(buffer==native && writes==0 && !published.actor && driver.snapshot().body_placement_rejected==rejected_before+3,"unsupported gun ancestry refuses entire placement batch");
        check(driver.snapshot().body_placement_refusals[static_cast<size_t>(vr_body_placement::Refusal::Attachment)]==3 &&
              driver.snapshot().body_placement_refusals[static_cast<size_t>(vr_body_placement::Refusal::Mapping)]==1 &&
              driver.snapshot().body_placement_refusals[static_cast<size_t>(vr_body_placement::Refusal::Hierarchy)]==1 &&
              driver.snapshot().body_placement_refusals[static_cast<size_t>(vr_body_placement::Refusal::Topology)]==1,"native refusal counters preserve precise bounded stage reasons");
        current=false;driver.apply(pose,now);check(buffer==native && writes==0,"lost native ownership refuses candidate");current=true;
        driver.apply(pose,now+std::chrono::milliseconds{101});check(buffer==native && writes==0,"stale target refuses candidate");
        driver.allow(false,3);driver.apply(pose,now);check(buffer==native && writes==0,"focus loss refuses candidate");driver.allow(true,3);driver.targets(tracked);
        fail_write=2;driver.apply(pose,now);check(buffer==native && driver.snapshot().failures==1 && driver.snapshot().body_placement_updates==1 && !published.actor,"failed mapped batch rolls back body and arms, publishes no shot");
        fail_write=0;writes=0;driver.follow_weapon(false);driver.apply(gun_unknown,now);
        check(buffer[mapping[index("CENTRE")]]!=native[mapping[index("CENTRE")]] && writes>0 && driver.snapshot().body_placement_updates==2 && !published.actor,"explicit weapon-follow-off still permits body placement without an attachment");
        driver.follow_weapon(true);driver.hide_head(true);
        const auto before_sleep=driver.snapshot();
        for(unsigned active:{0u,1u,2u}) {
            auto partial=tracked;
            for(size_t hand=0;hand<2;++hand) if(!(active&(1u<<hand)))partial.wrists[hand].reset();
            buffer=native;writes=0;driver.targets(partial);driver.apply(pose,now);
            check(buffer[mapping[index("CENTRE")]]==native[mapping[index("CENTRE")]] && buffer[mapping[index("HEAD")]]!=native[mapping[index("HEAD")]],"zero/partial wrist tracking suspends body placement while head hiding still commits");
            check(driver.snapshot().body_placement_updates==before_sleep.body_placement_updates && driver.snapshot().body_placement_rejected==before_sleep.body_placement_rejected,"missing wrists suspend candidate without triggering full-tracked weapon refusal");
            if(active) {
                auto partial_locals=layout->bind_locals;
                for(size_t i=0;i<names.size();++i) if(mapping[i]>=0)partial_locals[i]=buffer[mapping[i]];
                // HEAD/helmet deliberately contain non-rigid hiding matrices. Restore
                // only those independent locals for FK of the unaffected arm branch.
                for(const char* name:{"HEAD","helmet"})partial_locals[index(name)]=locals[index(name)];
                check(vr_arm::hierarchy(partial_locals,parents,actor,after),"remaining wrist hierarchy reconstructs");
                const size_t hand=active==1?0:1;
                check(close(after[index(hand?"HANDR":"HANDL")],*partial.wrists[hand]),"remaining tracked hand retains normal exact IK during candidate suspension");
            }
        }
        check(driver.snapshot().body_placement_suspended==before_sleep.body_placement_suspended+3 && driver.snapshot().head_updates==before_sleep.head_updates+3,"sleeping/partial controllers each report suspension and successful head hide");
        buffer=native;driver.targets(tracked);driver.apply(pose,now);
        check(driver.snapshot().body_placement_updates==before_sleep.body_placement_updates+1 && driver.snapshot().body_placement_suspended==before_sleep.body_placement_suspended+3 && published.actor==7,"both fresh wrists resume normal atomic body/weapon placement");
    }
    auto tilted=vr_upright::rotation({1,0,0},.4f);
    const auto level=vr_body_placement::place(locals,parents,strings,mapping,tilted,.9f,{.2f});
    check(level && std::abs(level->world_offset[1])<1e-7f && std::abs(vr_arm::length(level->world_offset)-.18f)<1e-6f,"actor pitch does not lift or shorten body offset");
    auto vertical=vr_upright::rotation({1,0,0},1.5707963f);
    check(!vr_body_placement::place(locals,parents,strings,mapping,vertical,.9f,{.2f}),"undefined level forward rejected");
    live_control();
    out_of_order_control();
}
} // namespace body_placement_tests
