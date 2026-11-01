#include "NativeOrdinaryPairColour.h"
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>

void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class T>void put(uintptr_t address,size_t offset,const T& value){std::memcpy(reinterpret_cast<void*>(address+offset),&value,sizeof(value));}
using Pair=NativeOrdinaryPairColour;using Probe=NativeOrdinaryProbe;using Entries=NativeCameraEntries;
struct Fixture {
    std::vector<uint8_t> list=std::vector<uint8_t>(0x2a000),renderer=std::vector<uint8_t>(0x600);
    std::array<std::array<uint8_t,0x280>,2> wrappers{};Pair::Scope scope;
    bool scoped{true},provider_error{},observer_error{},post_error{},compositor_error{};unsigned posts{},compositors{},mutation{};uint32_t native_slot{};uintptr_t output{};
    std::array<uintptr_t,10> seen_post{};float delta{};std::array<uintptr_t,3> seen_compositor{};
    uintptr_t lp()const{return reinterpret_cast<uintptr_t>(list.data());}uintptr_t rp()const{return reinterpret_cast<uintptr_t>(renderer.data());}
    uintptr_t wrapper()const{return reinterpret_cast<uintptr_t>(wrappers[native_slot].data());}
    Fixture(){auto& entries=Entries::get();entries.enable();auto left=vr_camera::IDENTITY,right=left;right[12]=.064f;FrameSyncClock::Frame frame{0,81,0,0,false};
        auto ticket=entries.begin(lp()+0x160,Entries::Role::main,frame);entries.finish(*ticket,0,left);ticket=entries.begin(lp()+0x160,Entries::Role::ordinary_right,frame);entries.finish(*ticket,1,right);
        const auto revision=*entries.revision(lp());const std::array<Entries::Entry,2> actual{{{lp()+0x168,0,Entries::Role::main,left},{lp()+0x168+0xa60,1,Entries::Role::main,right}}};
        scope.pair=*entries.ordinary_pair(lp(),actual,revision);require(!entries.current(scope.pair.left) && !entries.primary(lp(),actual,revision),"ordinary pair never passes sole-primary identity");
        auto& s=scope.scene;s.renderer=rp();s.list=lp();s.dispatch_sequence=77;s.render_counter_before=42;s.camera_revision_before=revision;s.view_flags_before=0x3f7c013;s.refusal=NativeRenderProbe::SceneRefusal::none;
        s.resource_before.descriptors.epoch=1;s.resource_before.descriptors.serial=10;
        for(size_t i=0;i<2;++i){put(reinterpret_cast<uintptr_t>(wrappers[i].data()),0x278,uintptr_t(0xf000+i));put(lp()+0x168+i*0xa60,0,s.view_flags_before);}
        bind(0);inventory(8);
    }
    void inventory(int32_t count){put(lp(),0x29968,count);for(int32_t i=2;i<count && i<64;++i)put(lp()+0x168+size_t(i)*0xa60,0,uint64_t(0x10000000));}
    void bind(uint32_t slot){native_slot=slot;auto& s=scope.scene;s.slot=slot;s.ordinal=slot+1;s.entry=slot?scope.pair.right.entry:scope.pair.left.entry;put(rp(),0x140,s.entry);put(lp(),0x29c78,s.render_counter_before);}
    RenderTargetProbe::Observation observe(uintptr_t resource){if(observer_error)throw std::runtime_error("observer");
        if(mutation==1)put(lp(),0x29c78,uint32_t(43));
        if(mutation==2)scope.pair.producer_revision++;
        if(mutation==3)put(wrapper(),0x278,uintptr_t(0xf999));
        if(mutation==4)scoped=false;
        RenderTargetProbe::View v;v.resource=resource;v.identity=resource+8;v.handle=resource+16;v.known=true;v.created=2;v.bound=15;v.command_list=0xaaaa;
        v.width=3840;v.height=2160;v.storage=v.format=DXGI_FORMAT_R8G8B8A8_UNORM;v.dimension=D3D12_RTV_DIMENSION_TEXTURE2D;v.resource_dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;v.flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        v.depth_or_array=v.mips=v.samples=v.planes=v.subresources=1;return {1,20,{v}};
    }
};
Fixture* current{};
__declspec(noinline)void post(uintptr_t a,uintptr_t* b,uintptr_t c,uintptr_t d,uintptr_t entry,uint32_t f,uintptr_t g,uintptr_t h,uintptr_t i,uintptr_t j,float delta){
    auto& w=*current;++w.posts;w.seen_post={a,reinterpret_cast<uintptr_t>(b),c,d,entry,f,g,h,i,j};w.delta=delta;if(w.post_error)throw std::runtime_error("native post");*b=w.wrapper();volatile auto used=w.posts;(void)used;
}
__declspec(noinline)void compositor(uintptr_t a,uintptr_t b,uint8_t c){auto& w=*current;++w.compositors;w.seen_compositor={a,b,c};if(w.compositor_error)throw std::runtime_error("native compositor");volatile auto used=w.compositors;(void)used;}
__declspec(noinline)void invoke_post(Probe::Post volatile fn,uintptr_t entry){fn(1,&current->output,3,4,entry,0xf1234567,7,8,9,10,-.125f);volatile auto used=current->posts;(void)used;}
__declspec(noinline)void invoke_compositor(Probe::Compositor volatile fn,uintptr_t state){fn(current->wrapper(),state,0xfd);volatile auto used=current->compositors;(void)used;}
int main(int argc,char** argv){try{
    Fixture fixture;current=&fixture;auto& pair=Pair::get();auto& probe=Probe::get();
    std::vector<uint64_t> delivered;unsigned invalidated{};bool consumer_error{};
    require(!pair.arm({},{}),"empty providers cannot arm");
    require(pair.arm([&]()->std::optional<Pair::Scope>{if(fixture.provider_error)throw std::runtime_error("provider");return fixture.scoped?std::optional{fixture.scope}:std::nullopt;},[&](uintptr_t resource){return fixture.observe(resource);},[&](const Pair::Event* event){
        if(!event){++invalidated;return;}const auto saved=pair.sample(event->scope.scene.dispatch_sequence);
        require(!saved.events.empty() && saved.events.back().serial==event->serial && event->thread==GetCurrentThreadId(),"consumer synchronously receives the exact retained event outside the observer lock");
        delivered.push_back(event->serial);if(consumer_error)throw std::runtime_error("consumer");
    }),"explicit pair observer attaches to existing hook startup");
    require(!pair.arm([]{return std::optional<Pair::Scope>{};},[](uintptr_t){return RenderTargetProbe::Observation{};}),"pair configuration cannot be replaced");
    require(probe.install(reinterpret_cast<void*>(post),reinterpret_cast<void*>(compositor),[]{return std::optional<Probe::Scope>{};},[](uintptr_t){return RenderTargetProbe::Observation{};}),"real native colour detours installed once");
    require(!probe.attach_raw([](const auto&){}),"raw tap cannot be changed after hook publication");
    for(uint32_t slot=0;slot<2;++slot){fixture.bind(slot);invoke_post(post,fixture.scope.scene.entry);invoke_compositor(compositor,fixture.rp()+0x10);
        require(fixture.seen_post==std::array<uintptr_t,10>{1,reinterpret_cast<uintptr_t>(&fixture.output),3,4,fixture.scope.scene.entry,0xf1234567,7,8,9,10} && fixture.delta==-.125f &&
                fixture.seen_compositor==std::array<uintptr_t,3>{fixture.wrapper(),fixture.rp()+0x10,0xfd},"all eleven post and three compositor native arguments preserved exactly");}
    auto sample=pair.sample(77);require(sample.armed && sample.installed && !sample.instruction_guards && sample.events.size()==6 && !sample.refused && !sample.errors && !sample.dropped && !sample.raw_errors && fixture.posts==2 && fixture.compositors==2,"both actual native eye stage sequences captured without fictional fixture shipping guards");
    require(delivered==std::vector<uint64_t>{1,2,3,4,5,6} && !invalidated && !sample.aborted && !sample.consumer_errors,"both ordinary eyes reach the explicit synchronous consumer once in native order");
    for(size_t i=0;i<6;++i){const auto& e=sample.events[i];require(e.scope.scene.slot==i/3 && uint32_t(e.boundary.stage)==i%3 && e.before==e.after && e.before.count==8 && e.before.view_flags.size()==8 && e.output.resource==0xf000+i/3 && e.output.descriptors.views[0].subresources==1,"exact eye/stage/resource attribution and full auxiliary inventory retained");}
    require(probe.sample(77).events.empty() && pair.sample(78).events.empty(),"sole-primary observer stays strict and no nearest-frame attribution exists");
    std::ostringstream initial;Pair::write(initial,sample,3840,2160);require(initial.str().find("\"gpu_completion_verified\":false")!=std::string::npos && initial.str().find("\"final_writer_verified\":false")!=std::string::npos && initial.str().find("\"resource_leases\":false")!=std::string::npos,"CPU output attribution never promotes leases, final writer or GPU completion");
    if(argc==2){std::ofstream output{argv[1]};output<<initial.str()<<'\n';require(bool(output),"actual detour fixture exported");}
    fixture.bind(0);const auto good=fixture.scope;
    for(unsigned bad=0;bad<11;++bad){fixture.scope=good;fixture.bind(0);fixture.inventory(8);uintptr_t entry=fixture.scope.scene.entry;
        switch(bad){case 0:fixture.scope.pair.right.token++;break;case 1:fixture.scope.scene.ordinal=2;break;case 2:put(fixture.rp(),0x140,uintptr_t(1));break;
        case 3:put(fixture.lp(),0x29c78,uint32_t(43));break;case 4:fixture.inventory(65);break;case 5:put(fixture.lp()+0x168+2*0xa60,0,uint64_t(0));break;
        case 6:put(fixture.lp()+0x168+2*0xa60,0,uint64_t(0x40));break;case 7:entry++;break;case 8:fixture.scope.pair.producer_revision++;break;
        case 9:fixture.scope.scene.slot=2;break;case 10:fixture.scope.scene.renderer=UINTPTR_MAX;break;}
        const auto before=fixture.posts;invoke_post(post,entry);require(fixture.posts==before+1 && pair.sample(77).events.back().refusal!=Pair::Refusal::none,"each invalid provenance/context/inventory refuses while original runs once");}
    for(unsigned change=1;change<5;++change){fixture.scope=good;fixture.bind(0);fixture.inventory(8);put(fixture.wrapper(),0x278,uintptr_t(0xf000));fixture.mutation=change;invoke_post(post,fixture.scope.scene.entry);fixture.mutation=0;fixture.scoped=true;
        require(pair.sample(77).events.back().refusal!=Pair::Refusal::none,"counter/provenance/resource/expired scope changes during actual callback cannot certify a colour boundary");}
    fixture.scope=good;fixture.bind(0);fixture.inventory(8);put(fixture.wrapper(),0x278,uintptr_t(0xf000));
    const auto size=pair.sample(77).events.size();const auto lost_before=invalidated;fixture.scoped=false;invoke_post(post,fixture.scope.scene.entry);fixture.scoped=true;require(pair.sample(77).events.size()==size && invalidated==lost_before+1,"unscoped original work stays unlabelled and invalidates an active consumer instead of leaving native RTV state redirected");
    fixture.provider_error=true;invoke_post(post,fixture.scope.scene.entry);fixture.provider_error=false;require(pair.sample(77).errors==1 && probe.raw_errors()==0,"pair provider exception remains isolated and transparent");
    fixture.observer_error=true;invoke_post(post,fixture.scope.scene.entry);fixture.observer_error=false;require(pair.sample(77).events.back().refusal==Pair::Refusal::resource,"failed cached descriptor observation never grants resource attribution");
    const auto before=pair.sample(77).events.size();fixture.post_error=true;try{invoke_post(post,fixture.scope.scene.entry);require(false,"post exception expected");}catch(const std::runtime_error&){}fixture.post_error=false;require(pair.sample(77).events.size()==before,"native post exception propagates without retry or completed boundary");
    const auto abort_before=invalidated;const auto native_before=fixture.compositors;bool original_propagated{};
    fixture.compositor_error=true;try{invoke_compositor(compositor,fixture.rp()+0x10);}catch(const std::runtime_error& error){original_propagated=std::string_view(error.what())=="native compositor";}fixture.compositor_error=false;
    require(original_propagated && fixture.compositors==native_before+1 && invalidated==abort_before+1 && pair.sample(77).aborted==1 && pair.sample(77).events.size()==before+1 && pair.sample(77).events.back().boundary.stage==Probe::Stage::compositor_before,"native compositor exception propagates unchanged once, invalidates consumer and never fabricates an after event");
    const auto failures_before=invalidated;consumer_error=true;const auto posts_before=fixture.posts;invoke_post(post,fixture.scope.scene.entry);consumer_error=false;
    require(fixture.posts==posts_before+1 && invalidated==failures_before+1 && pair.sample(77).consumer_errors==1 && !probe.raw_errors(),"throwing consumer is isolated, invalidated once and never retries native rendering");
    for(unsigned i=0;i<70;++i)invoke_post(post,fixture.scope.scene.entry);require(pair.sample(77).events.size()==Pair::CAPACITY && pair.sample(77).dropped>0,"bounded explicit pair event loss cannot suppress native rendering");
    std::cout<<"ordinary pair colour: actual detours, both eye scopes, immutable sole-primary guard, auxiliary inventory and transparent failures PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
