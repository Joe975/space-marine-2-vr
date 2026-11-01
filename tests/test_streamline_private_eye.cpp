#include "StreamlineHistoryProbe.h"
#include "StreamlineProbe.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Frame:sl::FrameToken{uint32_t value{42};operator uint32_t()const override{return value;}};
}
int main(int argc,char** argv){try{
    require(argc==2,"fixture required");using H=StreamlineHistoryProbe;using C=OrdinaryDlssContext;auto& h=H::get();auto& options=StreamlineOptionsProbe::get();auto& constants=StreamlineProbe::get();
    std::optional<H::Scope> scope;C::Cameras cameras;cameras.renderer=0x1000;cameras.counter=12;
    for(unsigned i=0;i<2;++i){cameras.world[i]=vr_camera::IDENTITY;cameras.world[i][10]=-1;cameras.world[i][12]=i*.064f;cameras.projection[i]=cameras.inverse_projection[i]=vr_camera::IDENTITY;}
    require(h.arm([&]{return scope;}),"actual SDK observer arms with exact producing Scene provider");options.enable();
    const auto dll=LoadLibraryA(argv[1]);require(dll,"fixture loads");
    const auto ready=reinterpret_cast<void(*)(bool)>(GetProcAddress(dll,"fixture_options_ready"));const auto resolve=reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(dll,"slGetFeatureFunction"));
    const auto set=reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(dll,"slSetConstants"));const auto evaluate=reinterpret_cast<PFun_slEvaluateFeature*>(GetProcAddress(dll,"slEvaluateFeature"));
    const auto new_frame=reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(dll,"slGetNewFrameToken"));const auto set_token=reinterpret_cast<void(*)(sl::FrameToken*)>(GetProcAddress(dll,"fixture_history_token"));
    const auto inspect=reinterpret_cast<void(*)(bool)>(GetProcAddress(dll,"fixture_history_inspect"));const auto actual_view=reinterpret_cast<uint32_t(*)()>(GetProcAddress(dll,"fixture_history_viewport"));const auto calls=reinterpret_cast<unsigned(*)()>(GetProcAddress(dll,"fixture_history_calls"));
    require(ready && resolve && set && evaluate && new_frame && set_token && inspect && actual_view && calls,"actual SDK fixture exports required");ready(true);inspect(true);const auto now=std::chrono::steady_clock::now();h.poll(now);options.poll(now);constants.poll(now);
    constants.set_constants_observer([&](const auto& values,const auto& frame,const auto& viewport){return h.constants_begin(values,frame,viewport);});
    // Headset (continuous) mode, as Sm2Probe arms it. Dispatch 5 stands for a refused tracked
    // packet: the headset provider returns no cameras for it.
    h.continuous(true);
    h.arm_private([&]()->std::optional<C::Cameras>{return scope && scope->dispatch!=5?std::optional{cameras}:std::nullopt;},[&](const auto& view,const auto& values){return options.submit_observed(view,values);},[&](const auto& values,const auto& frame,const auto& view){return constants.submit_observed(values,frame,view);});
    void* target{};resolve(sl::kFeatureDLSS,"slDLSSSetOptions",target);require(target,"native option entry resolves");const auto set_options=reinterpret_cast<PFun_slDLSSSetOptions*>(target);
    sl::ViewportHandle viewport{0u};sl::DLSSOptions mode;mode.mode=sl::DLSSMode::eMaxPerformance;mode.outputWidth=3840;mode.outputHeight=2160;set_options(viewport,mode);
    Frame frame;set_token(&frame);sl::FrameToken* live{};new_frame(live,nullptr);require(live==&frame,"actual getter return is the SDK owner");
    sl::Constants values;values.structVersion=2;for(auto* matrix:{&values.cameraViewToClip,&values.clipToCameraView,&values.clipToPrevClip,&values.prevClipToClip})std::memcpy(matrix,vr_camera::IDENTITY.data(),64);
    values.cameraPos={0,0,0};values.cameraRight={1,0,0};values.cameraUp={0,1,0};values.cameraFwd={0,0,-1};values.jitterOffset={0,0};values.mvecScale={1,1};values.cameraNear=.1f;values.cameraFar=1000;values.cameraFOV=1;values.cameraAspectRatio=1.5f;values.reset=sl::eFalse;values.depthInverted=sl::eTrue;values.cameraMotionIncluded=sl::eTrue;values.motionVectors3D=sl::eFalse;
    std::array<sl::Resource,4> resources;std::array<sl::ResourceTag,4> tags;const std::array<uint32_t,4> types{0,1,3,4};const sl::BaseStructure* inputs[5]{&viewport};
    for(unsigned i=0;i<4;++i){resources[i]=sl::Resource{sl::ResourceType::eTex2d,reinterpret_cast<void*>(uintptr_t(0x10000+i)),128};resources[i].width=1920;resources[i].height=1080;tags[i]=sl::ResourceTag{&resources[i],types[i],sl::ResourceLifecycle::eValidUntilEvaluate};inputs[i+1]=&tags[i];}
    auto* command=reinterpret_cast<sl::CommandBuffer*>(0x123400);scope=H::Scope{1,7,19,41,0,0x100168,0x100000};
    set(values,frame,viewport);evaluate(sl::kFeatureDLSS,frame,inputs,5,command);require(actual_view()==0,"actual left SDK evaluation remains native viewport 0");scope->slot=1;scope->entry+=0xa60;
    const auto before=calls();require(evaluate(sl::kFeatureDLSS,frame,inputs,5,command)==sl::Result::eOk && calls()==before+1 && actual_view()==C::VIEWPORT,"real intercepted right API evaluates exactly once on the private viewport");
    auto batch=h.sample(1);require(batch.events.size()==4 && batch.events[2].kind==H::Kind::constants && batch.events[2].scope.slot==1 && batch.events[2].viewport==C::VIEWPORT && batch.events[2].constants && batch.events[2].constants->reset==1 && batch.events[2].constants->position[0]==.064f,"actual private right constants are submitted and observed before its evaluation");
    require(batch.events[3].viewport==C::VIEWPORT && batch.events[3].options_current && batch.events[3].dlss_options && batch.events[3].dlss_options->viewport==C::VIEWPORT && batch.events[3].returned_frame_current && batch.events[3].returned_frame==batch.events[1].returned_frame,"right evaluation consumes its actual private options and the exact same returned native SDK frame owner");
    require(constants.frame_camera(42) && constants.frame_camera(42)->position.x==0 && constants.snapshot().latest->viewport==C::VIEWPORT && uint32_t(viewport)==0,"private camera does not overwrite the native main-camera cache or caller viewport");
    frame.value++;new_frame(live,nullptr);set_options(viewport,mode);cameras.counter++;for(auto& world:cameras.world)world[12]+=1;values.cameraPos.x=1;scope=H::Scope{2,8,20,42,0,0x200168,0x200000};
    set(values,frame,viewport);evaluate(sl::kFeatureDLSS,frame,inputs,5,command);scope->slot=1;scope->entry+=0xa60;evaluate(sl::kFeatureDLSS,frame,inputs,5,command);batch=h.sample(2);
    require(actual_view()==C::VIEWPORT && batch.events.size()==4 && batch.events[2].constants->reset==0 && std::abs(batch.events[2].constants->clip_to_previous[12]-1)<1e-5f,"real subsequent SDK call receives the accepted prior right pose rather than shared left history");
    // 23 Oct: a job whose tracked packet was refused recoverably has no tracked cameras. Its right eye
    // stays on the native SDK path, and the private eye resumes (with a reset) on the next job.
    static bool skipping{};h.skip_private([](uint64_t dispatch){return skipping && dispatch==5;});
    const auto job=[&](uint64_t dispatch){frame.value++;new_frame(live,nullptr);set_options(viewport,mode);cameras.counter++;scope=H::Scope{dispatch,10+dispatch,22+dispatch,uint32_t(44+dispatch),0,0x100168+dispatch*0x100000,0x100000+dispatch*0x100000};
        set(values,frame,viewport);evaluate(sl::kFeatureDLSS,frame,inputs,5,command);scope->slot=1;scope->entry+=0xa60;return evaluate(sl::kFeatureDLSS,frame,inputs,5,command);};
    skipping=true;const auto skipped_calls=calls();
    require(job(5)==sl::Result::eOk && calls()==skipped_calls+2 && actual_view()==0 && !h.private_accepted(5) && h.sample(5).events.size()==3,"a skipped job's right eye evaluates once on the native viewport, without private constants or a receipt");
    skipping=false;require(job(6)==sl::Result::eOk && actual_view()==C::VIEWPORT && h.private_accepted(6) && h.sample(6).events[2].constants && h.sample(6).events[2].constants->reset==1,"the next job uses the unpoisoned private eye again, from a history reset");
    scope.reset();const auto recorded=h.sample(2).events.size();evaluate(sl::kFeatureDLSS,frame,inputs,5,command);require(actual_view()==0 && h.sample(2).events.size()==recorded,"unscoped SDK calls preserve native inputs and never inherit the private eye");
    h.reset();options.reset();frame.value++;new_frame(live,nullptr);set_options(viewport,mode);cameras.counter++;scope=H::Scope{7,9,21,43,0,0x300168,0x300000};set(values,frame,viewport);evaluate(sl::kFeatureDLSS,frame,inputs,5,command);scope->slot=1;scope->entry+=0xa60;evaluate(sl::kFeatureDLSS,frame,inputs,5,command);require(actual_view()==0 && h.sample(7).events.size()==3,"reset poisons private reuse and forwards the unchanged native SDK call once");
    std::ostringstream json;H::write(json,h.sample(2));require(json.str().find("\"private_options_submitted\":true")!=std::string::npos && json.str().find("\"histories_verified\":false")!=std::string::npos,"actual SDK input separation never certifies native/GPU histories");
    std::cout<<"Actual Streamline private-eye detour integration passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
