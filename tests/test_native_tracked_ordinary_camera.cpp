#include "NativeTrackedOrdinaryCamera.h"
#include "ForeignViewHold.h"
#include "StereoHoldControl.h"
#include <filesystem>
#include <fstream>
#include "NativeOrdinaryCamera.h"
#include "FrameSyncProbe.h"
#include "fixtures/native_compact_frustum.h"
#include <intrin.h>
#include <iostream>
#include <thread>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using C=NativeTrackedOrdinaryCamera;
using Matrix=vr_camera::Matrix;
using Build=void(*)(void*,float*,float,float);
using NativeAdd=int64_t(*)(void*,void*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
constexpr uint64_t PREFIX=0x1234567800000000ull;
enum class Fault {none,left_builder,right_builder,world,projection,owned,borrowed,job,renderer,left_throw,right_throw,slot,culling};
struct Backend final:VrBackend {
    Tracking pose;unsigned reads{},actions{},starts{};
    Backend(){pose.head[13]=1.7f;pose.eyes[0][12]=-.032f;pose.eyes[1][12]=.032f;
        pose.fov={vr_projection::Tangents{-1.2f,.8f,-.7f,1.1f},vr_projection::Tangents{-.8f,1.2f,-1.1f,.7f}};}
    bool available()override{return true;}bool start()override{++starts;return true;}
    std::optional<Tracking> tracking()override{++reads;return pose;}
    std::optional<VrControllerSample> controllers()override{++actions;return VrControllerSample{};}
    bool submit(const EyeTextureCapture::Pair&,ID3D12CommandQueue*)override{return false;}
    void stop()override{}std::string status()const override{return "fake-only";}
};
struct Fixture {
    std::vector<uint8_t> list=std::vector<uint8_t>(0x2980c),renderer=std::vector<uint8_t>(0x400);
    C::Source source{};FrameSyncClock::Frame job{101,1,1,0,false};
    C::Args args{0,0x1122334455667788ull,0xf123456789abcdefull,3,4,5,6,7,8};
    Backend* backend{};VrSession session;
    std::array<C::Source,2> seen{};std::array<C::Args,2> arguments{};std::array<void*,2> pointers{};
    std::array<Matrix,2> projections{};unsigned calls{},wrappers{},returns{},provider_calls{};
    Fault fault{};bool renderer_changed{};void* borrowed{};
    bool nested_helper{},inside_helper{};unsigned helper_calls{};
    std::vector<uint8_t> helper_list=std::vector<uint8_t>(0x2980c);Matrix helper_projection{};
    Fixture():session{[&]{auto value=std::make_unique<Backend>();backend=value.get();return value;}()}{
        source.fill(0xa5);auto world=vr_camera::IDENTITY;world[10]=-1;world[12]=10;world[13]=20;world[14]=30;
        const auto view=vr_camera::inverse(world);std::memcpy(source.data(),world.data(),64);std::memcpy(source.data()+0x40,view.data(),64);
        const float near_z=.1f,far_z=1000;std::memcpy(source.data()+0x80,&near_z,4);std::memcpy(source.data()+0x84,&far_z,4);
        session.enable(true);session.set_stereo(true);session.set_render_path(VrSession::RenderPath::ordinary);session.poll();borrowed=source.data();
    }
    void begin(uint64_t id){std::fill(list.begin(),list.end(),0);job={uint32_t(id+100),id,int(id&1),0,false};calls=wrappers=returns=0;renderer_changed=false;}
    uintptr_t native_renderer()const{return reinterpret_cast<uintptr_t>(renderer.data())+uintptr_t(renderer_changed);}
    C::Provider provider(){return [&](auto bytes,const auto& frame)->std::optional<C::Prepared>{
        ++provider_calls;const auto snapshot=session.ordinary_frame(frame);if(!snapshot)return {};
        Matrix anchor{};std::memcpy(anchor.data(),bytes.data(),64);return ordinary_tracked_camera::prepare(bytes,anchor,frame,*snapshot,std::chrono::steady_clock::now());};}
    bool arm(C& camera,unsigned jobs=4,C::Provider supplied={},bool continuous=false){return camera.arm([&]{return native_renderer();},[&]{return std::optional{job};},supplied?std::move(supplied):provider(),jobs,continuous);}
    int32_t count()const{int32_t value{};std::memcpy(&value,list.data()+0x29808,4);return value;}
};
Fixture* active{};uintptr_t builder_return{};
__declspec(noinline) int64_t native_add(void*,void*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
__declspec(noinline) int64_t helper_site(NativeAdd volatile,void*,void*,const C::Args&);
__declspec(noinline) void builder(void*,float* output,float,float){
    builder_return=reinterpret_cast<uintptr_t>(_ReturnAddress());for(size_t i=0;i<16;++i)output[i]=-77-float(i);
}
__declspec(noinline) int64_t native_body(void* list,void* source,const C::Args& args){
    auto& f=*active;int32_t slot{};std::memcpy(&slot,static_cast<uint8_t*>(list)+0x29808,4);
    if(ForeignViewHold::foreign(args[0]) && slot>=2){ // a game PIP view behind the pair: appended after it
        require(slot>=0 && slot<64,"fixture PIP slot range");std::memcpy(static_cast<uint8_t*>(list)+8+size_t(slot)*0xa60,&args[0],8);
        const int32_t count=slot+1;std::memcpy(static_cast<uint8_t*>(list)+0x29808,&count,4);return int64_t(PREFIX|uint32_t(slot));}
    require(slot>=0&&slot<2,"fixture native slot range");
    if(!f.inside_helper){++f.calls;f.arguments[slot]=args;f.pointers[slot]=source;std::memcpy(f.seen[slot].data(),source,f.seen[slot].size());}
    if(f.nested_helper && !f.inside_helper){
        std::fill(f.helper_list.begin(),f.helper_list.end(),0);auto helper_args=args;helper_args[0]=0x44;
        f.inside_helper=true;helper_site(&native_add,f.helper_list.data(),source,helper_args);f.inside_helper=false;++f.helper_calls;
        require(f.helper_projection[0]==-77,"nested helper at the SAME verified builder caller must be masked from the outer ordinary eye");
    }
    float near_z{},far_z{};std::memcpy(&near_z,static_cast<uint8_t*>(source)+0x80,4);std::memcpy(&far_z,static_cast<uint8_t*>(source)+0x84,4);
    const bool skip=f.fault==(slot?Fault::right_builder:Fault::left_builder);
    auto projection=vr_camera::IDENTITY;
    if(!skip){Build volatile build=&builder;build(source,projection.data(),near_z,far_z);}
    if(f.inside_helper)f.helper_projection=projection;else f.projections[slot]=projection;
    const auto entry=static_cast<uint8_t*>(list)+8+size_t(slot)*0xa60;const uint64_t flags=args[0];std::memcpy(entry,&flags,8);
    // Native cache writer initializes +45c during add. +49c is a later
    // main-helper copy and deliberately remains a distinct stale canary here.
    std::memcpy(entry+0x34,source,C::Source{}.size());native_compact_frustum(entry+0x34);
    std::memcpy(entry+0x45c,projection.data(),64);
    std::memset(entry+0x49c,0x5a,64);
    if(f.fault==Fault::culling)std::memset(entry+0x34+0x340,0,12*sizeof(float));
    const int32_t count=slot+1;std::memcpy(static_cast<uint8_t*>(list)+0x29808,&count,4);
    if(f.fault==Fault::world)entry[0x34+0x30]^=1;
    if(f.fault==Fault::projection)entry[0x45c+0x20]^=1;
    if(f.fault==Fault::owned)static_cast<uint8_t*>(source)[0x300]^=1;
    if(f.fault==Fault::borrowed)static_cast<uint8_t*>(f.borrowed)[0x300]^=1;
    if(f.fault==Fault::job)f.job.invalid=true;
    if(f.fault==Fault::renderer)f.renderer_changed=true;
    if(f.fault==(slot?Fault::right_throw:Fault::left_throw))throw std::runtime_error("actual native add exception");
    return int64_t(PREFIX|uint32_t(slot+(f.fault==Fault::slot)));
}
__declspec(noinline) int64_t native_add(void* list,void* source,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6,uint64_t a7,uint64_t a8){
    ++active->wrappers;const auto result=native_body(list,source,{a0,a1,a2,a3,a4,a5,a6,a7,a8});++active->returns;return result;
}
__declspec(noinline) int64_t main_site(NativeAdd volatile add,void* list,void* source,const C::Args& a){
    const auto result=add(list,source,a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]);++active->returns;return result;
}
__declspec(noinline) int64_t helper_site(NativeAdd volatile add,void* list,void* source,const C::Args& a){
    const auto result=add(list,source,a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]);++active->returns;return result;
}
C::Add unhooked(){return [](void* list,void* source,const auto& args){return native_body(list,source,args);};}
void check_no_scope_leak(){
    auto& f=*active;const auto fault=f.fault;f.fault=Fault::none;std::vector<uint8_t> list(0x2980c);
    native_body(list.data(),f.source.data(),f.args);f.fault=fault;
    require(f.projections[0][0]==-77,"the SAME verified native builder caller sees no leaked eye binding after add/unwind");
}
void local_positive_and_capacity(){
    Fixture f;active=&f;C camera;require(f.arm(camera),"bounded tracked adapter arms explicitly");const auto original=f.source;
    for(uint64_t id=1;id<=4;++id){f.begin(id);const auto result=camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
        const auto receipts=camera.receipts();require(uint64_t(result)==PREFIX && f.calls==2 && f.source==original,"both real native calls retain original left result and borrowed source bytes");
        require(receipts.size()==id && receipts.back().accepted && receipts.back().native_calls==2 && receipts.back().applied==std::array{true,true} &&
            receipts.back().pair && NativeCameraEntries::get().current(*receipts.back().pair),"actual two native slots/projections join one explicit ordinary receipt");
        require(f.arguments[0]==f.args && f.arguments[1]==f.args && f.pointers[0]!=f.source.data() && f.pointers[1]!=f.source.data() && f.pointers[0]!=f.pointers[1],"both adds forward all nine exact slots using distinct owned source copies");
        require(f.projections[0][8]>.19f && f.projections[1][8]<-.19f,"different actual asymmetric builder overrides reach the two native entries");
        require(camera.native_camera_id_arg()==0x88,"native camera id argument byte is observable");
        for(size_t eye=0;eye<2;++eye){float cap{};std::memcpy(&cap,f.list.data()+8+eye*0xa60+0x34+0x30c,4);
            require(std::abs(cap+.1f)<1e-5f,"actual native add path corrects each asymmetric compact near cap before publishing the pair");}
        require(f.backend->reads==id && f.backend->actions==id && f.provider_calls==id,"both camera adds use exactly one frozen tracking and action read per native job");
        const auto revision=*NativeCameraEntries::get().revision(reinterpret_cast<uintptr_t>(f.list.data())-0x160);
        std::array<NativeCameraEntries::Entry,2> views{};for(size_t i=0;i<2;++i){views[i]={reinterpret_cast<uintptr_t>(f.list.data())+8+i*0xa60,int32_t(i),NativeCameraEntries::Role::main,{}};std::memcpy(views[i].world.data(),reinterpret_cast<void*>(views[i].address+0x34),64);}
        require(!NativeCameraEntries::get().primary(reinterpret_cast<uintptr_t>(f.list.data())-0x160,views,revision) && !NativeCameraEntries::get().pair(reinterpret_cast<uintptr_t>(f.list.data())-0x160,views,revision),"tracked ordinary pair never impersonates sole-main or PIP");
        check_no_scope_leak();
    }
    const auto old=*camera.receipts().back().pair;f.begin(5);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
    require(f.calls==1 && f.pointers[0]==f.source.data() && f.projections[0][0]==-77 && camera.receipts().size()==4 && !NativeCameraEntries::get().current(old),"capacity returns to exactly one raw native add and invalidates old stereo provenance");
}
void second_camera_id_diagnostic(){
    Fixture f;active=&f;C camera;require(f.arm(camera),"camera id diagnostic arms");
    camera.set_second_camera_id(1);f.begin(1);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
    auto expected=f.args;expected[1]=(f.args[1]&~uint64_t{0xff})|1;
    require(f.calls==2 && f.arguments[0]==f.args && f.arguments[1]==expected,"diagnostic changes only slot 1's camera id byte");
    camera.set_second_camera_id(-1);f.begin(2);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
    require(f.calls==2 && f.arguments[1]==f.args,"cleared diagnostic restores the inherited native id");
}
void continuous_producer(){
    Fixture f;active=&f;C camera;require(f.arm(camera,4,{},true),"continuous producer requires explicit selection");
    require(camera.readiness().reason==OwnedStereoReadiness::Reason::disabled && !camera.readiness().checks,"controls-OFF route has no owned-player readiness dependency");
    for(uint64_t id=1;id<=12;++id){f.begin(id);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());const auto receipt=camera.receipt(id);
        require(f.calls==2 && receipt && receipt->accepted && receipt->prepared && receipt->prepared->render_id==id && receipt->prepared->eyes[0].world!=receipt->prepared->eyes[1].world,"each continuous job retains exact independently prepared tracked eyes");
        require(camera.receipts().size()<=4 && !camera.dropped(),"continuous receipts remain bounded without stopping native work");
        if(id>4)require(!camera.receipt(id-4),"retired native provenance is no longer available");
    }
    camera.request_stop();f.begin(13);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(f.calls==1 && !camera.receipt(13),"explicit stop still restores single native call");
}
// 22 Oct: a swap-chain reset made the consumer stop the producer for the rest of the process.
// restart() lifts that stop: the stream warms up and settles again, then produces stereo.
void restart_after_consumer_stop(){
    Fixture f;active=&f;C camera;require(!camera.restart(),"a producer that was never armed cannot restart");
    const float fov=90;std::memcpy(f.source.data()+0x2d0,&fov,4); // warmup eligibility reads the camera's fov
    require(f.arm(camera,4,{},true),"continuous producer arms");camera.warmup(2);
    const auto job=[&](uint64_t id){f.begin(id);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());return camera.receipt(id);};
    job(1);job(2);require(f.calls==1 && camera.receipts().empty(),"warmup forwards the native camera");
    require(job(3) && camera.receipt(3)->accepted && f.calls==2,"stereo runs after the warmup");
    camera.request_stop();require(camera.stopped() && !job(4) && f.calls==1,"consumer stop: native mono, no receipt");
    require(camera.restart() && !camera.stopped() && !camera.suspended(),"restart lifts the consumer's stop");
    require(!job(5) && f.calls==1 && !job(6) && f.calls==1,"after a restart the warmup applies again: native mono, no tracking");
    const auto resumed=job(7);
    require(resumed && resumed->accepted && f.calls==2 && resumed->prepared && resumed->prepared->render_id==7,"the first job after the warmup is a fully checked stereo job");
    require(job(8) && camera.receipt(8)->accepted && f.calls==2,"stereo keeps running");
    camera.request_suspend();require(camera.suspended(),"suspended");
    require(camera.restart() && !camera.suspended() && !camera.stopped(),"restart also clears a pending suspension");
    job(9);job(10);require(f.calls==1,"warmup after the second restart");
    f.begin(11);f.renderer_changed=true;camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());const auto changed=camera.receipt(11);f.renderer_changed=false;
    require(changed && !changed->accepted && camera.stopped() && f.calls==1,"a changed renderer is the producer's own stop");
    require(camera.restart(),"an own stop is lifted the same way; the next job is re-checked from scratch");
    job(12);job(13);require(f.calls==1,"warmup again");
    require(job(14) && camera.receipt(14)->accepted && f.calls==2,"stereo resumes with the original renderer");
}
void continuous_suspension(){
    Fixture f;active=&f;C camera;require(f.arm(camera,4,{},true),"continuous producer arms");bool open{};unsigned gates{};
    camera.set_resume_gate([&]{++gates;return open;});
    const auto job=[&](uint64_t id){f.begin(id);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());return camera.receipt(id);};
    for(uint64_t id=1;id<=2;++id)require(job(id) && f.calls==2,"stereo runs before any suspension");
    camera.request_suspend();camera.request_suspend();
    require(camera.suspended() && !camera.stopped() && camera.suspension().suspensions==1,"recoverable refusal suspends once without a stop");
    const auto reads=f.backend->reads;
    for(uint64_t id=3;id<=5;++id){const auto r=job(id);
        require(r && !r->accepted && r->refusal==C::Refusal::suspended && f.calls==1 && f.pointers[0]==f.source.data() && f.projections[0][0]==-77,"suspended job is one untouched native mono add");}
    require(f.backend->reads==reads && f.provider_calls==2 && gates==3 && camera.suspension().suspended_jobs==3,"no tracking acquisition while suspended; gate asked once per job");
    open=true;const auto resumed=job(6);
    require(resumed && resumed->accepted && f.calls==2 && !camera.suspended() && camera.suspension().resumes==1 && gates==4,"open gate admits a fresh fully checked stereo job");
    require(job(7) && f.calls==2 && gates==4,"running stereo never consults the gate");
    camera.request_suspend();open=false;require(job(8) && f.calls==1,"second episode suspends again");
    camera.request_stop();open=true;f.begin(9);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
    require(f.calls==1 && camera.stopped() && !camera.receipt(9),"a stop always wins over suspension");
    Fixture g;active=&g;C bounded;require(g.arm(bounded),"bounded producer arms");bounded.request_suspend();
    require(bounded.stopped() && !bounded.suspended(),"bounded experiments keep stop-on-refusal");
}
void continuous_hold(){
    // Profiling A/B: held jobs are untouched native mono through the suspension path, the open
    // resume gate is not even asked while held, and release resumes a fully checked stereo job.
    Fixture f;active=&f;C camera;require(f.arm(camera,4,{},true),"continuous producer arms");unsigned gates{};
    camera.set_resume_gate([&]{++gates;return true;});
    const auto job=[&](uint64_t id){f.begin(id);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());return camera.receipt(id);};
    const auto first=job(1);require(first && first->accepted && f.calls==2,"stereo before the hold");
    camera.hold(true);
    require(camera.held() && camera.suspended() && !camera.stopped(),"hold suspends without a stop");
    for(uint64_t id=2;id<=4;++id){const auto r=job(id);
        require(r && !r->accepted && r->refusal==C::Refusal::suspended && f.calls==1 && f.pointers[0]==f.source.data(),"held job is one native mono add");}
    require(gates==0,"the resume gate is never consulted while held");
    camera.hold(false);const auto resumed=job(5);
    require(!camera.held() && resumed && resumed->accepted && f.calls==2 && !camera.suspended() && gates==1 && camera.suspension().resumes==1,"release resumes through the gate with full checks");
    Fixture g;active=&g;C bounded;require(g.arm(bounded),"bounded producer arms");bounded.hold(true);
    require(!bounded.held() && !bounded.stopped() && !bounded.suspended(),"bounded experiments ignore the hold");
}
void stereo_hold_control(){
    require(StereoHoldControl::parse("mono")==true && StereoHoldControl::parse("stereo\r")==false,"exact words, CRLF tolerated");
    require(!StereoHoldControl::parse("Mono") && !StereoHoldControl::parse("mon") && !StereoHoldControl::parse(""),"anything else keeps the state");
    const auto path=std::filesystem::temp_directory_path()/("sm2vr_hold_"+std::to_string(GetCurrentProcessId())+".txt");
    StereoHoldControl control;require(!control.configure("relative.txt") && control.configure(path),"absolute control path only");
    require(!control.poll(0) && !control.held(),"missing file keeps stereo");
    {std::ofstream f{path};f<<"mono\n";}
    require(!control.poll(500),"polled at most once a second");
    require(control.poll(1000)==true && control.held(),"mono holds");
    {std::ofstream f{path};f<<"bogus";}
    require(!control.poll(2000) && control.held(),"an unknown word keeps the hold");
    {std::ofstream f{path};f<<"stereo";}
    require(control.poll(3000)==false && !control.held(),"stereo releases");
    require(!control.poll(4000),"no change, no event");
    std::filesystem::remove(path);
}
// docs/LOAD_CRASH_2026-10-26.md: a game PIP view behind the pair overruns the game's HDR table.
// While the foreign-view gate holds, every job is one untouched native mono add (the PIP then gets
// slot 1, as without the mod), nothing stops or suspends, and a suspension's resume gate is not
// consulted (a PIP blocks every resume path); stereo returns once the gate lets go.
void foreign_view_gate(){
    Fixture f;active=&f;C camera;require(f.arm(camera,4,{},true),"continuous producer arms");bool held{},open{true};unsigned gates{};
    camera.set_foreign_gate([&]{return held;});camera.set_resume_gate([&]{++gates;return open;});
    const auto job=[&](uint64_t id){f.begin(id);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());return camera.receipt(id);};
    require(job(1) && camera.receipt(1)->accepted && f.calls==2,"stereo runs while no PIP shows");
    held=true;
    for(uint64_t id=2;id<=4;++id){const auto r=job(id);
        require(r && !r->accepted && r->refusal==C::Refusal::foreign_view && f.calls==1 && f.count()==1 && f.pointers[0]==f.source.data() && f.projections[0][0]==-77,"a held job is one untouched native mono add");}
    require(!camera.stopped() && !camera.suspended() && camera.foreign_jobs()==3 && f.provider_calls==1,"the hold neither stops nor suspends, and reads no tracking");
    held=false;require(job(5) && camera.receipt(5)->accepted && f.calls==2,"stereo returns, fully checked, once the gate lets go");
    camera.request_suspend();held=true;
    require(job(6) && camera.receipt(6)->refusal==C::Refusal::foreign_view && f.calls==1 && gates==0 && camera.suspended(),"while suspended the hold wins: the resume gate is not asked");
    held=false;require(job(7) && camera.receipt(7)->accepted && f.calls==2 && gates==1 && !camera.suspended(),"then the suspension's own gate decides");
    camera.set_foreign_gate([]()->bool{throw std::runtime_error("gate failure");});
    require(job(8) && camera.receipt(8)->refusal==C::Refusal::foreign_view && f.calls==1 && !camera.stopped(),"a failing gate holds (fails closed), it never stops stereo");
    camera.set_foreign_gate({});require(job(9) && camera.receipt(9)->accepted && f.calls==2,"no gate: no hold");
}
// 31 Oct 19:00: TAA in the game's settings. Every pair tried was refused (no DLSS evaluation), each
// refusal stopped the stack and three cold restarts ended stereo for the session. Held like a PIP
// instead: mono, nothing stops, and stereo returns on its own once DLSS is back. Whichever way the
// job would have gone: running, suspended, or the first job after a device-reset restart.
void dlss_gate(){
    Fixture f;active=&f;C camera;require(f.arm(camera,4,{},true),"continuous producer arms");bool held{},open{true};unsigned gates{};
    camera.set_dlss_gate([&]{return held;});camera.set_resume_gate([&]{++gates;return open;});
    const auto job=[&](uint64_t id){f.begin(id);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());return camera.receipt(id);};
    require(job(1) && camera.receipt(1)->accepted && f.calls==2,"stereo runs while DLSS does");
    held=true;
    for(uint64_t id=2;id<=4;++id){const auto r=job(id);
        require(r && !r->accepted && r->refusal==C::Refusal::no_dlss && f.calls==1 && f.count()==1 && f.pointers[0]==f.source.data() && f.projections[0][0]==-77,"a held job is one untouched native mono add");}
    require(!camera.stopped() && !camera.suspended() && camera.dlss_jobs()==3 && f.provider_calls==1,"the hold neither stops nor suspends, and reads no tracking");
    held=false;require(job(5) && camera.receipt(5)->accepted && f.calls==2,"DLSS back: stereo returns by itself, fully checked");
    camera.request_suspend();held=true;
    require(job(6) && camera.receipt(6)->refusal==C::Refusal::no_dlss && gates==0 && camera.suspended(),"while suspended the hold wins: the resume gate is not asked");
    held=false;require(job(7) && camera.receipt(7)->accepted && gates==1 && !camera.suspended(),"then the suspension's own gate decides");
    // A device reset under TAA: the re-arm restarts the producer and the hold still stands.
    held=true;require(camera.restart() && !job(8)->accepted && camera.receipt(8)->refusal==C::Refusal::no_dlss && !camera.stopped(),"a restart does not lift the hold");
    held=false;camera.set_dlss_gate([]()->bool{throw std::runtime_error("gate failure");});
    require(job(9) && camera.receipt(9)->refusal==C::Refusal::no_dlss && !camera.stopped(),"a failing gate holds (fails closed), it never stops stereo");
    camera.set_dlss_gate({});require(job(10) && camera.receipt(10)->accepted,"no gate: no hold");
}
OwnedStereoReadiness::Observation owned_observation(OwnedStereoReadiness::Clock::time_point now){
    return {11,{11,22,33,44,55,66,77},now,now,true,true,true};
}
void owned_readiness_rules(){
    using R=OwnedStereoReadiness;const auto now=R::Clock::time_point{}+std::chrono::seconds(10);
    for(unsigned fault=0;fault<10;++fault){R readiness;auto value=owned_observation(now);
        if(fault==0)value.actor=0;if(fault==1)value.pose=false;if(fault==2)value.owner[0]=99;if(fault==3)value.current=false;
        if(fault==4)value.world=false;if(fault==5)value.received+=std::chrono::milliseconds(1);
        if(fault==6)value.received-=std::chrono::milliseconds(101);if(fault==7)value.owner_received-=std::chrono::milliseconds(101);
        if(fault==8)value.owner_received+=std::chrono::milliseconds(1);if(fault==9)value.owner[6]=0;
        require(!readiness.observe(1,value,now) && !readiness.snapshot().consecutive,"missing/mismatched/native-invalid/world-invalid/future/stale ownership cannot earn readiness");
        value=owned_observation(now);
        require(!readiness.observe(1,value,now) && !readiness.snapshot().consecutive,"correcting the same refused job never earns startup credit");
        require(!readiness.observe(2,value,now) && !readiness.observe(3,value,now) && readiness.observe(4,value,now),"three distinct fresh exact-owner jobs are required after refusal");
        require(readiness.observe(4,value,now) && readiness.snapshot().consecutive==3 && readiness.snapshot().ready_jobs==1,"second same-job validation checks freshness without another credit");
        value.owner[2]=123;require(!readiness.observe(5,value,now) && readiness.snapshot().consecutive==1 && readiness.snapshot().owner_changes==1,"replacement controller restarts exact-owner stabilization");
        require(!readiness.observe(6,value,now) && readiness.observe(7,value,now),"replacement owner needs three independent jobs");
        readiness.gap();require(!readiness.observe(8,value,now) && readiness.snapshot().consecutive==1,"ambiguous/absent camera job discards readiness credit");
    }
    for(size_t component=0;component<7;++component){R readiness;auto value=owned_observation(now);
        require(!readiness.observe(1,value,now) && !readiness.observe(2,value,now) && readiness.observe(3,value,now),"identity fixture starts ready");
        ++value.owner[component];if(!component)value.actor=value.owner[0];
        require(!readiness.observe(4,value,now) && readiness.snapshot().consecutive==1,"every exact native owner component participates in identity replacement");
    }
    R boundary;auto value=owned_observation(now);value.received-=std::chrono::milliseconds(100);value.owner_received=value.received;
    require(!boundary.observe(1,value,now) && !boundary.observe(2,value,now) && boundary.observe(3,value,now),"exact 100ms freshness boundary remains valid");
    require(!boundary.observe(2,value,now) && !boundary.snapshot().consecutive,"an older job cannot retain or advance readiness");
    R viewer;R::Observation frozen{};frozen.viewer=true; // photo mode: no actor, no pose
    require(viewer.observe(1,frozen,now) && viewer.observe(2,frozen,now) && viewer.snapshot().reason==R::Reason::viewer,"the viewer is ready on every job while the player pose is stopped");
    require(!viewer.observe(1,frozen,now),"an older job is still refused in the viewer");
    value=owned_observation(now);
    require(!viewer.observe(3,value,now) && !viewer.observe(4,value,now) && viewer.observe(5,value,now),"leaving the viewer settles over three owned jobs again");
    // 24 Oct 20:00: a story cutscene removes the player actor. The cutscene view needs no pose.
    R cutscene;R::Observation scene{};scene.cutscene=true;
    require(cutscene.observe(1,scene,now) && cutscene.observe(2,scene,now) && cutscene.snapshot().reason==R::Reason::cutscene,"a cutscene is ready on every job without a player actor");
    value=owned_observation(now);require(!cutscene.observe(3,value,now) && !cutscene.observe(4,value,now) && cutscene.observe(5,value,now),"after the cutscene first person settles over three owned jobs");
    R::Observation gone{};require(!cutscene.observe(6,gone,now) && cutscene.snapshot().reason==R::Reason::actor_missing,"without the flag a missing actor still blocks");
    // 25 Oct 18:57: the loading screen into a mission carries the cutscene bit (0x5410); behind it
    // the level is half streamed ("white snow"), so it stands stereo down.
    R::Observation loading=scene;loading.loading=true;
    require(!cutscene.observe(7,loading,now) && cutscene.snapshot().reason==R::Reason::loading,"the loading screen stands stereo down even with the cutscene bit");
    require(cutscene.observe(8,scene,now) && cutscene.snapshot().reason==R::Reason::cutscene,"once loading ends the cutscene view is ready again");
    // 30 Oct: by default the stand-down is the menu gate only; stereo returns as the menu closes.
    {using namespace std::chrono_literals;MenuHold plain;const auto t=now;
    require(MenuHold::SETTLE==0ms && plain.update(true,t) && !plain.update(false,t+1ms),"default: no hold after the menu closes");
    // The 24 Oct hold, restored with SM2VR_DIAG_MENU_HOLD_MS=3000: owned stereo stays down for 3 s after the menu.
    MenuHold hold;hold.settle(MenuHold::LEGACY_SETTLE);
    require(!hold.update(false,t),"no menu: no hold");
    require(hold.update(true,t+1s) && hold.update(true,t+2s),"held while the menu is up");
    require(hold.update(false,t+2s+150ms) && hold.update(false,t+4999ms),"still held 0.15 s after it closes (the 20:50:24 resume) and until SETTLE");
    require(!hold.update(false,t+5s),"released SETTLE after the menu closed");
    require(hold.update(true,t+6s) && hold.update(false,t+8s) && !hold.update(false,t+9s),"each menu episode holds again");
    // The 26 Oct timer audit's check: SM2VR_DIAG_MENU_HOLD_MS=0 takes the hold out for a scripted run.
    MenuHold off;off.settle(0ms);require(off.update(true,t) && !off.update(false,t+1ms) && off.settle()==0ms,"settle 0: the gate only, no hold");
    off.settle(-5ms);require(off.settle()==MenuHold::SETTLE,"a negative settle keeps the default");}
    // 24 Oct mission select: a menu over a rendered world. The owned pose is valid, stereo stands down.
    R menu;value=owned_observation(now);require(!menu.observe(1,value,now) && !menu.observe(2,value,now) && menu.observe(3,value,now),"ready before the menu");
    value.menu=true;require(!menu.observe(4,value,now) && menu.snapshot().reason==R::Reason::menu && !menu.snapshot().consecutive,"a menu panel stands owned stereo down despite a valid pose");
    value.menu=false;require(!menu.observe(5,value,now) && !menu.observe(6,value,now) && menu.observe(7,value,now),"leaving the menu settles over three owned jobs again");
}
void owned_readiness_native_path(){
    Fixture f;active=&f;C camera;unsigned state{},observations{};bool invalidate_during_provider{};
    const auto provider=f.provider();
    require(camera.arm([&]{return f.native_renderer();},[&]{return std::optional{f.job};},[&](auto source,const auto& job){
        auto prepared=provider(source,job);if(invalidate_during_provider)state=0;return prepared;
    },4,true,{},{},[&]{++observations;auto value=owned_observation(OwnedStereoReadiness::Clock::now());
        if(!state)value.actor=0;if(state==2)value.owner[2]=123;return value;
    }),"actual native adapter accepts independent native readiness proof, without input/driver arming");
    const auto original=f.source;
    const auto run=[&](uint64_t job,bool stereo){f.begin(job);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
        const auto receipt=camera.receipt(job);require(receipt && receipt->accepted==stereo && !camera.stopped() && f.source==original,"readiness retry stays live and preserves borrowed source");
        if(stereo)require(f.calls==2 && receipt->native_calls==2,"ready job enters actual two-eye add path");
        else require(f.calls==1 && receipt->native_calls==0 && receipt->refusal==C::Refusal::initializing && f.pointers[0]==f.source.data() && f.projections[0][0]==-77,"unready job forwards exactly one native mono camera without stereo scope");
    };
    for(uint64_t job=1;job<=6;++job)run(job,false);
    require(!f.provider_calls && !f.backend->reads && observations==6,"loading camera jobs never call the provider or acquire tracking before actual player readiness");
    state=1;run(7,false);run(8,false);require(!f.provider_calls,"first two fresh same-owner jobs cannot prepare eyes");
    run(9,true);require(f.provider_calls==1 && camera.readiness().consecutive==3,"third distinct verified owner job starts stereo");
    const auto old=*camera.receipt(9)->pair;
    state=2;run(10,false);require(!NativeCameraEntries::get().current(old) && f.provider_calls==1,"owner replacement retires old provenance without acquiring tracking");
    run(11,false);invalidate_during_provider=true;run(12,false);
    require(f.provider_calls==2 && camera.readiness().consecutive==0,"ownership lost during provider acquisition is rechecked before either owned add");
    invalidate_during_provider=false;state=1;run(13,false);run(14,false);run(15,true);
    require(f.provider_calls==3 && camera.receipts().size()==4,"retry uses new jobs and bounded receipts");
}
void owned_readiness_post_read_clock(){
    Fixture f;active=&f;C camera;auto now=OwnedStereoReadiness::Clock::now();bool callback_finished{};unsigned reads{};
    const auto provider=f.provider();
    require(camera.arm([&]{return f.native_renderer();},[&]{return std::optional{f.job};},[&](auto source,const auto& job){
        auto pair=provider(source,job);now=pair->eyes[0].tracking.captured;return pair;
    },4,true,{},[&]{++reads;require(callback_finished,"producer readiness validator must sample its clock after the observation callback returns");return now;},[&]{
        callback_finished=false;now=OwnedStereoReadiness::Clock::now();auto value=owned_observation(now);
        callback_finished=true;return value;
    }),"post-read clock fixture arms actual producer");
    for(uint64_t job=1;job<=3;++job){callback_finished=false;f.begin(job);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
        require(f.calls==(job==3?2u:1u) && !camera.stopped(),"fresh publications observed after callback entry cannot reset three-job readiness");}
    require(reads>=4 && camera.receipt(3)->accepted && f.provider_calls==1,"third same-owner job starts stereo using post-read freshness");
}
void continuous_tracking_startup(){
    Fixture f;active=&f;C camera;bool available=false;const auto provider=f.provider();
    require(f.arm(camera,4,[&](auto source,const auto& job)->std::optional<C::Prepared>{return available?provider(source,job):std::nullopt;},true),"live startup retry fixture arms");
    for(uint64_t id=1;id<=6;++id){f.begin(id);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
        require(f.calls==1 && f.pointers[0]==f.source.data() && camera.receipts().size()<=4 && !camera.receipts().back().accepted,"missing startup tracking forwards one original camera and keeps bounded evidence");}
    available=true;f.begin(7);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
    require(f.calls==2 && camera.receipt(7)->accepted,"next fresh job starts stereo when tracking becomes available");
    available=false;f.begin(8);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(f.calls==1 && !camera.receipt(8)->accepted,"tracking loss after startup still refuses the stream");
    available=true;f.begin(9);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(f.calls==1 && !camera.receipt(9),"a stopped active stream cannot silently restart");
}
void startup_age_expiry(){
    for(bool established:{false,true})for(unsigned fault=0;fault<7;++fault){
        Fixture f;active=&f;C camera;auto simulated_now=std::chrono::steady_clock::now();bool late=true;unsigned time_reads{};
        const auto provider=f.provider();
        require(camera.arm([&]{return f.native_renderer();},[&]{return std::optional{f.job};},[&](auto source,const auto& job)->std::optional<C::Prepared>{
            auto pair=provider(source,job);require(bool(pair),"age fixture begins with real prepared tracking");
            simulated_now=pair->eyes[0].tracking.captured+std::chrono::milliseconds(late?101:0);
            if(late && fault==1)pair->eyes[1].projection[8]+=.01f;
            if(late && fault==2)simulated_now=pair->eyes[0].tracking.captured-std::chrono::milliseconds(1);
            if(late && fault==3)pair->eyes[1].tracking.captured-=std::chrono::milliseconds(1);
            if(late && fault==4)f.job.invalid=true;
            if(late && fault==5)f.renderer_changed=true;
            if(fault==6)simulated_now=pair->eyes[0].tracking.captured; // Expire only after native left add.
            time_reads=0;return pair;
        },4,true,{},[&]{++time_reads;if(late && fault==6 && f.calls)simulated_now+=std::chrono::milliseconds(101);return simulated_now;}),"deterministic producer clock arms");
        uint64_t id=1;
        if(established){late=false;f.begin(id++);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(camera.receipt(1)->accepted,"age recovery fixture first establishes an actual stereo stream");late=true;}
        f.begin(id);const auto original=f.source;camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
        const auto receipt=camera.receipt(id);require(receipt && !receipt->accepted && f.source==original,"aged provider never publishes stereo or mutates borrowed source");
        if(!fault){require(receipt->expired_pre_add_retry && receipt->native_calls==0 && f.calls==1 && f.pointers[0]==f.source.data() && receipt->tracking_age_ms==101 && !camera.stopped(),"pure provider age expiry falls back once before either owned eye add, including established streams");}
        else require(!receipt->expired_pre_add_retry,"corrupt/future/mismatched/changed/half-added cases cannot use age recovery");
        late=false;f.begin(++id);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
        require(!fault?(camera.receipt(id) && camera.receipt(id)->accepted && f.calls==2):(!camera.receipt(id) && f.calls==1),"only pure age expiry resumes on a newly prepared valid job; corrupt/future/mismatched/changed/half-added cases stay stopped");
    }
    // A structurally valid established stream must not freeze permanently after
    // one slow provider call (the live calibration failure at job16736).
    Fixture f;active=&f;C camera;bool late=false;auto now=std::chrono::steady_clock::now();const auto provider=f.provider();
    require(camera.arm([&]{return f.native_renderer();},[&]{return std::optional{f.job};},[&](auto source,const auto& job){auto pair=provider(source,job);now=pair->eyes[0].tracking.captured+std::chrono::milliseconds(late?101:0);return pair;},4,true,{},[&]{return now;}),"active stream fixture arms");
    f.begin(1);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(camera.receipt(1)->accepted,"stream established");
    const auto old=*camera.receipt(1)->pair;
    late=true;f.begin(2);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(camera.receipt(2)->expired_pre_add_retry && !camera.receipt(2)->accepted && camera.receipt(2)->native_calls==0 && f.calls==1 && !camera.stopped() && !NativeCameraEntries::get().current(old),"established pure age expiry skips owned eyes, retires old pair and forwards one mono camera");
    late=false;f.begin(3);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(camera.receipt(3)->accepted && f.calls==2 && f.provider_calls==3,"freshly acquired new job resumes established stream without reusing expired eyes");
}
void continuous_clock_gap(){
    Fixture f;active=&f;C camera;FrameSyncClock clock;
    require(camera.arm([&]{return f.native_renderer();},[&]{return clock.camera_frame();},f.provider(),4,true),"clock gap fixture arms");
    const auto open=[&](uint32_t token){for(unsigned phase=0;phase<=2;++phase)clock.marker(phase,token);};
    const auto close=[&](uint32_t token){for(unsigned phase=3;phase<=5;++phase)clock.marker(phase,token);};
    const auto run=[&](uint64_t id){f.begin(id);const auto job=clock.camera_frame();
        camera.on_main(f.list.data(),f.source.data(),job?&*job:nullptr,f.args,unhooked());};
    open(101);run(1);require(f.calls==2 && camera.receipt(1)->accepted,"initial genuine job produces stereo");
    const auto old=*camera.receipt(1)->pair;close(101);
    for(unsigned i=0;i<6;++i){run(2);require(f.calls==1 && f.pointers[0]==f.source.data() && f.projections[0][0]==-77 &&
        !NativeCameraEntries::get().current(old),"unbound cameras run once without stereo or old provenance");}
    require(f.provider_calls==1,"unbound cameras never acquire tracking");
    open(102);run(2);require(f.calls==2 && camera.receipt(2)->accepted,"fresh genuine job resumes after clock gap");
    // Overlapping intervals are ambiguous: skip them, never choose an eye/job.
    open(103);run(3);require(f.calls==1 && f.provider_calls==2,"ambiguous interval never synthesizes a frame");
    close(102);run(3);require(f.calls==2 && camera.receipt(3)->accepted,"unique newer job resumes after ambiguity clears");
    // An explicit invalid job, unlike absence of an identity, stays fatal.
    f.begin(4);auto invalid=*clock.camera_frame();invalid.invalid=true;
    camera.on_main(f.list.data(),f.source.data(),&invalid,f.args,unhooked());
    close(103);open(104);run(4);require(f.calls==1 && !camera.receipt(4),"corrupt job cannot restart a stopped stream");
}
void continuous_tracking_recovery(){
    Fixture f;active=&f;C camera;bool available=true,missing=false,corrupt=false;unsigned attempts{};const auto provider=f.provider();
    require(camera.arm([&]{return f.native_renderer();},[&]{return std::optional{f.job};},[&](auto source,const auto& job)->std::optional<C::Prepared>{
        ++attempts;missing=!available;if(missing)return {};auto pair=provider(source,job);if(corrupt && pair)pair->render_id=0;return pair;
    },4,true,[&]{return missing;}),"explicit tracking-recovery producer arms");
    f.begin(1);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(camera.receipt(1)->accepted,"first tracked stereo job accepted");
    const auto old=*camera.receipt(1)->pair;
    available=false;
    for(uint64_t id:{2ull,3ull}){f.begin(id);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(f.calls==1 && f.pointers[0]==f.source.data() && !camera.receipt(id)->accepted && !NativeCameraEntries::get().current(old),"tracking loss forwards one unchanged mono camera and retires previous stereo provenance");}
    available=true;f.begin(4);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(f.calls==2 && camera.receipt(4)->accepted && camera.receipt(4)->prepared->render_id==4 && attempts==4,"fresh tracking resumes both eyes on a newly validated job");
    corrupt=true;f.begin(5);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(f.calls==1 && !camera.receipt(5)->accepted,"corrupt prepared tracking remains a terminal refusal");
    corrupt=false;f.begin(6);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());require(f.calls==1 && attempts==5,"recovery never bypasses a terminal validation failure");
}
void refusal_and_unwind(){
    {Fixture f;active=&f;C camera;require(f.arm(camera),"completed-pair stop fixture arms");f.begin(1);
        camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());const auto pair=*camera.receipts()[0].pair;
        require(NativeCameraEntries::get().current(pair),"stop regression begins with actual eligible native pair");std::thread stop([&]{camera.request_stop();});stop.join();
        require(!NativeCameraEntries::get().current(pair),"stopping the adapter immediately invalidates already-issued native pair provenance");
    }
    for(const auto fault:{Fault::left_builder,Fault::right_builder,Fault::world,Fault::projection,Fault::owned,Fault::borrowed,Fault::job,Fault::renderer,Fault::slot,Fault::left_throw,Fault::right_throw,Fault::culling}){
        Fixture f;active=&f;C camera;require(f.arm(camera),"negative fixture arms");f.begin(1);f.fault=fault;bool threw{};
        try{camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());}catch(const std::runtime_error& e){threw=std::string_view(e.what())=="actual native add exception";}
        const auto receipts=camera.receipts();require(receipts.size()==1 && !receipts[0].accepted && receipts[0].refusal!=C::Refusal::none,"missing/changed native output never accepts tracked pair");
        const bool native_exception=fault==Fault::left_throw||fault==Fault::right_throw;
        require(threw==native_exception && receipts[0].aborted==native_exception,"actual native exception propagates unchanged and invalidates complete stereo receipt");
        require(f.calls==((fault==Fault::right_builder||fault==Fault::right_throw)?2u:1u),"native failure cannot cause fallback replay or duplicate the original call");
        require(!NativeCameraEntries::get().revision(reinterpret_cast<uintptr_t>(f.list.data())-0x160),"refused/unwound producer leaves no eligible native pair");
        check_no_scope_leak();f.fault=Fault::none;f.begin(2);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
        require(f.calls==1 && f.projections[0][0]==-77 && camera.receipts().size()==1,"stopped producer never appends again or reuses old eye scope");
    }
    {Fixture f;active=&f;C camera;require(f.arm(camera),"stop-during-right fixture arms");f.begin(1);
        const auto add=[&](void* list,void* source,const auto& args){const auto result=native_body(list,source,args);if(f.calls==2){std::thread stop([&]{camera.request_stop();});stop.join();}return result;};
        camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,add);
        require(f.calls==2 && camera.receipts().size()==1 && !camera.receipts()[0].accepted &&
            !NativeCameraEntries::get().revision(reinterpret_cast<uintptr_t>(f.list.data())-0x160),"a stop during the final native eye must invalidate the pair before acceptance");
    }
    for(unsigned mode=0;mode<5;++mode){Fixture f;active=&f;C camera;
        auto provider=f.provider();C::Provider bad=[&,provider](auto source,const auto& job)->std::optional<C::Prepared>{
            if(mode==0)return {};if(mode==1)throw std::runtime_error("provider failure");auto prepared=provider(source,job);
            if(mode==2)prepared->eyes[1].projection[8]+=.01f;if(mode==3)prepared->eyes[1].source[0x300]^=1;return prepared;};
        require(f.arm(camera,4,bad),"invalid provider fixture arms");f.begin(1);if(mode==4)f.renderer[0x258]=1;
        camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
        require(f.calls==1 && f.pointers[0]==f.source.data() && !camera.receipts().back().accepted && f.projections[0][0]==-77,"lost/corrupt provider or pre-owned native bank stays one unmodified original call");
    }
    {Fixture f;active=&f;C camera;require(f.arm(camera),"duplicate native identity fixture arms");f.begin(1);
        camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());const auto pair=*camera.receipts()[0].pair;f.begin(1);
        camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
        require(f.calls==1 && f.provider_calls==1 && f.backend->reads==1 && !camera.receipts().back().accepted && !NativeCameraEntries::get().current(pair),"duplicate job cannot reacquire tracking or preserve earlier stereo provenance");
    }
    for(unsigned mode=0;mode<3;++mode){Fixture f;active=&f;C camera;require(f.arm(camera),"invalid native input fixture arms");f.begin(1);
        if(mode==0)f.job.invalid=true;if(mode==1)f.args[0]=0x44;
        camera.on_main(f.list.data(),f.source.data(),mode==2?nullptr:&f.job,f.args,unhooked());
        require(f.calls==1 && !f.provider_calls && !f.backend->reads && !camera.receipts().back().accepted,"invalid/missing job and PIP/excluded flags never acquire tracking or append an ordinary eye");
    }
    {Fixture f;active=&f;C camera;require(f.arm(camera),"observer failure fixture arms");f.begin(1);
        camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked(),[](const auto&,int64_t,bool){throw std::runtime_error("observer");});
        require(f.calls==1 && camera.dropped()==1 && !camera.receipts().back().accepted,"observer exception cannot unwind into engine or leave an eligible pair");check_no_scope_leak();
    }
}
void engine_hook_integration(const char* fixture_path){
    Fixture f;active=&f;auto& engine=EngineCameraProbe::get();const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    require(engine.install(reinterpret_cast<void*>(&native_add),base,0x10000000)==EngineCameraProbe::State::installed,"actual engine add detour installs");
    f.begin(10);main_site(&native_add,f.list.data(),f.source.data(),f.args);const auto site=engine.snapshot().by_caller.back().caller_rva;
    engine.set_main_view_return_rva(site);
    f.inside_helper=true;helper_site(&native_add,f.helper_list.data(),f.source.data(),f.args);f.inside_helper=false;
    const auto fixture=LoadLibraryA(fixture_path);require(fixture!=nullptr,"existing marker fixture loads");
    const auto ready=reinterpret_cast<void(*)(bool)>(GetProcAddress(fixture,"fixture_marker_ready"));
    const auto resolve=reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(fixture,"slGetFeatureFunction"));require(ready&&resolve,"marker APIs exist");
    ready(true);void* target{};require(resolve(sl::kFeaturePCL,"slPCLSetMarker",target)==sl::Result::eOk,"marker resolved");FrameSyncProbe::get().poll();
    const auto mark=reinterpret_cast<PFun_slPCLSetMarker*>(target);struct Token:sl::FrameToken{uint32_t value{};operator uint32_t()const override{return value;}}token;
    token.value=500;mark(sl::PCLMarker::eSimulationStart,token);token.value=501;mark(sl::PCLMarker::eRenderSubmitStart,token);
    const auto job=FrameSyncProbe::get().render_camera_frame();require(bool(job),"one actual hooked marker job is current");f.begin(job->render_id);f.job=*job;
    unsigned afr{},pip{};engine.set_eye_provider([&](const auto&)->std::optional<EngineCameraProbe::VrEye>{++afr;return {};});engine.set_main_observer([&](void*,const void*,const auto*){++pip;});
    require(NativeOrdinaryCamera::get().arm([&]{return f.native_renderer();},{4,1,.064f}),"old fixed-offset path deliberately armed for exclusion check");
    auto& camera=C::get();require(camera.arm([&]{return f.native_renderer();},[]{return FrameSyncProbe::get().render_camera_frame();},f.provider(),2),"new route explicitly armed, never by normal startup");
    const auto original=f.source;const auto result=main_site(&native_add,f.list.data(),f.source.data(),f.args);
    require(uint64_t(result)==PREFIX && f.wrappers==2 && f.calls==2 && f.source==original && !afr && !pip && NativeOrdinaryCamera::get().receipts().empty(),"actual main detour adds two owned tracked views exactly once each without AFR/fixed/PIP replay");
    const auto receipts=camera.receipts();require(receipts.size()==1 && receipts[0].accepted && receipts[0].job.render_id==job->render_id && receipts[0].job.token==job->token,"new route retains exact actual marker/native producer receipt");
    require(!engine.rendered_frame(job->render_id),"single-job tracked eyes cannot enter legacy adjacent-AFR image provenance");check_no_scope_leak();
    mark(sl::PCLMarker::eRenderSubmitEnd,token);
    token.value=501;mark(sl::PCLMarker::eSimulationStart,token);token.value=502;mark(sl::PCLMarker::eRenderSubmitStart,token);
    const auto next=FrameSyncProbe::get().render_camera_frame();require(bool(next),"second exact native camera job opens");f.begin(next->render_id);f.job=*next;f.nested_helper=true;
    main_site(&native_add,f.list.data(),f.source.data(),f.args);
    require(f.calls==2 && f.wrappers==4 && f.helper_calls==2 && !afr && !pip && camera.receipts().size()==2 && camera.receipts()[1].accepted &&
        f.projections[0][8]>.19f && f.projections[1][8]<-.19f && f.helper_projection[0]==-77,"nested native helper views mask and restore each outer asymmetric eye scope without a third ordinary append");
    require(f.backend->reads==2 && f.backend->actions==2 && !NativeCameraEntries::get().current(*receipts[0].pair),"helpers never reacquire tracking and the next producing job retires old native provenance");
    f.nested_helper=false;
    // The game adds a PIP view after the main view in the same frame (26 Oct 14:44, the class
    // selector): through the actual detour it lands in slot 2, behind the owned pair, and marks the hold.
    auto pip_args=f.args;pip_args[0]=0x10043;const auto before=ForeignViewHold::get().stats();
    const auto pip_slot=int32_t(helper_site(&native_add,f.list.data(),f.source.data(),pip_args));
    const auto after=ForeignViewHold::get().stats();
    require(pip_slot==2 && after.sightings==before.sightings+1 && after.behind_pair==before.behind_pair+1 && after.slot==2 && ForeignViewHold::get().held(std::chrono::steady_clock::now()),"a PIP added behind the pair is seen by the actual add detour and holds stereo");
    std::vector<uint8_t> other(0x2980c);helper_site(&native_add,other.data(),f.source.data(),pip_args);
    require(ForeignViewHold::get().stats().behind_pair==after.behind_pair && ForeignViewHold::get().stats().sightings==after.sightings+1,"a PIP in another list holds too, but is not behind the pair");
    auto plain=f.args;plain[0]=0x410001400;f.inside_helper=true;helper_site(&native_add,f.helper_list.data(),f.source.data(),plain);f.inside_helper=false;
    require(ForeignViewHold::get().stats().sightings==after.sightings+1,"shadow/UI views (no PIP flag) are not foreign views");
    mark(sl::PCLMarker::eRenderSubmitEnd,token);engine.set_eye_provider({});engine.set_main_observer({});camera.request_stop();
}
}
int main(int argc,char** argv){try{
    require(argc==2,"fixture path required");Fixture baseline;active=&baseline;
    require(C::warmup_from(nullptr)==241 && C::warmup_from("")==241 && C::warmup_from("12")==241 && C::warmup_from("241")==241,"stereo warmup defaults to and never goes below 241 jobs");
    require(C::warmup_from("2400")==2400 && C::warmup_from("99999999999999999999")==100000 && C::warmup_from("-5")==241 && C::warmup_from("3e3")==241,"stereo warmup parses digits only, clamped to 100000");
    {C passive;require(!baseline.arm(passive),"tracked native route cannot arm before the projection detour exists");}
    native_body(baseline.list.data(),baseline.source.data(),baseline.args);
    auto& projection=EngineProjectionHook::get();const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));const auto verified=builder_return-base;
    require(projection.install(reinterpret_cast<void*>(&builder),base)==EngineProjectionHook::State::installed,"actual projection detour installs");projection.set_add_return_rva(verified);
    for(const unsigned limit:{0u,5u}){C invalid;require(!baseline.arm(invalid,limit),"unbounded or zero-job native producer cannot arm");}
    {Fixture f;active=&f;C camera;camera.warmup(2);require(f.arm(camera),"warmup fixture arms");
        for(unsigned n=1;n<=3;++n){f.begin(n);const float fov=80;std::memcpy(f.source.data()+0x2d0,&fov,4);camera.on_main(f.list.data(),f.source.data(),&f.job,f.args,unhooked());
            require(f.calls==(n<3?1u:2u) && camera.receipts().size()==(n<3?0u:1u),"warmup forwards original camera before tracked stereo starts");}}
    local_positive_and_capacity();second_camera_id_diagnostic();continuous_producer();restart_after_consumer_stop();continuous_suspension();continuous_hold();foreign_view_gate();dlss_gate();stereo_hold_control();owned_readiness_rules();owned_readiness_post_read_clock();owned_readiness_native_path();continuous_tracking_startup();startup_age_expiry();continuous_tracking_recovery();continuous_clock_gap();refusal_and_unwind();engine_hook_integration(argv[1]);
    std::cout<<"tracked ordinary native adds/projection scopes/provenance/refusals/unwind PASS; synthetic camera/marker/backend only; no game/GPU/runtime/headset claim\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
