#include "synthetic_code.h"
#include "NativePipExperiment.h"
#include <iostream>
#include <stdexcept>
#include <limits>
#include <vector>
#include <atomic>
#include <future>
#include <thread>

namespace {
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
struct Fixture {
    using Experiment=NativePipExperiment;
    Experiment experiment;
    alignas(16) std::array<uint8_t,0x388> camera{};
    std::array<uint8_t,0x48*8> records{};
    Experiment::Array array{reinterpret_cast<uintptr_t>(records.data()),0,8};
    std::array<uint8_t,0x40> manager{};
    std::array<uint8_t,0x18> texture{};
    Experiment::Array pool{reinterpret_cast<uintptr_t>(texture.data()),1,1};
    unsigned creates{},releases{};uintptr_t last_camera{},context_override{},manager_override{};
    std::function<void()> on_context,on_create;
    Fixture(Experiment::Now now=[]{return std::chrono::steady_clock::now();}) {
        auto world=vr_camera::IDENTITY;world[10]=-1;world[12]=10;world[13]=20;world[14]=30;
        const auto view=vr_camera::inverse(world);
        std::memcpy(camera.data(),world.data(),64);std::memcpy(camera.data()+64,view.data(),64);
        const float fov=73.7f;std::memcpy(camera.data()+0x2d0,&fov,4);
        const auto pool_address=reinterpret_cast<uintptr_t>(&pool);
        std::memcpy(manager.data()+0x30,&pool_address,8);
        const uintptr_t target=0x12345678;std::memcpy(texture.data(),&target,8);
        Experiment::Size size{648,368};std::memcpy(texture.data()+8,&size,8);
        require(experiment.bind({[this]{if(on_context) on_context();return context_override?context_override:reinterpret_cast<uintptr_t>(&array);},[this]{return manager_override?manager_override:reinterpret_cast<uintptr_t>(manager.data());},
            [this](uintptr_t,int32_t* index,const Experiment::Size* size,const Experiment::Request* request,bool reuse) {
                require(!reuse,"experiment must not consume native free slots");
                require(size->width==641 && size->height==361,"pass exact request to native allocator");
                if(*index==-1) *index=array.count++;
                require(request->flags==0x50,"request expanded-effects PIP branch with inherited exposure");
                last_camera=request->camera;++creates;
                std::memcpy(records.data()+*index*0x48+8,&request->camera,8);
                int32_t target_index=0;std::memcpy(records.data()+*index*0x48+0x40,&target_index,4);
                if(on_create) on_create();
            },[this](uintptr_t,int32_t index,bool immediate,bool recycle) {
                require(index==0 && immediate && !recycle,"release only owned request immediately without recycling");++releases;
                uintptr_t zero{};std::memcpy(records.data()+8,&zero,8);
            },std::move(now)}),"bind fake native API");
    }
    void arm(uint32_t frames=2,uint32_t warmup=0) {Experiment::Config config;config.size={641,361};config.frames=frames;config.warmup_frames=warmup;require(experiment.arm(config),"arm bounded experiment");}
    void main(uint64_t id=1,bool gameplay=true) {experiment.on_main(camera.data(),id,gameplay,uint32_t(100+id));}
};
}
int main() {
    try {
        using E=NativePipExperiment;
        require(E::matches(E::BUILD_TIMESTAMP,SYNTH(E::GET_BYTES),SYNTH(E::REGISTER_BYTES),SYNTH(E::RELEASE_BYTES)),"known build accepted");
        auto bytes=SYNTH(E::REGISTER_BYTES);bytes[3]^=1;
        require(!E::matches(E::BUILD_TIMESTAMP,SYNTH(E::GET_BYTES),bytes,SYNTH(E::RELEASE_BYTES)),"changed native code rejected");
        require(!E::matches(0,SYNTH(E::GET_BYTES),SYNTH(E::REGISTER_BYTES),SYNTH(E::RELEASE_BYTES)),"unknown timestamp rejected");
        E unbound;require(!unbound.arm({}),"cannot arm without native bindings");
        {
            const auto base=E::Time{}+std::chrono::seconds(10);int64_t milliseconds{};
            Fixture f{[&]{return base+std::chrono::milliseconds(milliseconds);}};f.arm(10);f.main();
            auto evidence=f.experiment.inspect_owned_camera();
            require(evidence && evidence->current_eligible && evidence->history.size()==1 && evidence->history[0].render_id==1 &&
                evidence->history[0].updates==1 && evidence->history[0].token==101 && evidence->history[0].age_ms==0,"history diagnostics retain exact accepted job identity and age");
            milliseconds=250;require(f.experiment.owned_camera().has_value(),"250 ms boundary remains eligible");
            milliseconds=251;evidence=f.experiment.inspect_owned_camera();
            require(evidence && !evidence->current_eligible && evidence->recent_worlds.empty() && evidence->history[0].age_ms==251 &&
                !f.experiment.owned_camera(),"expired pose remains diagnostic evidence but cannot drive a lens");
            evidence=f.experiment.inspect_owned_camera(base-std::chrono::milliseconds(1));
            require(evidence && evidence->history[0].age_ms==-1 && !evidence->history[0].eligible &&
                !f.experiment.owned_camera(base-std::chrono::milliseconds(1)),"an explicitly future-relative pose remains ineligible and identifiable");
            milliseconds=0;f.on_context=[&]{milliseconds=251;};
            require(!f.experiment.owned_camera(),"default eligibility clock is sampled after ownership validation, not before waiting or native context work");
        }
        {
            const auto base=E::Time{}+std::chrono::seconds(10);
            std::atomic<int64_t> milliseconds{};std::atomic<DWORD> query_thread{};std::atomic<bool> checked{};
            Fixture f{[&]{
                require(GetCurrentThreadId()!=query_thread || checked.load(),"ownership clock must follow validation under the acquired mutex");
                return base+std::chrono::milliseconds(milliseconds.load());
            }};f.arm(10);f.main();
            std::array<uint8_t,0xa60> entry{};std::array<uint8_t,0x148> renderer{};
            const auto address=reinterpret_cast<uintptr_t>(entry.data());std::memcpy(renderer.data()+0x140,&address,8);
            const uint64_t flags=0x40;const int32_t record=0;std::memcpy(entry.data(),&flags,8);std::memcpy(entry.data()+0xa50,&record,4);
            auto world=vr_camera::IDENTITY;world[10]=-1;world[12]=11;world[13]=20;world[14]=30;const auto view=vr_camera::inverse(world);
            std::memcpy(f.camera.data(),world.data(),64);std::memcpy(f.camera.data()+64,view.data(),64);
            std::promise<void> holding,release,starting;auto released=release.get_future().share();
            f.on_create=[&]{std::memcpy(entry.data()+0x34,reinterpret_cast<void*>(f.last_camera),64);holding.set_value();released.wait();};
            f.on_context=[&]{if(GetCurrentThreadId()==query_thread) checked=true;};
            auto writer=std::async(std::launch::async,[&]{f.main(2);});holding.get_future().wait();
            auto reader=std::async(std::launch::async,[&]{
                query_thread=GetCurrentThreadId();starting.set_value();
                const auto owned=f.experiment.owned_camera();
                require(owned && owned->updates==2 && owned->current_eligible,"contended reader sees freshly published owned pose");
                checked=false;const auto origin=f.experiment.render_origin(reinterpret_cast<uintptr_t>(renderer.data()));
                require(origin && origin->render_id==2 && origin->token==102,"writer origin also uses its post-lock clock and exact accepted source");
            });
            starting.get_future().wait();milliseconds=200;release.set_value();writer.get();reader.get();
        }
        {
            Fixture f;f.arm(10);f.main();
            std::array<uint8_t,0xa60> entry{};std::array<uint8_t,0x148> renderer{};
            const auto entry_address=reinterpret_cast<uintptr_t>(entry.data()),renderer_address=reinterpret_cast<uintptr_t>(renderer.data());
            std::memcpy(renderer.data()+0x140,&entry_address,8);const uint64_t flags=0x40;std::memcpy(entry.data(),&flags,8);
            const int32_t record=0;std::memcpy(entry.data()+0xa50,&record,4);std::memcpy(entry.data()+0x34,reinterpret_cast<void*>(f.last_camera),64);
            auto origin=f.experiment.render_origin(renderer_address);
            require(origin && origin->render_id==1 && origin->token==101 && origin->source_world==origin->render_world,"actual owned native entry identifies its unique recorded camera job");
            f.main(2);require(!f.experiment.render_origin(renderer_address),"identical stationary poses across two jobs are ambiguous and cannot inherit latest identity");
            require(!f.experiment.render_origin(1),"unreadable renderer cannot identify a writer camera");
        }
        {
            Fixture f;E::Config config;config.size={641,361};config.frames=4096;config.warmup_frames=0;config.shift=0;config.pose_tags=true;
            require(f.experiment.arm(config),"arm desktop camera tags");const auto source=f.camera;
            std::array<uint8_t,0xa60> entry{};std::array<uint8_t,0x148> renderer{};
            const auto entry_address=reinterpret_cast<uintptr_t>(entry.data()),renderer_address=reinterpret_cast<uintptr_t>(renderer.data());
            std::memcpy(renderer.data()+0x140,&entry_address,8);const uint64_t flags=0x40;std::memcpy(entry.data(),&flags,8);
            const int32_t record=0;std::memcpy(entry.data()+0xa50,&record,4);
            std::vector<vr_camera::Matrix> worlds;
            for(uint64_t id=1;id<=4096;++id) {
                f.main(id);vr_camera::Matrix world{};std::memcpy(world.data(),reinterpret_cast<void*>(f.last_camera),64);
                require(std::abs(world[12]-10)<=.0076f && std::abs(world[13]-20)<=.0076f && std::abs(world[14]-30)<=.0076f && f.camera==source,"camera tags are bounded and never modify the source camera");
                require(std::ranges::none_of(worlds,[&](const auto& old){return old==world;}),"all 4096 tags remain distinct at a stationary source pose");worlds.push_back(world);
                std::memcpy(entry.data()+0x34,world.data(),64);const auto origin=f.experiment.render_origin(renderer_address);
                require(origin && origin->render_id==id && origin->token==100+id,"tagged native entry resolves the exact source job through history rollover");
            }
            std::memcpy(entry.data()+0x34,worlds[4094].data(),64);const auto historical=f.experiment.render_origin(renderer_address);
            require(historical && historical->render_id==4095,"rendering a preceding tagged pose preserves its own source identity");
            require(!f.experiment.render_origin(renderer_address,std::chrono::steady_clock::now()+std::chrono::seconds(1)),"expired camera identity is refused");
            const uint64_t main_flags=0;std::memcpy(entry.data(),&main_flags,8);require(!f.experiment.render_origin(renderer_address),"main view cannot claim owned PIP provenance");
        }
        {
            Fixture f;E::Config config;config.size={641,361};config.frames=64;config.warmup_frames=0;config.shift=0;config.pose_tags=true;
            require(f.experiment.arm(config),"arm full-history collision control");
            f.main(1);vr_camera::Matrix first{};std::memcpy(first.data(),reinterpret_cast<void*>(f.last_camera),64);
            for(uint64_t id=2;id<=33;++id) f.main(id);
            vr_camera::Matrix source{};std::memcpy(source.data(),f.camera.data(),64);
            // Deliberate motion makes update 34's tag coincide with update 1,
            // which has already left the 16-entry lens-pose history.
            const float right=-.0065f,up=-.0055f,forward=-.0075f;
            for(size_t axis=0;axis<3;++axis) source[12+axis]=first[12+axis]-source[axis]*right-source[4+axis]*up-source[8+axis]*forward;
            const auto inverse=vr_camera::inverse(source);std::memcpy(f.camera.data(),source.data(),64);std::memcpy(f.camera.data()+64,inverse.data(),64);f.main(34);
            std::array<uint8_t,0xa60> entry{};std::array<uint8_t,0x148> renderer{};
            const auto entry_address=reinterpret_cast<uintptr_t>(entry.data());std::memcpy(renderer.data()+0x140,&entry_address,8);
            const uint64_t flags=0x40;std::memcpy(entry.data(),&flags,8);const int32_t record=0;std::memcpy(entry.data()+0xa50,&record,4);
            std::memcpy(entry.data()+0x34,first.data(),64);
            require(!f.experiment.render_origin(reinterpret_cast<uintptr_t>(renderer.data())),"an old pose outside the recent ring still prevents false newer-job attribution");
        }
        {
            Fixture f;const auto original=f.camera;f.main();require(f.creates==0,"default inert");f.arm();
            f.main(0);f.main(1,false);require(f.creates==0,"unknown frame or non-gameplay must not allocate");
            f.main();auto s=f.experiment.snapshot();
            require(s.active && s.updates==1 && s.texture==0x12345678 && s.allocated.width==648 && s.allocated.height==368,"record actual native texture/rounded dimensions");
            require(f.camera==original && f.last_camera!=reinterpret_cast<uintptr_t>(f.camera.data()),"never mutate or retain persistent source as PIP camera");
            vr_camera::Matrix world{},view{};std::memcpy(world.data(),reinterpret_cast<void*>(f.last_camera),64);std::memcpy(view.data(),reinterpret_cast<void*>(f.last_camera+64),64);
            require(std::abs(world[12]-10.1f)<1e-5f && std::abs(view[12]+10.1f)<1e-5f,"second view has distinct position and matching inverse");
            const auto owned=f.experiment.owned_camera();require(owned && owned->record==0 && owned->world==world && owned->updates==1,"final-lens target exposes only the currently owned native camera");
            require(owned->recent_worlds.size()==1 && owned->recent_worlds[0]==world && !f.experiment.owned_camera(std::chrono::steady_clock::now()+std::chrono::seconds(1)),"recent owned-pose evidence expires without sleeping in tests");
            const auto stable=f.last_camera;f.main(1);require(f.creates==1,"duplicate frame rejected");
            f.main(2);require(f.last_camera==stable && f.creates==2,"stable owned camera storage and index reused");
            f.main(3);require(f.releases==1 && f.experiment.snapshot().finished,"frame budget releases request");
            require(!f.experiment.owned_camera(),"released camera cannot drive final lens updates");
            f.main(4);require(f.creates==2 && f.releases==1,"finished experiment cannot restart or double release");
        }
        {
            Fixture f;f.arm(10);f.main(1);f.main(2);
            vr_camera::Matrix world{};std::memcpy(world.data(),reinterpret_cast<void*>(f.last_camera),64);
            std::vector<uint8_t> list(0x29c78);std::array<uint8_t,0x148> renderer{};
            const auto render_list=reinterpret_cast<uintptr_t>(list.data()),add_list=render_list+0x160;
            const auto entry=*NativeCameraEntries::address(add_list,1),renderer_address=reinterpret_cast<uintptr_t>(renderer.data());
            std::memcpy(renderer.data()+0x140,&entry,8);const uint64_t flags=0x40;std::memcpy(reinterpret_cast<void*>(entry),&flags,8);
            const int32_t record=0;std::memcpy(reinterpret_cast<void*>(entry+0xa50),&record,4);std::memcpy(reinterpret_cast<void*>(entry+0x34),world.data(),64);
            auto& entries=NativeCameraEntries::get();entries.enable();const FrameSyncClock::Frame frame{102,2,0,0,false};
            auto ticket=entries.begin(add_list,NativeCameraEntries::Role::main,frame);entries.finish(*ticket,0,world);
            ticket=entries.begin(add_list,NativeCameraEntries::Role::pip,frame);entries.finish(*ticket,1,world);
            const std::array<NativeCameraEntries::Entry,2> actual{{{*NativeCameraEntries::address(add_list,0),0,NativeCameraEntries::Role::main,world},
                {entry,1,NativeCameraEntries::Role::pip,world}}};
            const auto receipt=entries.pair(render_list,actual,*entries.revision(render_list));require(receipt.has_value(),"stationary causal receipt setup");
            require(!f.experiment.render_origin(renderer_address),"pose alone remains ambiguous at a stationary camera");
            const auto origin=f.experiment.render_origin(renderer_address,{},&*receipt);
            require(origin && origin->render_id==2 && origin->token==102,"physical frame receipt resolves identical stationary poses without jitter");
            const auto camera=f.experiment.frame_camera(*receipt);
            require(camera && camera->updates==2 && camera->history.size()==1 && camera->history[0].render_id==2,"lens receives exactly the producing frame's settings");
            auto wrong=*receipt;wrong.pip_entry+=8;require(!f.experiment.render_origin(renderer_address,{},&wrong),"another physical entry cannot inherit a valid frame");
            wrong=*receipt;wrong.token=999;require(!f.experiment.render_origin(renderer_address,{},&wrong) && !f.experiment.frame_camera(wrong),"wrong producing token cannot use a matching pose");
            require(!f.experiment.frame_camera(*receipt,std::chrono::steady_clock::now()+std::chrono::seconds(1)),"causal identity never bypasses freshness");
            const auto expired=f.experiment.inspect_frame_camera(*receipt,std::chrono::steady_clock::now()+std::chrono::seconds(1));
            require(expired && !expired->current_eligible && expired->recent_worlds.empty() && expired->history.size()==1 && !expired->history[0].eligible && expired->history[0].age_ms>250,
                "causal inspection retains the exact expired producing frame for refusal diagnostics without making it eligible");
            entries.reset(add_list);require(!f.experiment.render_origin(renderer_address,{},&*receipt) && !f.experiment.frame_camera(*receipt),"reset invalidates origin and lens evidence immediately");
        }
        {Fixture f;f.arm();f.main();f.main(2,false);require(f.releases==1,"menu/scene handover releases owned request");}
        for(bool gameplay_exit:{false,true}) {
            Fixture f;E::Config config;config.size={641,361};config.frames=1;config.warmup_frames=0;config.retain_until_exit=true;
            require(f.experiment.arm(config),"arm bounded native retention");f.main(1);const auto camera=f.last_camera;
            f.main(2,!gameplay_exit);uintptr_t source{};std::memcpy(&source,f.records.data()+8,8);
            const auto state=f.experiment.snapshot();
            require(state.quarantined && state.finished && !state.active && !state.lost && !source && f.releases==0 && f.array.count==1 &&
                !f.records[0x3c] && !f.records[0x3d] && !f.texture[0x10] && f.last_camera==camera,
                "budget and gameplay exit null only the owned source while retaining unmarked native storage without Release/recycle");
            f.main(3);require(f.creates==1 && !f.experiment.arm(config) && !f.experiment.bind({[]{return uintptr_t{1};},[]{return uintptr_t{1};},
                [](uintptr_t,int32_t*,const E::Size*,const E::Request*,bool){},[](uintptr_t,int32_t,bool,bool){}}),"quarantined request cannot allocate, restart or rebind storage");
        }
        {
            Fixture f;E::Config config;config.size={641,361};config.frames=1;config.warmup_frames=0;config.retain_until_exit=true;
            require(f.experiment.arm(config),"arm failed-ownership retention control");f.main(1);f.texture[0x10]=1;f.main(2);
            uintptr_t source{};std::memcpy(&source,f.records.data()+8,8);
            require(f.experiment.snapshot().lost && !f.experiment.snapshot().quarantined && source==f.last_camera && f.releases==0,
                "changed pool ownership refuses both source mutation and native Release");
        }
        {Fixture f;f.arm();f.main();uintptr_t other=0xbeef;std::memcpy(f.records.data()+8,&other,8);
         require(!f.experiment.owned_camera(),"final lens rejects foreign ownership before the next camera update notices loss");f.main(2);
         require(f.experiment.snapshot().lost && f.releases==0 && f.creates==1,"ownership loss cannot release or overwrite another caller");}
        {Fixture f;f.arm();f.main();f.array.count=0;f.main(2);require(f.experiment.snapshot().lost && f.releases==0,"scene reset cannot reuse old index");}
        {Fixture f;f.arm();float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(f.camera.data(),&nan,4);f.main();require(f.creates==0,"invalid matrix rejected");}
        {Fixture f;f.arm(2,2);float title_fov=10.0402775f;std::memcpy(f.camera.data()+0x2d0,&title_fov,4);
         f.main(1);f.main(2);f.main(3);require(f.creates==0,"captured loading/title lens cannot start experiment even with zero game-state flags");
         const float gameplay_fov=73.7f;std::memcpy(f.camera.data()+0x2d0,&gameplay_fov,4);
         f.main(4);f.main(4);f.main(5);require(f.creates==0,"warmup counts distinct accepted frame IDs");
         f.main(6,false);f.main(7);f.main(8);require(f.creates==0,"gameplay interruption resets warmup");f.main(9);require(f.creates==1,"stable gameplay lens can start after warmup");f.main(10,false);}
        {Fixture f;E::Config config;config.size.width=99999;require(!f.experiment.arm(config),"oversized target rejected");config={};config.frames=0;require(!f.experiment.arm(config),"unbounded request rejected");}
        {E::Config config;config.size={1280,720};require(config.valid(),"tested 720p envelope accepted");
         config.size={1281,720};require(!config.valid(),"untested wider target rejected");
         config.size={1280,721};require(!config.valid(),"untested taller target rejected");
         config.size={3840,2160};require(!config.valid(),"known corrupting 4K request rejected before allocation");}
        {Fixture f;E::Config config;config.size={641,361};config.warmup_frames=0;config.lens_shift=.1f;config.shift=0;
         const std::array<float,4> rectangle{-.075f,-.042f,.15f,.084f};std::memcpy(f.camera.data()+0x12c,rectangle.data(),sizeof(rectangle));
         require(f.experiment.arm(config),"arm off-centre lens diagnostic");const auto original=f.camera;f.main();
         float left{};std::memcpy(&left,reinterpret_cast<void*>(f.last_camera+0x12c),4);
         require(std::abs(left+.06f)<1e-6f && f.camera==original && f.experiment.snapshot().center==f.experiment.snapshot().shifted,"offset owned lens while keeping persistent source and zero-shift position intact");f.main(2,false);}
        {
            Fixture f;f.arm(10);std::array<uint8_t,0x300> texture{};
            const auto texture_address=reinterpret_cast<uintptr_t>(texture.data());std::memcpy(f.texture.data(),&texture_address,8);
            const uintptr_t resource=0x11223344;std::memcpy(texture.data()+0x278,&resource,8);
            std::vector<uint8_t> context(0x109a4);uint32_t tracker_index=1;std::memcpy(context.data()+0x109a0,&tracker_index,4);
            const auto tracker=0xd8+0x50;uint32_t one=1;std::memcpy(texture.data()+tracker+0x10,&one,4);
            uint32_t global_state=0xc0;std::memcpy(texture.data()+tracker+0x18+0x10,&global_state,4);
            f.main();bool visited{};
            const auto visit=[&](const E::Snapshot& state,const E::Resource& binding) {
                visited=true;require(state.index==0 && binding.resource==resource && binding.state==0xc0 && binding.tracker_index==1,"read the exact owned resource and tracked graphics state");
                require(binding.lease.manager==reinterpret_cast<uintptr_t>(f.manager.data()) && binding.lease.pool==reinterpret_cast<uintptr_t>(&f.pool) &&
                    binding.lease.slots==f.pool.data && binding.lease.slot==f.pool.data && binding.lease.texture==texture_address && binding.lease.texture_index==0 &&
                    !binding.lease.pool_release_marked && !binding.lease.record_recycle && !binding.lease.record_deferred_release,"export exact current unmarked pool-slot ownership evidence");
            };
            require(f.experiment.visit_resource(reinterpret_cast<uintptr_t>(context.data()),visit) && visited,"owned single-view target is inspectable");
            const auto refused=[&] {visited=false;require(!f.experiment.visit_resource(reinterpret_cast<uintptr_t>(context.data()),visit) && !visited,"changed or release-marked pool ownership cannot reach resource visitor");};
            f.context_override=1;refused();f.context_override=0;
            f.manager_override=1;refused();f.manager_override=0;
            for(const auto offset:{0x3cu,0x3du}) {f.records[offset]=1;refused();f.records[offset]=0;}
            f.texture[0x10]=1;refused();f.texture[0x10]=0;
            int32_t different_index=1;std::memcpy(f.records.data()+0x40,&different_index,4);refused();different_index=0;std::memcpy(f.records.data()+0x40,&different_index,4);
            different_index=1;std::memcpy(f.records.data()+0x30,&different_index,4);refused();different_index=0;std::memcpy(f.records.data()+0x30,&different_index,4);
            const auto saved_pool=f.pool;
            for(const auto count:{0,1025}) {f.pool.count=count;refused();}f.pool=saved_pool;
            f.pool.capacity=0;refused();f.pool=saved_pool;
            f.pool.data=UINTPTR_MAX-4;refused();f.pool=saved_pool;
            const auto saved_slot=f.texture;f.texture[0]^=1;refused();f.texture=saved_slot;
            f.texture[8]^=1;refused();f.texture=saved_slot;
            const auto pool_address=reinterpret_cast<uintptr_t>(&f.pool);uintptr_t other_pool=1;
            std::memcpy(f.manager.data()+0x30,&other_pool,8);refused();std::memcpy(f.manager.data()+0x30,&pool_address,8);
            const auto saved_records=f.array;f.array.data=UINTPTR_MAX-4;refused();f.array=saved_records;
            f.array.capacity=0;refused();f.array=saved_records;
            visited=false;require(f.experiment.visit_resource(reinterpret_cast<uintptr_t>(context.data()),visit) && visited,"restored current pool slot is inspectable without weakening earlier refusals");
            std::array<uint8_t,56> external{};external[0]=external[0x30]=1;
            const auto external_address=reinterpret_cast<uintptr_t>(external.data());std::memcpy(texture.data()+tracker+8,&external_address,8);
            int32_t lane=0,depth=1;const auto stack=reinterpret_cast<uintptr_t>(&lane);
            std::memcpy(context.data()+0x38,&stack,8);std::memcpy(context.data()+0x40,&depth,4);
            std::memcpy(external.data()+0x28,&global_state,4);visited=false;
            require(f.experiment.visit_resource(reinterpret_cast<uintptr_t>(context.data()),visit) && visited,"parallel tracker uses graphics-lane state rather than stale global state");
            lane=1;require(!f.experiment.visit_resource(reinterpret_cast<uintptr_t>(context.data()),visit),"compute lane cannot masquerade as graphics at Present");lane=0;
            depth=0;std::memcpy(context.data()+0x40,&depth,4);require(!f.experiment.visit_resource(reinterpret_cast<uintptr_t>(context.data()),visit),"missing parallel-lane stack is rejected");depth=1;std::memcpy(context.data()+0x40,&depth,4);
            tracker_index=4;std::memcpy(context.data()+0x109a0,&tracker_index,4);visited=false;
            require(!f.experiment.visit_resource(reinterpret_cast<uintptr_t>(context.data()),visit) && !visited,"unknown tracker index is rejected");
            tracker_index=1;std::memcpy(context.data()+0x109a0,&tracker_index,4);f.array.count=2;
            require(!f.experiment.visit_resource(reinterpret_cast<uintptr_t>(context.data()),visit),"other additional views make resource identity ambiguous");
            f.array.count=1;f.main(2,false);require(!f.experiment.visit_resource(reinterpret_cast<uintptr_t>(context.data()),visit),"released target cannot be inspected via stale snapshot");
        }
        {Fixture f;f.arm();f.main();f.manager_override=1;f.main(2);
         require(f.experiment.snapshot().lost && f.releases==0 && f.creates==1,"manager handover cannot release or overwrite an old index through a new manager");}
        {Fixture f;f.arm();f.main();require(f.experiment.snapshot().texture!=0,"first accepted update records texture identity");f.pool.count=0;f.main(2);
         require(!f.experiment.snapshot().texture && !f.experiment.snapshot().pool && f.experiment.snapshot().texture_index==0 && !f.experiment.snapshot().allocated.width,
             "failed native pool observation cannot retain a previous update's cached allocation");}
        std::cout<<"native PIP experiment tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
