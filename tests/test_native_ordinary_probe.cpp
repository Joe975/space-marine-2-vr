#include "synthetic_code.h"
#include "NativeOrdinaryProbe.h"
#include "game_build_ported.h"
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
using P=NativeOrdinaryProbe;using E=NativeCameraEntries;
std::optional<P::Scope> scope;
bool provider_error{},post_error{};unsigned posts{},compositors{};
uintptr_t colour_wrapper{},seen_context{},seen_post_context{},seen_entry{},seen_state{};uint8_t seen_flag{};
std::array<uintptr_t,7> seen_args{};uint32_t seen_index{};float seen_delta{};
std::function<void()> compositor_body;
__declspec(noinline) void native_post(uintptr_t a,uintptr_t* b,uintptr_t c,uintptr_t d,uintptr_t e,uint32_t f,uintptr_t g,uintptr_t h,uintptr_t i,uintptr_t j,float delta){
    ++posts;seen_post_context=seen_context=a;seen_entry=e;seen_index=f;seen_delta=delta;seen_args={reinterpret_cast<uintptr_t>(b),c,d,g,h,i,j};
    if(post_error)throw std::runtime_error("native original");*b=colour_wrapper;volatile auto keep=posts;(void)keep;
}
__declspec(noinline) void native_compositor(uintptr_t colour,uintptr_t state,uint8_t flag){++compositors;seen_context=colour;seen_state=state;seen_flag=flag;if(compositor_body)compositor_body();volatile auto keep=compositors;(void)keep;}
__declspec(noinline) void invoke_post(P::Post volatile fn,uintptr_t* output,uintptr_t entry){fn(0x1122334455667788,output,3,4,entry,0xf1234567,7,8,9,10,-.125f);volatile auto keep=posts;(void)keep;}
__declspec(noinline) void invoke_compositor(P::Compositor volatile fn,uintptr_t colour,uintptr_t state,uint8_t flag){fn(colour,state,flag);volatile auto keep=compositors;(void)keep;}
template<class T> void put(uint8_t* p,size_t offset,const T& value){std::memcpy(p+offset,&value,sizeof(value));}
}
int main(int argc,char** argv){try{
    require(P::requested("1","0","0","1","0",true),"single-frame headset rendering accepts OpenXR");
    require(P::matches(NativeRenderProbe::TIMESTAMP,SYNTH(P::POST_BYTES),SYNTH(P::COMPOSITOR_BYTES)),"known ordinary native ABI accepted");
    auto altered=SYNTH(P::POST_BYTES);altered[4]^=1;require(!P::matches(NativeRenderProbe::TIMESTAMP,altered,SYNTH(P::COMPOSITOR_BYTES)) &&
        !P::matches(0,SYNTH(P::POST_BYTES),SYNTH(P::COMPOSITOR_BYTES)),"unknown post/compositor implementations refused");
    require(P::matches(game_build_ported::EXE,game_build_ported::code(P::POST_RVA),game_build_ported::code(P::COMPOSITOR_RVA)),"the 2026-10-21 post/compositor match");
    for(auto ordinary:{"","0","1","true"}) for(auto pip:{"","0","1"}) for(auto vr:{"","0","1","invalid"})
        for(auto xr:{"","0","1","invalid"}) for(auto synthetic:{"","0","1"}) {
            const auto off=[](std::string_view value){return value.empty() || value=="0";};
            require(P::requested(ordinary,pip,vr,xr,synthetic)==(std::string_view(ordinary)=="1" && off(pip) && off(vr) && off(xr) && off(synthetic)),
                "ordinary control requires PIP and all runtimes/synthetic modes off");
            const bool single_runtime=(std::string_view(vr)=="1" && off(xr)) || (std::string_view(xr)=="1" && off(vr));
            require(P::requested(ordinary,pip,vr,xr,synthetic,true)==(std::string_view(ordinary)=="1" && off(pip) && single_runtime && off(synthetic)),
                "headset path requires exactly one runtime and refuses PIP, synthetic eyes and invalid selectors");
        }
    std::vector<uint8_t> list(0x29c7c);std::array<uint8_t,0x200> renderer{};std::array<uint8_t,0x280> wrapper{};
    const auto native_list=reinterpret_cast<uintptr_t>(list.data()),entry=native_list+0x168,renderer_address=reinterpret_cast<uintptr_t>(renderer.data());
    colour_wrapper=reinterpret_cast<uintptr_t>(wrapper.data());const uintptr_t raw=0x1234;put(wrapper.data(),0x278,raw);
    const uint64_t flags=0x7f7c013;put(list.data(),0x168,flags);const uint32_t counter=42;put(list.data(),0x29c78,counter);put(renderer.data(),0x140,entry);
    auto world=vr_camera::IDENTITY;world[10]=-1;auto& entries=E::get();entries.enable();const FrameSyncClock::Frame frame{0,8,0,0,false};
    auto ticket=entries.begin(native_list+0x160,E::Role::main,frame);entries.finish(*ticket,0,world);
    const std::array<E::Entry,1> actual{{{entry,0,E::Role::main,world}}};const auto primary=entries.primary(native_list,actual,*entries.revision(native_list));
    require(primary && primary->render_id==8 && primary->token==0 && entries.current(*primary),"ordinary-only receipt authenticates exact physical main slot with token zero");
    for(unsigned n=0;n<7;++n){auto bad=*primary;switch(n){case 0:++bad.render_id;break;case 1:++bad.token;break;case 2:++bad.revision;break;
        case 3:++bad.entry;break;case 4:++bad.slot;break;case 5:++bad.render_list;break;case 6:++bad.add_list;break;}
        require(!entries.current(bad),"each primary receipt field is independently authenticated");}
    auto changed=actual;changed[0].world[12]+=.01f;require(!entries.primary(native_list,changed,primary->revision),"world mismatch cannot label another ordinary output");
    P::Scope owner;owner.primary=*primary;auto& s=owner.scene;s.ordinal=1;s.dispatch_sequence=12;s.slot=0;s.list=native_list;s.entry=entry;s.renderer=renderer_address;
    s.view_flags_before=flags;s.camera_revision_before=primary->revision;s.render_counter_before=42;s.refusal=NativeRenderProbe::SceneRefusal::none;
    s.resource_before.descriptors.epoch=1;s.resource_before.descriptors.serial=10;scope=owner;
    std::vector<P::Stage> boundaries;
    auto& probe=P::get();bool raw_throws{};std::vector<P::RawBoundary> raw_boundaries;
    require(!probe.attach_raw({}) && probe.attach_raw([&](const auto& value){if(raw_throws)throw std::runtime_error("raw observer");raw_boundaries.push_back(value);}),"startup raw tap rejects empty callbacks and has no ownership attribution");
    require(probe.install(reinterpret_cast<void*>(&native_post),reinterpret_cast<void*>(&native_compositor),[]()->std::optional<P::Scope>{
        if(provider_error)throw std::runtime_error("provider");return scope;},[](uintptr_t resource){RenderTargetProbe::View v;v.resource=resource;v.identity=resource+8;v.known=true;
        v.width=3840;v.height=2160;v.handle=19;v.created=2;v.bound=15;v.command_list=20;v.storage=v.format=DXGI_FORMAT_R8G8B8A8_UNORM;v.subresources=1;
        return RenderTargetProbe::Observation{1,20,{v}};},[&](const P::Event& event){
            const auto observed=probe.sample(event.scope.scene.dispatch_sequence);
            require(!observed.events.empty() && observed.events.back().serial==event.serial,"boundary consumer runs outside the observer lock after the exact event is retained");
            boundaries.push_back(event.stage);
        }),"ordinary post and compositor detours install together");
    uintptr_t output{};invoke_post(&native_post,&output,entry);invoke_compositor(&native_compositor,colour_wrapper,renderer_address+0x10,0xfd);
    auto sample=probe.sample(12);require(posts==1 && compositors==1 && seen_context==colour_wrapper && seen_post_context==0x1122334455667788 && seen_state==renderer_address+0x10 && seen_flag==0xfd &&
        seen_entry==entry && seen_index==0xf1234567 && seen_delta==-.125f && seen_args==std::array<uintptr_t,7>{reinterpret_cast<uintptr_t>(&output),3,4,7,8,9,10},
        "all eleven post and three compositor arguments and original calls are unchanged");
    require(sample.events.size()==3 && !sample.refused && !sample.errors && sample.events[0].stage==P::Stage::post &&
        sample.events[1].stage==P::Stage::compositor_before && sample.events[2].stage==P::Stage::compositor_after &&
        sample.events[0].wrapper==colour_wrapper && sample.events[0].output.resource==raw && sample.events[0].output.descriptors.views[0].width==3840,
        "actual full colour wrapper and cached resource descriptors are recorded at all ordinary boundaries");
    require(boundaries==std::vector<P::Stage>{P::Stage::post,P::Stage::compositor_before,P::Stage::compositor_after},"optional copy consumer receives the verified native stage order");
    require(raw_boundaries.size()==3 && raw_boundaries[0].wrapper==colour_wrapper && raw_boundaries[0].entry==entry && !raw_boundaries[0].renderer_state && raw_boundaries[1].renderer_state==renderer_address+0x10 && raw_boundaries[2].stage==P::Stage::compositor_after && !probe.raw_errors(),"raw tap preserves exact native boundary arguments independently of sole-primary attribution");
    require(probe.sample(13).events.empty(),"events are selected by exact dispatch sequence, never nearest time");
    if(argc==2){NativeRenderProbe::Snapshot completed;completed.sequence=12;completed.list=native_list;completed.renderer=renderer_address;
        completed.primary=primary;completed.scene_calls=1;auto finished=s;finished.active_entry_after=entry;finished.view_flags_after=flags;
        finished.camera_revision_after=primary->revision;finished.render_counter_after=42;completed.scenes={finished};
        NativeRenderProbe::View view;view.slot=0;view.flags=flags;view.world=world;completed.before={view};completed.after={view};
        std::ofstream fixture{argv[1]};fixture<<std::setprecision(9);P::write_capture(fixture,completed,sample,3840,2160);fixture.close();require(bool(fixture),"native fixture export succeeds");}
    const auto count=sample.events.size();scope.reset();invoke_post(&native_post,&output,entry);require(posts==2 && probe.sample(12).events.size()==count,"out-of-scene native calls remain unmodified and unlabelled");
    scope=owner;provider_error=true;invoke_post(&native_post,&output,entry);provider_error=false;require(posts==3 && probe.sample(12).errors==1,"observation failure cannot suppress native rendering");
    scope->scene.view_flags_before|=0x40;invoke_post(&native_post,&output,entry);scope=owner;
    invoke_post(&native_post,&output,entry+8);invoke_compositor(&native_compositor,colour_wrapper,0,1);
    require(probe.sample(12).refused==4 && posts==5 && compositors==2,"PIP, foreign entry and missing renderer context refuse observation while forwarding originals");
    const auto previous=probe.sample(12).events.size();post_error=true;try{invoke_post(&native_post,&output,entry);require(false,"native exception expected");}catch(const std::runtime_error&){}post_error=false;
    require(posts==6 && probe.sample(12).events.size()==previous,"native exception is never retried or reported as a completed post boundary");
    unsigned depth{};compositor_body=[&]{if(!depth){++depth;invoke_compositor(&native_compositor,colour_wrapper,renderer_address+0x10,2);--depth;}};
    invoke_compositor(&native_compositor,colour_wrapper,renderer_address+0x10,1);compositor_body={};
    require(probe.sample(12).events.size()==previous+4,"nested original calls preserve exact event order without merging native draws");
    for(unsigned n=0;n<130;++n)invoke_post(&native_post,&output,entry);
    require(probe.sample(12).events.size()==128 && probe.sample(12).dropped>0 && posts==136,"bounded retention never suppresses engine work");
    ticket=entries.begin(native_list+0x160,E::Role::pip,frame);entries.finish(*ticket,1,world);
    require(!entries.current(*primary) && !entries.primary(native_list,actual,*entries.revision(native_list)),"an extra PIP producer cannot pass the ordinary-only receipt");
    invoke_post(&native_post,&output,entry);require(probe.sample(12).refused==5,"producer change invalidates old same-pose output ownership");
    NativeRenderProbe::Snapshot dispatch;dispatch.sequence=12;dispatch.list=native_list;dispatch.primary=primary;
    std::ostringstream json;P::write_capture(json,dispatch,probe.sample(12),3840,2160);
    require(json.str().find("\"pip_enabled\":false")!=std::string::npos && json.str().find("\"gpu_pair_verified\":false")!=std::string::npos &&
        json.str().find("\"final_writer_verified\":false")!=std::string::npos,"export explicitly distinguishes CPU boundaries from final writer/GPU stereo proof");
    const auto native_before=posts;const auto errors_before=probe.sample(12).errors;raw_throws=true;invoke_post(&native_post,&output,entry);
    require(posts==native_before+1 && probe.raw_errors()==1 && probe.sample(12).errors==errors_before,"raw observer exception cannot suppress/retry the native call or contaminate sole-primary observer error accounting");
    raw_throws=false;const auto raw_before=raw_boundaries.size();const auto compositor_before=compositors;bool native_propagated{};
    compositor_body=[](){throw std::runtime_error("original compositor failure");};
    try{invoke_compositor(&native_compositor,colour_wrapper,renderer_address+0x10,1);}catch(const std::runtime_error& error){native_propagated=std::string_view(error.what())=="original compositor failure";}
    compositor_body={};require(native_propagated && compositors==compositor_before+1 && raw_boundaries.size()==raw_before+2 &&
        !raw_boundaries[raw_before].aborted && raw_boundaries.back().aborted && raw_boundaries.back().stage==P::Stage::compositor_before &&
        raw_boundaries.back().wrapper==colour_wrapper && raw_boundaries.back().renderer_state==renderer_address+0x10,
        "native unwinding emits explicit raw abort rather than completed exit and preserves original exception exactly once");
    std::cout<<"Native ordinary probe tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}}
