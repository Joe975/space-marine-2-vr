#include "synthetic_code.h"
#include "NativeTemporalProbe.h"
#include "game_build_ported.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>void put(uintptr_t address,size_t offset,const T& value){std::memcpy(reinterpret_cast<void*>(address+offset),&value,sizeof(value));}
using Probe=NativeTemporalProbe;
struct Fixture {
    std::vector<uint8_t> list=std::vector<uint8_t>(0x2a000),renderer=std::vector<uint8_t>(0x600);
    std::array<std::array<uint8_t,0x280>,4> wrappers{};Probe::Scope scope;std::array<uintptr_t,7> args{};
    Fixture(){using E=NativeCameraEntries;E::get().enable();const auto list_pointer=reinterpret_cast<uintptr_t>(list.data());const auto renderer_pointer=reinterpret_cast<uintptr_t>(renderer.data());
        auto left=vr_camera::IDENTITY,right=left;right[12]=.064f;const FrameSyncClock::Frame frame{23,19,0,0,false};auto ticket=E::get().begin(list_pointer+0x160,E::Role::main,frame);E::get().finish(*ticket,0,left);ticket=E::get().begin(list_pointer+0x160,E::Role::ordinary_right,frame);E::get().finish(*ticket,1,right);
        std::array<E::Entry,2> actual{{{list_pointer+0x168,0,E::Role::main,left},{list_pointer+0x168+0xa60,1,E::Role::main,right}}};const auto revision=*E::get().revision(list_pointer);scope.pair=*E::get().ordinary_pair(list_pointer,actual,revision);
        scope.scene.renderer=renderer_pointer;scope.scene.list=list_pointer;scope.scene.dispatch_sequence=57;scope.scene.render_counter_before=7;scope.scene.camera_revision_before=revision;scope.scene.refusal=NativeRenderProbe::SceneRefusal::none;
        for(size_t i=0;i<4;++i)put(reinterpret_cast<uintptr_t>(wrappers[i].data()),0x278,uintptr_t(0xf000+i));
        bind(0,0);
    }
    void bind(uint8_t mode,uint32_t slot,bool no_previous=false){
        scope.scene.slot=slot;scope.scene.ordinal=slot+1;scope.scene.entry=(slot?scope.pair.right:scope.pair.left).entry;scope.scene.view_flags_before=0x3f7c013;
        const auto renderer_pointer=scope.scene.renderer;put(scope.scene.list,0x29c78,scope.scene.render_counter_before);put(renderer_pointer,0x140,scope.scene.entry);put(scope.scene.entry,0,scope.scene.view_flags_before);
        args={renderer_pointer,reinterpret_cast<uintptr_t>(wrappers[0].data()),reinterpret_cast<uintptr_t>(wrappers[1].data()),no_previous?0:reinterpret_cast<uintptr_t>(wrappers[2].data()),renderer_pointer+0x10,reinterpret_cast<uintptr_t>(wrappers[3].data()),mode};
        if(mode<3){const std::array<size_t,3> previous{0x158,0x178,0x160},next{0x1b0,0x1d0,0x1b8};put(renderer_pointer,previous[mode],args[3]);put(renderer_pointer,next[mode],args[2]);}
        put(renderer_pointer,0x20,args[1]);put(renderer_pointer,0x28,args[3]);put(renderer_pointer,0x30,args[5]);put(renderer_pointer,0x78,uintptr_t(0));put(renderer_pointer,0x180,uintptr_t(0));
    }
};
Fixture* current{};unsigned calls{},mutation{};std::array<uintptr_t,7> seen{};bool provider_enabled{},provider_throws{},observer_throws{};
__declspec(noinline)void native(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uint8_t g){++calls;seen={a,b,c,d,e,f,g};
    if(mutation==1)put(current->scope.scene.list,0x29c78,uint32_t(8));
    if(mutation==2)put(a,0x158,uintptr_t(1));
    if(mutation==3)put(c,0x278,uintptr_t(0xf999));
    if(mutation==4)provider_enabled=false;
    if(mutation==5)throw std::runtime_error("native exception");
    if(mutation==6)put(a,0x20,uintptr_t(0));
    if(mutation==7)put(c,0x98,uint64_t(0x123456));
    volatile unsigned used=calls;(void)used;
}
__declspec(noinline)void invoke(Probe::Native volatile function,const std::array<uintptr_t,7>& args){function(args[0],args[1],args[2],args[3],args[4],args[5],uint8_t(args[6]));volatile unsigned used=calls;(void)used;}
int main(){try{
    // Call-site identity follows the running build (2026-10-21: every temporal call was refused
    // as "callsite" because the reference RVA was compared, so bloom stopped stereo on job one).
    for(uint8_t mode=0;mode<3;++mode)require(Probe::expected_callsite(mode,Probe::CALLS[mode]+5),"reference build: the reference return address");
    require(!Probe::expected_callsite(3,Probe::CALLS[0]+5) && !Probe::expected_callsite(0,Probe::CALLS[0]),"unknown mode or the call itself refused");
    game_build::override_active_for_testing(game_build_ported::EXE);
    for(uint8_t mode=0;mode<3;++mode){const auto ported=game_build::rva(Probe::CALLS[mode]);
        require(ported && ported!=Probe::CALLS[mode] && Probe::expected_callsite(mode,ported+5),"ported build: the translated return address");
        require(!Probe::expected_callsite(mode,Probe::CALLS[mode]+5),"ported build: the reference return address is refused");}
    game_build::override_active_for_testing(game_build_ported::EXE+1);
    require(!Probe::expected_callsite(0,Probe::CALLS[0]+5),"unknown build: nothing matches");
    game_build::override_active_for_testing(0);
    auto code=SYNTH(Probe::BYTES);auto sites=SYNTH_ALL(Probe::CALL_BYTES);require(Probe::matches(NativeRenderProbe::TIMESTAMP,code,sites),"exact shipping function and all three native call sites match");code[0]^=1;require(!Probe::matches(NativeRenderProbe::TIMESTAMP,code,sites),"changed temporal prologue refuses");code=SYNTH(Probe::BYTES);sites[1][1]^=1;require(!Probe::matches(NativeRenderProbe::TIMESTAMP,code,sites),"changed mode-specific caller refuses");require(!Probe::matches(0,SYNTH(Probe::BYTES),SYNTH_ALL(Probe::CALL_BYTES)) && !Probe::matches(NativeRenderProbe::TIMESTAMP,std::span<const uint8_t>{SYNTH(Probe::BYTES)}.first(3),SYNTH_ALL(Probe::CALL_BYTES)),"wrong build/short prologue refuse");
    Fixture fixture;current=&fixture;auto& probe=Probe::get();
    require(probe.install(reinterpret_cast<void*>(&native),[&]()->std::optional<Probe::Scope>{if(provider_throws)throw std::runtime_error("provider");return provider_enabled?std::optional{fixture.scope}:std::nullopt;},[&](uintptr_t resource){if(observer_throws)throw std::runtime_error("descriptor read");RenderTargetProbe::View v;v.resource=resource;v.identity=resource+100;v.handle=resource+200;v.width=1920;v.height=1080;v.flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;v.mips=v.depth_or_array=v.samples=v.planes=v.subresources=1;v.resource_dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;v.known=true;return RenderTargetProbe::Observation{1,4,{v}};},0,0,true),"actual seven-argument native detour installed");
    const auto draw=[&]{const auto before=calls;invoke(native,fixture.args);require(calls==before+1 && seen==fixture.args,"all seven arguments and exactly one original native call preserved");};
    draw();require(probe.sample(57).events.empty(),"unscoped AFR path produces no temporal attribution");provider_enabled=true;
    for(uint32_t slot=0;slot<2;++slot)for(uint8_t mode=0;mode<3;++mode){fixture.bind(mode,slot,slot==1);draw();const auto sample=probe.sample(57);const auto& event=sample.events.back();
        require(event.refusal==Probe::Refusal::none && event.before && event.before==event.after && event.mode==mode && event.scope.scene.slot==slot,"actual mode-specific previous/next bank matches both ordinary slots");
        require(event.outputs_before[1].resource==0xf001 && event.outputs_after[1].resource==0xf001 && event.outputs_before[1].descriptors.views[0].flags==D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS && event.outputs_before[1].descriptors.views[0].subresources==1,"actual borrowed wrapper POD joins copied descriptor flags/subresource metadata");
        require(event.before->inputs[0]==fixture.args[1] && event.outputs_before[4].resource==0xf000 && event.outputs_before[6].resource==0xf003 && event.outputs_before[8].status==NativeRenderProbe::OutputStatus::unavailable,"all five actual descriptor resource inputs are explicit, including null fallback");
        require(event.texture_contract_before && event.texture_contract_after && event.texture_contract_before==event.texture_contract_after && event.texture_contract_before->wrappers[1].address==fixture.args[2] && event.texture_contract_before->wrappers[4].address==fixture.args[1] && !event.texture_contract_before->complete,"actual detour captures all borrowed native wrapper arguments without inventing a missing factory contract");
        require(event.global_history_before && event.global_history_after && event.global_history_before->resource[0]==0xf000 && event.global_history_before->readable[0] && !event.global_history_before->readable[1] && !event.global_history_before->pool.complete && event.global_history_before->descriptors[0].views.empty(),"actual opt-in captures global current/previous POD without querying invalid fake COM addresses or inventing descriptor metadata");
        if(slot)require(event.arguments[3]==0 && event.outputs_before[2].status==NativeRenderProbe::OutputStatus::unavailable,"null first/cut predecessor remains explicit, without a false resource lease");}
    for(unsigned bad=0;bad<6;++bad){fixture.bind(0,0);const auto saved=fixture.scope;
        if(bad==0)fixture.scope.pair.right.revision++;
        if(bad==1)fixture.args[4]+=8;
        if(bad==2)fixture.args[3]+=8;
        if(bad==3)fixture.args[6]=255;
        if(bad==4)put(fixture.scope.scene.renderer,0x140,uintptr_t(1));
        if(bad==5)fixture.scope.scene.view_flags_before|=0x40;
        draw();require(probe.sample(57).events.back().refusal!=Probe::Refusal::none,"invalid scope, descriptors, bank, mode, active camera or PIP flags refuse attribution");fixture.scope=saved;}
    std::cerr<<"ABI/scoped positive and refusal controls passed\n";
    for(unsigned change=1;change<5;++change){fixture.bind(0,0);mutation=change;draw();provider_enabled=true;mutation=0;const auto event=probe.sample(57).events.back();require(event.refusal==(change==3?Probe::Refusal::resource_changed:Probe::Refusal::changed),"counter, previous resource, wrapper replacement or expired scope cannot certify unchanged temporal work");put(reinterpret_cast<uintptr_t>(fixture.wrappers[1].data()),0x278,uintptr_t(0xf001));}
    fixture.bind(0,0);provider_throws=true;draw();provider_throws=false;require(probe.sample(57).errors>0,"provider exception remains transparent and counted");
    observer_throws=true;draw();observer_throws=false;require(probe.sample(57).events.back().outputs_before[1].status==NativeRenderProbe::OutputStatus::unobserved,"descriptor observer failure does not create resource metadata or change the native call");
    std::cerr<<"Changed state and observer-exception controls passed\n";
    fixture.bind(0,0);mutation=6;draw();mutation=0;require(probe.sample(57).events.back().refusal==Probe::Refusal::changed,"actual descriptor resource replacement during native work refuses");fixture.bind(0,0);
    mutation=7;draw();mutation=0;{const auto event=probe.sample(57).events.back();require(event.refusal==Probe::Refusal::none && event.texture_contract_before!=event.texture_contract_after,"changed borrowed native flags remain explicit without changing native work or silently certifying its allocation contract");}put(fixture.args[2],0x98,uint64_t(0));
    mutation=5;const auto before=calls;bool propagated{};try{invoke(native,fixture.args);}catch(const std::runtime_error&){propagated=true;}mutation=0;require(propagated && calls==before+1,"native exception propagates without retry");
    std::cerr<<"Native exception control passed\n";
    for(unsigned i=0;i<140;++i)draw();const auto sample=probe.sample(57);require(sample.events.size()==128 && sample.dropped>0,"bounded ring preserves explicit lost evidence");
    std::ostringstream out;Probe::write(out,sample);require(out.str().find("\"native_gpu_histories_verified\":false")!=std::string::npos && out.str().find("\"resource_leases\":false")!=std::string::npos && out.str().find("\"subresources\":1")!=std::string::npos,"export owns POD metadata without promoting GPU history or lifetime");
    std::ostringstream global;Probe::write_global_history(global,sample);require(probe.observes_global_history() && global.str().find("\"ordinary_global_history_contract_v1\"")!=std::string::npos && global.str().find("\"writer_verified\":false")!=std::string::npos && out.str().find("global_history")==std::string::npos,"new global-history evidence has a separate explicit schema and never changes old temporal observation exports");
    require(!native_global_history::capture(0,UINTPTR_MAX,0,0).readable[0],"overflow wrapper input refuses without wrapping a native resource read");
    std::cout<<"native temporal probe: exact ABI, scope, bank/resource changes and bounded POD PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
