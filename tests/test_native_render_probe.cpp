#include "synthetic_code.h"
#include "NativeMemory.h"
#include "NativeRenderProbe.h"
#include "game_build_ported.h"
#include "NativeCameraTransfer.h"
#include "NativePipLens.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
std::vector<uint8_t> list(0x29c7c);
unsigned native_resets{},native_copies{};bool corrupt_copy{};
__declspec(noinline) void fake_reset(void* list) {++native_resets;const uint64_t zero{};std::memcpy(static_cast<uint8_t*>(list)+0x29808,&zero,4);std::memcpy(list,&zero,4);std::memcpy(static_cast<uint8_t*>(list)+0x29b10,&zero,8);}
__declspec(noinline) void fake_copy(void* a,void* b) {++native_copies;std::memcpy(a,b,NativeCameraTransfer::BYTES);if(corrupt_copy) static_cast<uint8_t*>(a)[16]^=1;}
__declspec(noinline) void invoke_copy(void(*volatile fn)(void*,void*),void* a,void* b) {fn(a,b);volatile auto used=native_copies;(void)used;}
__declspec(noinline) void invoke_reset(void(*volatile fn)(void*),void* list) {fn(list);volatile auto used=native_resets;(void)used;}
uintptr_t active_list{};bool enabled=true,replace_list{},renderer_scope_seen{},frame_scope_seen{};unsigned calls{};void* seen{};uint32_t seen_flags{};
std::function<void(void*,uint32_t)> dispatch_body;
std::function<void(void*,uint32_t,uint32_t)> scene_body;
unsigned scene_calls{};void* scene_renderer{};uint32_t scene_flags{},scene_slot{};
template<class T> void put(size_t offset,const T& value) {std::memcpy(list.data()+offset,&value,sizeof(value));}
template<class T> void put_renderer(void* renderer,size_t offset,const T& value) {std::memcpy(static_cast<uint8_t*>(renderer)+offset,&value,sizeof(value));}
__declspec(noinline) void fake_scene(void* renderer,uint32_t flags,int32_t slot) {
    ++scene_calls;scene_renderer=renderer;scene_flags=flags;scene_slot=slot;
    if(scene_body) scene_body(renderer,flags,slot);
    else {const uintptr_t entry=slot>=0 && slot<64?active_list+0x168+size_t(slot)*0xa60:0;put_renderer(renderer,0x140,entry);}
    volatile auto used=scene_calls;(void)used;
}
__declspec(noinline) void invoke_scene(void(*volatile fn)(void*,uint32_t,int32_t),void* renderer,uint32_t flags,int32_t slot) {fn(renderer,flags,slot);volatile auto used=scene_calls;(void)used;}
void unused_cache(void*,const float*,const float*,const float*) {}
void unused_frustum(void*) {}
__declspec(noinline) void fake_dispatch(void* renderer,uint32_t flags) {
    ++calls;seen=renderer;seen_flags=flags;
    renderer_scope_seen=NativeRenderProbe::current_renderer()==reinterpret_cast<uintptr_t>(renderer);
    const auto receipt=NativeRenderProbe::current_camera_entries();frame_scope_seen=receipt && receipt->render_id==9;
    if(dispatch_body) {dispatch_body(renderer,flags);return;}
    const float zero=0;put(0x168+0xa60+0x45c+8*4,zero);
    if(replace_list) active_list=0;
    volatile unsigned keep=calls; (void)keep;
}
__declspec(noinline) void invoke(void(*volatile fn)(void*,uint32_t),void* renderer,uint32_t flags) {fn(renderer,flags);++seen_flags;}
}
int main() {
    try {
        using E=NativeCameraEntries;E entries;entries.enable();
        auto stationary=vr_camera::IDENTITY;stationary[10]=-1;
        const uintptr_t render_list=0x1000,add_list=render_list+0x160;
        const FrameSyncClock::Frame frame{0,1,0,0,false};
        auto add=entries.begin(add_list,E::Role::main,frame);entries.finish(*add,0,stationary);
        add=entries.begin(add_list,E::Role::pip,frame);entries.finish(*add,1,stationary);
        std::array<E::Entry,2> actual{{{*E::address(add_list,0),0,E::Role::main,stationary},{*E::address(add_list,1),1,E::Role::pip,stationary}}};
        const auto revision=*entries.revision(render_list);const auto first=entries.pair(render_list,actual,revision);
        require(first && first->render_id==1 && first->token==0,"exact physical slots bind identical stationary poses and valid token zero");
        for(unsigned field=0;field<9;++field) {
            auto changed=*first;
            switch(field) {
                case 0:++changed.render_id;break;case 1:++changed.token;break;case 2:++changed.revision;break;
                case 3:++changed.render_list;break;case 4:++changed.add_list;break;case 5:++changed.main_entry;break;
                case 6:++changed.pip_entry;break;case 7:++changed.main_slot;break;case 8:++changed.pip_slot;break;
            }
            require(!entries.current(changed),"a matching revision cannot authenticate a changed frame/token/list/slot/address receipt");
        }
        const FrameSyncClock::Frame next{1,2,1,0,false};
        add=entries.begin(add_list,E::Role::main,next);
        require(!entries.current(*first) && !entries.revision(render_list),"producer entry invalidates an old dispatch before overwriting memory");
        entries.finish(*add,0,stationary);add=entries.begin(add_list,E::Role::pip,next);entries.finish(*add,1,stationary);
        require(!entries.pair(render_list,actual,revision),"same-pose overwrite between pre-read and post-read cannot relabel an older snapshot");
        const auto current=*entries.revision(render_list);const auto second=entries.pair(render_list,actual,current);
        require(second && second->render_id==2,"a fresh slot receipt identifies the next frame without diagnostic jitter");
        auto wrong=actual;wrong[1].address+=8;require(!entries.pair(render_list,wrong,current),"copied/rebased entries need actual pointer proof");
        wrong=actual;wrong[1].world[12]+=.1f;require(!entries.pair(render_list,wrong,current),"changed native world cannot inherit a slot receipt");
        add=entries.begin(add_list,E::Role::other,std::nullopt);entries.finish(*add,1,std::nullopt);
        require(!entries.pair(render_list,actual,*entries.revision(render_list)),"an unknown caller overwriting a PIP slot invalidates its receipt");
        const FrameSyncClock::Frame third{2,3,0,0,false};
        add=entries.begin(add_list,E::Role::main,third);entries.finish(*add,0,stationary);
        add=entries.begin(add_list,E::Role::pip,next);entries.finish(*add,1,stationary);
        require(!entries.pair(render_list,actual,*entries.revision(render_list)),"a PIP add from a different producing frame cannot pair with main");
        auto overlap=entries.begin(add_list,E::Role::other,std::nullopt);add=entries.begin(add_list,E::Role::pip,third);
        entries.finish(*overlap,2,std::nullopt);entries.finish(*add,1,stationary);
        require(!entries.revision(render_list),"overlapping producer callbacks refuse partially read native entries");
        const FrameSyncClock::Frame fourth{3,4,1,0,false};
        add=entries.begin(add_list,E::Role::main,fourth);entries.finish(*add,0,stationary);
        add=entries.begin(add_list,E::Role::pip,fourth);entries.finish(*add,1,stationary);
        auto exchange=entries.begin_copy(0x5000,add_list);
        require(exchange && !entries.revision(render_list),"a native list copy invalidates readers before copying bytes");
        require(entries.finish_copy(*exchange,true),"verified native byte copy transfers existing producing-frame content");
        auto moved=actual;for(auto& entry:moved) entry.address=*E::address(0x5000,entry.slot);
        const auto transferred=entries.pair(0x5000-0x160,moved,*entries.revision(0x5000-0x160));
        require(transferred && transferred->render_id==4 && entries.pair(render_list,actual,entries.revision(render_list).value_or(0)).has_value(),"actual destination and unchanged source retain the copied slot generation");
        exchange=entries.begin_copy(0x5000,add_list);require(!entries.finish_copy(*exchange,false),"a byte mismatch cannot transfer provenance");
        require(!entries.revision(render_list) && !entries.revision(0x5000-0x160),"failed copy leaves both lists unaccepted");
        require(!E::address(UINTPTR_MAX-4,0) && !E::address(add_list,64),"native slot arithmetic is bounded");
        using P=NativeRenderProbe;require(P::matches(P::TIMESTAMP,SYNTH(P::PROLOGUE)),"known dispatch build accepted");
        std::array<uint8_t,0x280> wrapper{};uintptr_t candidate=0x1234;
        std::memcpy(wrapper.data()+0x278,&candidate,sizeof(candidate));const auto wrapper_address=reinterpret_cast<uintptr_t>(wrapper.data());
        unsigned output_reads{};
        P::OutputObserver output_reader=[&](uintptr_t resource){++output_reads;require(resource==candidate,"raw resource observed without COM dereference");
            RenderTargetProbe::View view;view.resource=resource;view.identity=0x5678;
            return RenderTargetProbe::Observation{1,42,{view}};};
        require(P::read_output(wrapper_address,{}).status==P::OutputStatus::unavailable && !output_reads,"missing output observer never dereferences wrapper");
        require(P::read_output(0x1234,output_reader).status==P::OutputStatus::unreadable && !output_reads &&
                P::read_output(UINTPTR_MAX-4,output_reader).status==P::OutputStatus::unreadable,"unreadable and overflow wrapper pointers never query COM");
        auto output_receipt=P::read_output(wrapper_address,output_reader);
        require(output_receipt.status==P::OutputStatus::observed && output_receipt.resource==candidate && output_receipt.descriptors.serial==42 &&
                output_receipt.descriptors.views[0].identity==0x5678,"actual wrapper resource maps to a copied descriptor observation");
        require(P::read_output(wrapper_address,[](uintptr_t){return RenderTargetProbe::Observation{};}).status==P::OutputStatus::unobserved,
                "an unknown resource pointer cannot become a descriptor observation");
        require(P::read_output(wrapper_address,[](uintptr_t)->RenderTargetProbe::Observation{throw std::runtime_error("reader");}).status==P::OutputStatus::unobserved,
                "descriptor provider failure cannot escape observation");
        require(P::read_output(wrapper_address,[](uintptr_t){RenderTargetProbe::Observation value;value.views.resize(9);return value;}).status==P::OutputStatus::overflow,
                "output descriptor evidence is bounded and never silently truncated");
        output_receipt=P::read_output(wrapper_address,[&](uintptr_t resource){const uintptr_t changed=resource+1;std::memcpy(wrapper.data()+0x278,&changed,sizeof(changed));return RenderTargetProbe::Observation{};});
        require(output_receipt.status==P::OutputStatus::changed,"wrapper replacement while collecting descriptor evidence refuses attribution");
        std::memcpy(wrapper.data()+0x278,&candidate,sizeof(candidate));
        auto bytes=SYNTH(P::PROLOGUE);bytes[2]^=1;require(!P::matches(P::TIMESTAMP,bytes) && !P::matches(0,SYNTH(P::PROLOGUE)),"unknown native dispatch bytes/timestamp rejected");
        require(P::matches(game_build_ported::EXE,game_build_ported::code(P::DISPATCH_RVA)) && P::matches_scene(game_build_ported::EXE,game_build_ported::code(P::SCENE_RVA)),"the 2026-10-21 dispatch and scene functions match");
        active_list=reinterpret_cast<uintptr_t>(list.data());const int32_t count=2;put(0x29968,count);
        auto world=vr_camera::IDENTITY;world[10]=-1;world[12]=10;const auto view=vr_camera::inverse(world);
        auto projection=vr_camera::IDENTITY;projection[11]=1;projection[15]=0;
        for(int i=0;i<2;++i) {
            const size_t entry=0x168+size_t(i)*0xa60;const uint64_t flags=i?0xe50043:0x3f7c013;
            put(entry,flags);put(entry+0x34,world);put(entry+0x3dc,view);put(entry+0x45c,projection);
            const std::array<float,4> rectangle{-.1f,-.1f,.2f,.2f};put(entry+0x160,rectangle);
            const float fov=90,aspect=1;put(entry+0x304,fov);put(entry+0x308,fov);put(entry+0x30c,aspect);const int32_t record=0;put(entry+0xa50,record);
        }
        const float shift=-.2f;put(0x168+0xa60+0x45c+8*4,shift);
        auto& actual_entries=E::get();actual_entries.enable();const FrameSyncClock::Frame actual_frame{7,9,0,0,false};
        auto ticket=actual_entries.begin(active_list+0x160,E::Role::main,actual_frame);actual_entries.finish(*ticket,0,world);
        ticket=actual_entries.begin(active_list+0x160,E::Role::pip,actual_frame);actual_entries.finish(*ticket,1,world);
        auto& probe=P::get();unsigned completed{};uint64_t completed_dispatch{};
        require(probe.attach_completion([&](const P::Snapshot& value){++completed;completed_dispatch=value.sequence;require(value.end>=value.begin && !value.after.empty(),"completion sees the actual post-dispatch snapshot");}),"post-camera transaction completion callback attaches once");
        require(!probe.attach_completion([](const auto&){}),"later callbacks cannot replace accepted completion ownership");
        require(probe.install(reinterpret_cast<void*>(&fake_dispatch),[]{return active_list;},[]{if(!enabled) throw std::runtime_error("disabled provider");return true;},output_reader),"install actual native detour fixture");
        invoke(&fake_dispatch,reinterpret_cast<void*>(0x1234),0x58);const auto sample=probe.latest();
        require(completed==1 && completed_dispatch==sample->sequence,"actual original dispatch completes before the callback and snapshot publication");
        require(calls==1 && seen==reinterpret_cast<void*>(0x1234) && seen_flags==0x59,"original runs exactly once with unchanged arguments");
        require(renderer_scope_seen && P::current_renderer()==0,"renderer identity is available only inside the actual native dispatch");
        require(frame_scope_seen && !P::current_camera_entries(),"causal camera receipts are scoped to the actual dispatch and cleared afterward");
        require(sample && sample->before.size()==2 && sample->after.size()==2 && sample->end>=sample->begin,"capture bounded before/after view caches");
        require(sample->camera_entries && sample->camera_entries->render_id==9 && sample->camera_entries->token==7 && sample->camera_entries->pip_slot==1,
                "actual dispatcher detour publishes unchanged physical slot receipts with its observed views");
        require(sample->before[0].main() && !sample->before[0].pip() && sample->after[1].pip() && sample->after[1].record==0,"separate main and PIP slots and record index");
        require(sample->before[1].projection[8]==-.2f && sample->after[1].projection[8]==0 && sample->after[0].world[12]==10,"actual native projection rebuild supersedes the at-add projection");
        auto& transfer=NativeCameraTransfer::get();
        require(NativeCameraTransfer::matches(NativeCameraTransfer::TIMESTAMP,SYNTH(NativeCameraTransfer::RESET_BYTES),SYNTH(NativeCameraTransfer::COPY_BYTES)),"known native list reset/exchange bytes accepted");
        auto changed_bytes=SYNTH(NativeCameraTransfer::COPY_BYTES);changed_bytes[3]^=1;
        require(!NativeCameraTransfer::matches(NativeCameraTransfer::TIMESTAMP,SYNTH(NativeCameraTransfer::RESET_BYTES),changed_bytes),"changed native transfer implementation refuses installation");
        require(NativeCameraTransfer::matches(game_build_ported::EXE,game_build_ported::code(NativeCameraTransfer::RESET_RVA),game_build_ported::code(NativeCameraTransfer::COPY_RVA)),"the 2026-10-21 list reset/exchange match");
        require(transfer.install(reinterpret_cast<void*>(&fake_reset),reinterpret_cast<void*>(&fake_copy)),"actual reset/exchange detours install in native fixture");
        std::vector<uint8_t> destination(NativeCameraTransfer::BYTES);
        const auto source_add=active_list+0x160;const auto target_add=reinterpret_cast<uintptr_t>(destination.data());
        invoke_copy(&fake_copy,destination.data(),reinterpret_cast<void*>(source_add));
        std::array<E::Entry,2> moved_actual{{{*E::address(target_add,0),0,E::Role::main,world},{*E::address(target_add,1),1,E::Role::pip,world}}};
        const auto actual_moved=actual_entries.pair(target_add-0x160,moved_actual,*actual_entries.revision(target_add-0x160));
        require(actual_moved && actual_moved->render_id==9 && native_copies==1 && transfer.status().verified==1,"real detoured native byte copy preserves the producing frame at the exact destination");
        invoke_copy(&fake_copy,reinterpret_cast<void*>(source_add),destination.data());
        corrupt_copy=true;invoke_copy(&fake_copy,destination.data(),reinterpret_cast<void*>(source_add));
        require(native_copies==3 && transfer.status().refused==1 && !actual_entries.revision(target_add-0x160),"real corrupted copy bytes refuse provenance while original still runs exactly once");
        corrupt_copy=false;invoke_copy(&fake_copy,reinterpret_cast<void*>(source_add),destination.data());
        invoke_reset(&fake_reset,reinterpret_cast<void*>(source_add));
        require(native_resets==1 && transfer.status().resets==1 && !actual_entries.revision(active_list),"actual native reset invalidates any previous list generation before forwarding");
        replace_list=true;invoke(&fake_dispatch,nullptr,0);require(probe.latest()->sequence==sample->sequence,"scene/list changes cannot publish a mixed dispatch");
        enabled=false;invoke(&fake_dispatch,nullptr,1);require(calls==3 && probe.latest()->sequence==sample->sequence,"diagnostic/provider exceptions cannot suppress original engine work");
        const int32_t overflow=65;put(0x29968,overflow);require(!P::read_views(reinterpret_cast<uintptr_t>(list.data())),"bounded camera count");put(0x29968,count);
        require(!P::read_views(UINTPTR_MAX-10),"wrapped native list address rejected");
        const float nan=std::numeric_limits<float>::quiet_NaN();put(0x168+0x160,nan);
        require(!P::read_views(reinterpret_cast<uintptr_t>(list.data())),"non-finite rectangle cannot corrupt JSON evidence");const float left=-.1f;put(0x168+0x160,left);
        put(0x168+0x45c,nan);require(!P::read_views(reinterpret_cast<uintptr_t>(list.data())),"invalid final matrices cannot become evidence");
        put(0x168+0x45c,projection);
        {const auto calls=native_memory::calls().load();require(P::read_views(reinterpret_cast<uintptr_t>(list.data())).has_value(),"fixture views read");
         const auto used=native_memory::calls().load()-calls;std::cout<<"read_views: "<<used<<" native reads for "<<count<<" views\n";
         require(used<=1+3*uint64_t(count),"one read per view entry (plus its camera packet), not per field");}
        require(P::matches_scene(P::TIMESTAMP,SYNTH(P::SCENE_PROLOGUE)),"actual three-argument scene entry accepted");
        auto bad_scene_bytes=SYNTH(P::SCENE_PROLOGUE);bad_scene_bytes[0]^=1;
        require(!P::matches_scene(P::TIMESTAMP,bad_scene_bytes) && !P::matches_scene(0,SYNTH(P::SCENE_PROLOGUE)),"unknown scene ABI/build refused");
        require(!probe.scene_installed(),"dispatcher observation does not claim an installed scene detour");
        std::array<uint8_t,0x300> renderer{},nested_renderer{};
        require(!P::read_history_bank(0,0) && !P::read_history_bank(UINTPTR_MAX-8,1) && !P::read_history_bank(reinterpret_cast<uintptr_t>(renderer.data()),2),"persistent ordinary bank refuses null/overflow and non-ordinary slots before reading");
        const std::array<uintptr_t,11> bank_left{10,11,12,13,14,15,16,17,18,19,20},bank_right{20,21,22,23,24,25,26,27,28,29,30};
        put_renderer(renderer.data(),0x200,bank_left);put_renderer(renderer.data(),0x258,bank_right);
        const auto native_bytes=renderer;
        require(P::read_history_bank(reinterpret_cast<uintptr_t>(renderer.data()),0)->words==bank_left && P::read_history_bank(reinterpret_cast<uintptr_t>(renderer.data()),1)->words==bank_right && renderer==native_bytes,"bounded persistent bank copies distinct per-slot POD without mutating native memory");
        std::optional<uintptr_t> prepared_output;
        auto& lens=NativePipLens::get();unsigned lens_provider_calls{};
        require(lens.install(reinterpret_cast<void*>(&fake_scene),&unused_cache,&unused_frustum,[]{return active_list;},
            [&]() -> std::optional<NativePipLens::Target> {++lens_provider_calls;if(prepared_output) put_renderer(renderer.data(),0x100,*prepared_output);return std::nullopt;}) && probe.scene_installed(),
            "existing lens owns the sole actual detour and attaches scene observation without a hook collision");
        require(!probe.attach_scene_hook(reinterpret_cast<void*>(&fake_scene)),"scene observer cannot attach twice");
        const uintptr_t output=0xabc;put_renderer(renderer.data(),0x100,output);put_renderer(nested_renderer.data(),0x100,output);
        enabled=true;replace_list=false;active_list=reinterpret_cast<uintptr_t>(list.data());
        const uint32_t render_counter=42;put(0x29c78,render_counter);
        const std::array<uintptr_t,11> temporal_previous{1,2,3,4,5,6,7,8,9,10,11},temporal_next{11,10,9,8,7,6,5,4,3,2,1};
        put_renderer(renderer.data(),0x150,temporal_previous);put_renderer(renderer.data(),0x1a8,temporal_next);
        dispatch_body=[](void* renderer,uint32_t flags) {invoke_scene(&fake_scene,renderer,flags,0);invoke_scene(&fake_scene,renderer,flags|0x10,1);};
        auto before_calls=calls,before_scenes=scene_calls;
        invoke(&fake_dispatch,renderer.data(),0x123);
        auto observed=*probe.latest();
        require(lens_provider_calls==1,"lens preparation still runs exactly once on the PIP scene before observation");
        require(calls==before_calls+1 && scene_calls==before_scenes+2 && scene_renderer==renderer.data() && scene_flags==0x133 && scene_slot==1,
            "three native scene arguments forwarded unchanged and each original runs once");
        require(observed.scene_observer && observed.scene_calls==2 && observed.scenes.size()==2 && !observed.scene_refused && !observed.scene_dropped,
            "actual ordinary/PIP scene calls bounded inside one dispatcher receipt");
        for(size_t i=0;i<2;++i) {
            const auto& scene=observed.scenes[i];
            require(scene.temporal_before && scene.temporal_after && scene.temporal_before->previous==temporal_previous && scene.temporal_after->next==temporal_next,"exact opaque temporal resource words are observed without dereferencing cached resources");
            require(scene.valid() && scene.slot==i && scene.ordinal==i+1 && scene.dispatch_sequence==observed.sequence &&
                scene.entry==active_list+0x168+i*0xa60 && scene.active_entry_after==scene.entry && scene.output_before==output && scene.output_after==output &&
                scene.render_counter_before==42 && scene.render_counter_after==42,"scene entry/output/counter are observed on the original call");
        }
        before_scenes=scene_calls;invoke_scene(&fake_scene,renderer.data(),0x55,0);
        require(scene_calls==before_scenes+1 && probe.latest()->sequence==observed.sequence && probe.latest()->scene_calls==2,
            "out-of-dispatch scene forwards once without contaminating the last receipt");
        enabled=false;before_scenes=scene_calls;invoke(&fake_dispatch,renderer.data(),7);
        require(scene_calls==before_scenes+2 && probe.latest()->sequence==observed.sequence,"disabled/provider exception forwards all scenes without publishing");enabled=true;
        prepared_output=0xdef;
        dispatch_body=[](void* renderer,uint32_t flags){invoke_scene(&fake_scene,renderer,flags,1);};
        invoke(&fake_dispatch,renderer.data(),0);
        require(!probe.latest()->scene_refused && probe.latest()->scenes[0].output_before==*prepared_output,
            "scene observation begins after the existing lens preparation rather than bypassing or wrapping ahead of it");
        prepared_output.reset();put_renderer(renderer.data(),0x100,output);
        dispatch_body=[&](void*,uint32_t flags){invoke_scene(&fake_scene,nested_renderer.data(),flags,0);};
        invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->scene_calls==1 && probe.latest()->scene_refused==1 && probe.latest()->scenes[0].refusal==P::SceneRefusal::renderer,
            "a foreign renderer inside the dispatch is counted and refused rather than silently omitted");
        dispatch_body=[](void* renderer,uint32_t flags){invoke_scene(&fake_scene,renderer,flags,-1);};
        invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->scene_calls==1 && probe.latest()->scene_refused==1 && probe.latest()->scenes[0].refusal==P::SceneRefusal::slot,
            "out-of-range slot refuses evidence while forwarding the native call");
        dispatch_body=[](void* renderer,uint32_t flags){invoke_scene(&fake_scene,renderer,flags,0);};
        scene_body=[](void* renderer,uint32_t,uint32_t){const uintptr_t wrong=0x1234;put_renderer(renderer,0x140,wrong);};
        invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->scene_refused==1 && probe.latest()->scenes[0].refusal==P::SceneRefusal::entry,"foreign active entry cannot authenticate a scene");
        scene_body=[](void* renderer,uint32_t,uint32_t slot){const auto entry=active_list+0x168+size_t(slot)*0xa60;put_renderer(renderer,0x140,entry);const uint32_t next=43;put(0x29c78,next);};
        invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->scenes[0].refusal==P::SceneRefusal::render_counter,"render counter advance during the scene refuses snapshot evidence");put(0x29c78,render_counter);
        scene_body=[](void* renderer,uint32_t,uint32_t slot){const auto entry=active_list+0x168+size_t(slot)*0xa60;put_renderer(renderer,0x140,entry);const float changed=10.25f;put(0x168+0x34+12*4,changed);};
        invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->scenes[0].refusal==P::SceneRefusal::view,"changed scene world cannot inherit the dispatcher snapshot");put(0x168+0x34,world);
        scene_body=[](void*,uint32_t,uint32_t){};
        invoke(&fake_dispatch,reinterpret_cast<void*>(0x1234),0);
        require(probe.latest()->scene_refused==1 && probe.latest()->scenes[0].refusal==P::SceneRefusal::unreadable,"unreadable renderer refuses without dereferencing it");
        scene_body={};
        dispatch_body=[](void* renderer,uint32_t flags){for(size_t i=0;i<P::MAX_SCENES+1;++i) invoke_scene(&fake_scene,renderer,flags,0);};
        before_scenes=scene_calls;invoke(&fake_dispatch,renderer.data(),0);
        require(scene_calls==before_scenes+P::MAX_SCENES+1 && probe.latest()->scene_calls==P::MAX_SCENES+1 && probe.latest()->scenes.size()==P::MAX_SCENES &&
                probe.latest()->scene_dropped==1 && !probe.latest()->scene_refused,"receipt cap never suppresses or repeats original scene work");
        unsigned depth{};std::optional<P::Snapshot> nested;
        dispatch_body=[&](void* current,uint32_t flags){
            invoke_scene(&fake_scene,current,flags,0);
            if(!depth) {++depth;invoke(&fake_dispatch,nested_renderer.data(),flags+1);nested=probe.latest();--depth;
                require(P::current_renderer()==reinterpret_cast<uintptr_t>(current),"nested dispatch restores actual parent scope");
                invoke_scene(&fake_scene,current,flags,1);}
        };
        invoke(&fake_dispatch,renderer.data(),0x44);observed=*probe.latest();
        require(nested && nested->renderer==reinterpret_cast<uintptr_t>(nested_renderer.data()) && nested->scene_calls==1 && observed.scene_calls==2 &&
                nested->sequence!=observed.sequence && observed.scenes[1].dispatch_sequence==observed.sequence && !observed.scene_refused && !nested->scene_refused,
                "nested dispatch scene receipts remain separate and parent scope resumes");
        dispatch_body=[](void* renderer,uint32_t flags){invoke_scene(&fake_scene,renderer,flags,0);};
        scene_body=[&](void* current,uint32_t flags,uint32_t slot){if(!depth){++depth;invoke_scene(&fake_scene,current,flags+1,1);--depth;}
            const auto entry=active_list+0x168+size_t(slot)*0xa60;put_renderer(current,0x140,entry);};
        invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->scene_calls==2 && !probe.latest()->scene_refused && probe.latest()->scenes[0].ordinal==1 && probe.latest()->scenes[1].ordinal==2,
            "nested scene calls preserve entry order without invalidating parent receipt storage");
        scene_body={};
        put_renderer(renderer.data(),0x100,wrapper_address);
        dispatch_body=[](void* current,uint32_t flags){invoke_scene(&fake_scene,current,flags,0);};
        invoke(&fake_dispatch,renderer.data(),0);
        require(!probe.latest()->scene_refused && probe.latest()->scenes[0].resource_before.resource==candidate &&
                probe.latest()->scenes[0].resource_after.descriptors.views[0].identity==0x5678,
                "actual shared scene detour records resource mapping on both sides of the native draw");
        put_renderer(renderer.data(),0x100,output);
        ticket=actual_entries.begin(active_list+0x160,E::Role::main,actual_frame);actual_entries.finish(*ticket,0,world);
        ticket=actual_entries.begin(active_list+0x160,E::Role::pip,actual_frame);actual_entries.finish(*ticket,1,world);
        scene_body=[](void* current,uint32_t,uint32_t slot){const auto entry=active_list+0x168+size_t(slot)*0xa60;put_renderer(current,0x140,entry);E::get().reset(active_list+0x160);};
        invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->camera_revision && probe.latest()->scenes[0].refusal==P::SceneRefusal::revision && !probe.latest()->camera_entries,
            "producer revision invalidation refuses scene identity and removes stale pair provenance");
        const int32_t ordinary_count=1;put(0x29968,ordinary_count);const FrameSyncClock::Frame ordinary_frame{8,10,1,0,false};
        ticket=actual_entries.begin(active_list+0x160,E::Role::main,ordinary_frame);actual_entries.finish(*ticket,0,world);
        scene_body=[](void* current,uint32_t,uint32_t slot){const auto entry=active_list+0x168+size_t(slot)*0xa60;put_renderer(current,0x140,entry);
            require(P::current_scene() && P::current_scene()->entry==entry && P::current_primary() && P::current_primary()->render_id==10,
                "actual ordinary original sees its own scene and exact primary producing-frame scope");};
        invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->primary && probe.latest()->primary->render_id==10 && !probe.latest()->camera_entries &&
                !P::current_primary() && !P::current_scene(),"ordinary-only dispatch exports primary provenance without a PIP pair or escaped TLS state");
        put(0x29968,count);
        const FrameSyncClock::Frame stereo_frame{9,11,0,0,false};auto right_world=world;right_world[12]+=.064f;const uint64_t ordinary_flags=0;
        put(0x168+0xa60,ordinary_flags);put(0x168+0xa60+0x34,right_world);
        ticket=actual_entries.begin(active_list+0x160,E::Role::main,stereo_frame);actual_entries.finish(*ticket,0,world);
        ticket=actual_entries.begin(active_list+0x160,E::Role::ordinary_right,stereo_frame);actual_entries.finish(*ticket,1,right_world);
        scene_body=[](void* current,uint32_t,uint32_t slot){const auto eye=P::current_ordinary_pair();require(eye && !P::current_primary() && !P::current_camera_entries() && P::current_scene()->slot==slot,"actual Scene sees only its separate current ordinary pair");const auto entry=active_list+0x168+size_t(slot)*0xa60;put_renderer(current,0x140,entry);};dispatch_body=[](void* current,uint32_t flags){invoke_scene(&fake_scene,current,flags,0);invoke_scene(&fake_scene,current,flags,1);};
        before_calls=calls;before_scenes=scene_calls;invoke(&fake_dispatch,renderer.data(),0);
        require(calls==before_calls+1 && scene_calls==before_scenes+2 && probe.latest()->ordinary_pair && probe.latest()->ordinary_pair->left.render_id==11 && !probe.latest()->scene_refused && !probe.latest()->primary && !probe.latest()->camera_entries,
                "one actual native dispatcher with two ordinary Scenes exports only the separate ordinary producing-job contract");
        require(probe.latest()->scenes[0].packet_before && probe.latest()->scenes[0].packet_after &&
                *probe.latest()->scenes[0].packet_before==*probe.latest()->scenes[0].packet_after &&
                probe.latest()->scenes[1].packet_before->entry==active_list+0x168+0xa60,
                "actual ordinary Scene boundaries retain separate stable owned native packets");
        const auto left_next=temporal_next;auto right_next=temporal_next;right_next[2]=0xabc000;
        scene_body=[](void* current,uint32_t,uint32_t slot){const auto entry=active_list+0x168+size_t(slot)*0xa60;put_renderer(current,0x140,entry);};
        dispatch_body=[&](void* current,uint32_t flags){
            // Simulate the shipping dispatcher's load -> Scene -> store order.
            std::array<uintptr_t,11> previous{};
            std::memcpy(previous.data(),static_cast<uint8_t*>(current)+0x200,sizeof(previous));put_renderer(current,0x150,previous);put_renderer(current,0x1a8,left_next);
            invoke_scene(&fake_scene,current,flags,0);put_renderer(current,0x200,left_next);
            std::memcpy(previous.data(),static_cast<uint8_t*>(current)+0x258,sizeof(previous));put_renderer(current,0x150,previous);put_renderer(current,0x1a8,right_next);
            invoke_scene(&fake_scene,current,flags,1);put_renderer(current,0x258,right_next);
        };
        before_calls=calls;before_scenes=scene_calls;invoke(&fake_dispatch,renderer.data(),0);observed=*probe.latest();
        require(calls==before_calls+1 && scene_calls==before_scenes+2 && !observed.scene_refused,"persistent-bank observation forwards one actual dispatch and both native Scenes");
        require(observed.scenes[0].persistent_before->words==bank_left && observed.scenes[1].persistent_before->words==bank_right && observed.scenes[0].temporal_before->previous==bank_left && observed.scenes[1].temporal_before->previous==bank_right,"persistent before words join each actual Scene's loaded previous bank");
        require(observed.scenes[0].persistent_after->words==left_next && observed.scenes[1].persistent_after->words==right_next && observed.scenes[1].persistent_after->address==reinterpret_cast<uintptr_t>(renderer.data())+0x258,"after words are captured after native dispatcher export, separately for both slots");
        invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->scenes[0].persistent_before->words==left_next && probe.latest()->scenes[1].persistent_before->words==right_next,"actual following dispatch reads the same eye's persisted output rather than another slot's bank");
        dispatch_body=[](void* current,uint32_t flags){invoke_scene(&fake_scene,current,flags,0);invoke_scene(&fake_scene,current,flags,1);};
        scene_body=[](void* current,uint32_t,uint32_t slot){const auto entry=active_list+0x168+size_t(slot)*0xa60;put_renderer(current,0x140,entry);put(0x168+size_t(slot)*0xa60+0x9b4,uint32_t(0x3f800000+slot));};
        before_calls=calls;before_scenes=scene_calls;invoke(&fake_dispatch,renderer.data(),0);
        require(calls==before_calls+1 && scene_calls==before_scenes+2 && !probe.latest()->scene_refused &&
                probe.latest()->scenes[1].packet_before && probe.latest()->scenes[1].packet_after &&
                probe.latest()->scenes[1].packet_before!=probe.latest()->scenes[1].packet_after,
                "native motion packet mutation remains visible across the one actual Scene call without altering provenance gates");
        const auto preceding_sequence=probe.latest()->sequence;
        scene_body=[](void*,uint32_t,uint32_t){active_list=0;};invoke(&fake_dispatch,renderer.data(),0);
        require(probe.latest()->sequence==preceding_sequence,"list replacement during scene never publishes a mixed dispatch");
        require(P::current_renderer()==0 && !P::current_camera_entries(),"all nested scene scopes clear after returning to caller");
        std::cout<<"native render probe tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
