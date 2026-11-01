#include "OrdinaryDlssContext.h"
#include "VrProjection.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Frame:sl::FrameToken{uint32_t value{42};operator uint32_t()const override{return value;}};
OrdinaryDlssContext::Request request(Frame& frame,uint64_t ordinal=1){
    OrdinaryDlssContext::Request r;r.dispatch=ordinal;r.sdk={ordinal,1,reinterpret_cast<uintptr_t>(&frame),frame.value,1,std::nullopt};r.cameras.renderer=0x1000;
    for(unsigned eye=0;eye<2;++eye){r.cameras.world[eye]=vr_camera::IDENTITY;r.cameras.world[eye][10]=-1;r.cameras.world[eye][12]=eye*.064f;r.cameras.projection[eye]=r.cameras.inverse_projection[eye]=vr_camera::IDENTITY;}
    auto& c=r.source;c.structVersion=2;std::memcpy(&c.cameraViewToClip,vr_camera::IDENTITY.data(),64);std::memcpy(&c.clipToCameraView,vr_camera::IDENTITY.data(),64);
    c.cameraPos={0,0,0};c.cameraRight={1,0,0};c.cameraUp={0,1,0};c.cameraFwd={0,0,-1};c.jitterOffset={.25f,-.5f};c.mvecScale={.01f,-.02f};
    c.cameraNear=.1f;c.cameraFar=1000;c.cameraFOV=1;c.cameraAspectRatio=1.5f;c.reset=sl::eFalse;c.depthInverted=sl::eTrue;c.cameraMotionIncluded=sl::eTrue;c.motionVectors3D=sl::eFalse;
    r.cameras.projection[0][8]=2*c.jitterOffset.x/1920;r.cameras.projection[0][9]=-2*c.jitterOffset.y/1080;r.cameras.inverse_projection[0][8]=-r.cameras.projection[0][8];r.cameras.inverse_projection[0][9]=-r.cameras.projection[0][9];
    r.options.mode=sl::DLSSMode::eMaxPerformance;r.options.outputWidth=3840;r.options.outputHeight=2160;r.render_width=1920;r.render_height=1080;return r;
}
}
int main(int argc,char** argv){try{
    require(argc==2,"fixture required");using C=OrdinaryDlssContext;const auto dll=LoadLibraryA(argv[1]);require(dll,"fixture loads");
    const auto ready=reinterpret_cast<void(*)(bool)>(GetProcAddress(dll,"fixture_options_ready"));const auto resolve=reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(dll,"slGetFeatureFunction"));
    const auto set=reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(dll,"slSetConstants"));const auto evaluate=reinterpret_cast<PFun_slEvaluateFeature*>(GetProcAddress(dll,"slEvaluateFeature"));
    const auto calls=reinterpret_cast<unsigned(*)()>(GetProcAddress(dll,"fixture_history_calls"));const auto option_calls=reinterpret_cast<unsigned(*)()>(GetProcAddress(dll,"fixture_options_calls"));
    const auto constants_calls=reinterpret_cast<unsigned(*)()>(GetProcAddress(dll,"fixture_calls"));const auto constants_result=reinterpret_cast<void(*)(sl::Result)>(GetProcAddress(dll,"fixture_result"));const auto eval_result=reinterpret_cast<void(*)(sl::Result)>(GetProcAddress(dll,"fixture_history_result"));
    require(ready && resolve && set && evaluate && calls && option_calls && constants_calls && constants_result && eval_result,"actual SDK API fixture exports");ready(true);void* target{};resolve(sl::kFeatureDLSS,"slDLSSSetOptions",target);require(target,"native options resolver");const auto set_options=reinterpret_cast<PFun_slDLSSSetOptions*>(target);
    Frame frame;sl::ViewportHandle native_view{0u};std::array<sl::Resource,4> resources;std::array<sl::ResourceTag,4> tags;const std::array<uint32_t,4> types{0,1,3,4};
    const sl::BaseStructure* inputs[5]{&native_view};for(unsigned i=0;i<4;++i){resources[i]=sl::Resource{sl::ResourceType::eTex2d,reinterpret_cast<void*>(uintptr_t(0x10000+i)),128};resources[i].width=1920;resources[i].height=1080;tags[i]=sl::ResourceTag{&resources[i],types[i],sl::ResourceLifecycle::eValidUntilEvaluate};inputs[i+1]=&tags[i];}
    sl::Constants submitted;uint32_t actual_view{},constant_view{};bool scope_current=true;auto* command=reinterpret_cast<sl::CommandBuffer*>(0x20000);
    const auto invoke=[&](const sl::BaseStructure** actual,uint32_t count){actual_view=uint32_t(*static_cast<const sl::ViewportHandle*>(actual[0]));return evaluate(sl::kFeatureDLSS,frame,actual,count,command);};
    const auto bind=[&](C& context){context.arm([&](const auto& view,const auto& options){require(uint32_t(view)==C::VIEWPORT,"private options never overwrite native viewport 0");return set_options(view,options);},[&](const auto& values,const auto& token,const auto& view){submitted=values;constant_view=uint32_t(view);require(&token==&frame,"exact live SDK FrameToken reference preserved");return set(values,token,view);});};
    C context;bind(context);auto r=request(frame);auto before=calls();auto opts=option_calls();auto consts=constants_calls();
    require(context.run(r,frame,inputs,5,[&]{return scope_current;},invoke)==sl::Result::eOk && calls()==before+1 && option_calls()==opts+1 && constants_calls()==consts+1 && actual_view==C::VIEWPORT && constant_view==C::VIEWPORT,"first private context submits options/camera then the actual SDK evaluation once");
    require(submitted.reset==sl::eTrue && std::abs(submitted.cameraPos.x-.064f)<1e-6f && submitted.cameraFwd.z==-1 && submitted.jitterOffset.x==0 && submitted.jitterOffset.y==0 && submitted.mvecScale.y==-.02f && r.source.cameraPos.x==0 && uint32_t(native_view)==0,"right native pose/reset/actual unjittered raster are explicit while original main arguments remain unchanged");
    require(context.receipts()[0].raster_relation && !context.receipts()[0].jitter_parity && context.receipts()[0].render_width==1920 && context.receipts()[0].source_jitter[0]==.25f,"SDK/native jitter relation never relabels unequal eye raster jitter as parity");
    frame.value++;r.sdk.token=frame.value;r.sdk.ordinal++;r.dispatch++;r.cameras.counter++;r.source.cameraPos.x=1;for(auto& world:r.cameras.world)world[12]+=1;
    require(context.run(r,frame,inputs,5,[&]{return true;},invoke)==sl::Result::eOk && submitted.reset==sl::eFalse && std::abs(submitted.clipToPrevClip[3].x-1)<1e-5f && std::abs(submitted.prevClipToClip[3].x+1)<1e-5f && option_calls()==opts+1,"consecutive private eye uses its own prior right pose, not the preceding left pose, and retains unchanged context options");
    r.sdk.token=frame.value+=241;r.sdk.ordinal+=241;r.dispatch+=241;r.cameras.counter+=241;context.run(r,frame,inputs,5,[&]{return true;},invoke);require(submitted.reset==sl::eTrue && submitted.clipToPrevClip[3].x==0 && !context.receipts().back().continued,"sparse controls reset instead of pretending an intervening right-eye history exists");
    r.sdk.token=++frame.value;r.sdk.ordinal++;r.dispatch++;r.cameras.counter++;r.source.reset=sl::eTrue;context.run(r,frame,inputs,5,[&]{return true;},invoke);require(submitted.reset==sl::eTrue,"native camera cut resets private history");
    before=calls();consts=constants_calls();context.run(r,frame,inputs,5,[&]{return true;},invoke);require(calls()==before+1 && constants_calls()==consts && actual_view==0,"four-call bound refuses further setup but forwards the actual native evaluation once");
    {   // 25 Oct 18:30: a level load jumps the camera so far that the reprojection has no usable
        // inverse. That is a cut: reset the history, never poison the context (stereo stopped for good).
        // The game's reversed-depth projection (StreamlineProbe log, near 0.1 m), so the reprojection
        // carries the same 1/near scale as in the game.
        C jump;bind(jump);auto j=request(frame,500);
        const vr_camera::Matrix game{.77f,0,0,0, 0,.708f,0,0, 0,0,-4e-7f,1, 0,0,.1f,0};
        std::memcpy(&j.source.cameraViewToClip,game.data(),64);sl::matrixFullInvert(j.source.clipToCameraView,j.source.cameraViewToClip);
        for(unsigned eye=0;eye<2;++eye){auto p=game;if(!eye){p[8]=2*j.source.jitterOffset.x/1920;p[9]=-2*j.source.jitterOffset.y/1080;}j.cameras.projection[eye]=p;
            sl::float4x4 native,inverse;std::memcpy(&native,p.data(),64);sl::matrixFullInvert(inverse,native);std::memcpy(j.cameras.inverse_projection[eye].data(),&inverse,64);}
        const float c=std::cos(1.f),s=std::sin(1.f);
        const auto place=[&](float x,bool turned){for(unsigned eye=0;eye<2;++eye){auto& w=j.cameras.world[eye];w=vr_camera::IDENTITY;w[10]=-1;if(turned){w[0]=c;w[2]=-s;w[8]=-s;w[10]=-c;}w[12]=x;w[13]=0;w[14]=0;
                for(unsigned i=0;i<3;++i)w[12+i]+=eye*.064f*w[i];}
            const auto& l=j.cameras.world[0];j.source.cameraRight={l[0],l[1],l[2]};j.source.cameraUp={l[4],l[5],l[6]};j.source.cameraFwd={l[8],l[9],l[10]};j.source.cameraPos={l[12],l[13],l[14]};};
        place(0,false);jump.run(j,frame,inputs,5,[]{return true;},invoke);require(!jump.receipts().back().refusal,"the game-projection fixture is accepted");
        j.sdk.token=++frame.value;j.sdk.ordinal++;j.dispatch++;j.cameras.counter++;
        place(2000,true);
        before=calls();require(jump.run(j,frame,inputs,5,[]{return true;},invoke)==sl::Result::eOk && calls()==before+1,"the jumped job still evaluates once");
        const auto last=jump.receipts().back();
        require(!last.refusal && last.reset && !last.continued && submitted.reset==sl::eTrue && submitted.clipToPrevClip[3].x==0 && actual_view==C::VIEWPORT,"a camera jump with no usable reprojection resets the private history instead of refusing");
        j.sdk.token=++frame.value;j.sdk.ordinal++;j.dispatch++;j.cameras.counter++;place(2000.5f,true);
        jump.run(j,frame,inputs,5,[]{return true;},invoke);require(!jump.receipts().back().refusal && jump.receipts().back().continued,"the next job chains from the jumped one");
    }
    C wrapped;bind(wrapped);frame.value=UINT32_MAX;r=request(frame,100);r.cameras.counter=UINT32_MAX;wrapped.run(r,frame,inputs,5,[&]{return true;},invoke);frame.value=0;r.sdk.token=0;r.sdk.ordinal++;r.dispatch++;r.cameras.counter=0;wrapped.run(r,frame,inputs,5,[&]{return true;},invoke);require(submitted.reset==sl::eFalse && wrapped.receipts().back().continued,"actual SDK token/native-counter wrap preserves a strictly consecutive same-eye predecessor");
    const auto expect_refusal=[&](C& c,std::optional<C::Request> source,const C::Current& current){before=calls();consts=constants_calls();c.run(source,frame,inputs,5,current,invoke);require(calls()==before+1 && constants_calls()==consts && actual_view==0 && c.receipts().back().refusal,"preflight refusal never calls the private camera or retries native evaluation");};
    C missing;bind(missing);expect_refusal(missing,std::nullopt,[]{return true;});C stale;bind(stale);r=request(frame);expect_refusal(stale,r,[]{return false;});C invalid;bind(invalid);r=request(frame);r.cameras.world[1][12]=0;expect_refusal(invalid,r,[]{return true;});
    C changed;bind(changed);r=request(frame);changed.run(r,frame,inputs,5,[]{return true;},invoke);r.sdk.token=++frame.value;r.sdk.ordinal++;r.dispatch++;r.cameras.counter++;r.options.outputWidth=1920;
    {   // 25 Oct 19:07: a resolution change in the game's settings. The private viewport takes the new
        // options and starts a fresh history; it used to refuse (5) and poison, stopping stereo for good.
        const auto o=option_calls();before=calls();changed.run(r,frame,inputs,5,[]{return true;},invoke);const auto last=changed.receipts().back();
        require(!last.refusal && last.reset && !last.continued && option_calls()==o+1 && calls()==before+1 && actual_view==C::VIEWPORT && submitted.reset==sl::eTrue,"changed DLSS options are set again on the private viewport with a history reset");
        r.sdk.token=++frame.value;r.sdk.ordinal++;r.dispatch++;r.cameras.counter++;changed.run(r,frame,inputs,5,[]{return true;},invoke);
        require(!changed.receipts().back().refusal && changed.receipts().back().continued && option_calls()==o+1,"the next job chains on the new options without setting them again");
        r.sdk.token=++frame.value;r.sdk.ordinal++;r.dispatch++;r.cameras.counter++;r.cameras.renderer=0x2000;changed.run(r,frame,inputs,5,[]{return true;},invoke);
        require(!changed.receipts().back().refusal && changed.receipts().back().reset && option_calls()==o+2,"a new native renderer rebinds the same way");}
    C failed;bind(failed);r=request(frame);eval_result(sl::Result::eErrorMissingInputParameter);before=calls();require(failed.run(r,frame,inputs,5,[]{return true;},invoke)==sl::Result::eErrorMissingInputParameter && calls()==before+1 && actual_view==C::VIEWPORT && failed.receipts().back().refusal==11,"actual private evaluation failure is returned unchanged and never retried on viewport 0");eval_result(sl::Result::eOk);expect_refusal(failed,r,[]{return true;});
    C reset;bind(reset);r=request(frame);reset.run(r,frame,inputs,5,[]{return true;},invoke);reset.reset();expect_refusal(reset,r,[]{return true;});
    C rejected;bind(rejected);r=request(frame);constants_result(sl::Result::eErrorMissingInputParameter);before=calls();rejected.run(r,frame,inputs,5,[]{return true;},invoke);constants_result(sl::Result::eOk);require(calls()==before+1 && actual_view==0 && rejected.receipts().back().refusal==9,"failed actual private constants preserve one native evaluation and poison further attempts");
    {   // Cold restart (OrdinaryColdRestart): a poisoned context (refusal 9 above, then refusal 2 on every
        // later job, which the packet turns into its SDK refusal 9: stereo stopped for good) is cleared
        // as at first use. The next job sets the options again and starts a fresh history.
        require(rejected.poisoned(),"a refused constants call poisons the context");
        frame.value++;r=request(frame,700);expect_refusal(rejected,r,[]{return true;});require(rejected.receipts().back().refusal==2,"poisoned: every later job is refused");
        require(rejected.restart() && !rejected.poisoned() && rejected.restarts()==1,"restart clears the poison");
        frame.value++;r=request(frame,701);const auto o=option_calls();before=calls();rejected.run(r,frame,inputs,5,[]{return true;},invoke);const auto last=rejected.receipts().back();
        require(!last.refusal && last.reset && !last.continued && option_calls()==o+1 && calls()==before+1 && actual_view==C::VIEWPORT && submitted.reset==sl::eTrue,"after a restart the private eye sets its options again and resets its history");
        // The device-reset poison (reset()) clears the same way.
        require(reset.poisoned() && reset.restart(),"a device-reset poison is cleared by a restart");
        frame.value++;r=request(frame,702);reset.run(r,frame,inputs,5,[]{return true;},invoke);require(!reset.receipts().back().refusal && reset.receipts().back().reset,"accepted after the restart");
    }
    C jittered;bind(jittered);r=request(frame);r.cameras.projection[1][8]=2*.125f/1920;r.cameras.projection[1][9]=-2*(-.25f)/1080;r.cameras.inverse_projection[1][8]=-r.cameras.projection[1][8];r.cameras.inverse_projection[1][9]=-r.cameras.projection[1][9];jittered.run(r,frame,inputs,5,[]{return true;},invoke);require(std::abs(submitted.jitterOffset.x-.125f)<1e-5f && std::abs(submitted.jitterOffset.y+.25f)<1e-5f && submitted.cameraViewToClip[2].x==0,"private SDK matrix stays unjittered while jitter describes the actual native right raster");
    C sign;bind(sign);r=request(frame);r.source.jitterOffset.y=-r.source.jitterOffset.y;expect_refusal(sign,r,[]{return true;});C inverse;bind(inverse);r=request(frame);r.cameras.inverse_projection[1][0]=2;expect_refusal(inverse,r,[]{return true;});C lens;bind(lens);r=request(frame);r.cameras.projection[1][0]=2;expect_refusal(lens,r,[]{return true;});
    C missing_jitter_inverse;bind(missing_jitter_inverse);r=request(frame);r.cameras.inverse_projection[0][8]=0;expect_refusal(missing_jitter_inverse,r,[]{return true;});C different_input;bind(different_input);r=request(frame);r.render_width=960;expect_refusal(different_input,r,[]{return true;});
    r=request(frame,2);r.sdk.token=++frame.value;r.cameras.counter=1;jittered.run(r,frame,inputs,5,[]{return true;},invoke);require(submitted.reset==sl::eFalse && std::abs(submitted.clipToPrevClip[2].x)<1e-7f && std::abs(submitted.clipToPrevClip[2].y)<1e-7f,"own-eye previous projection is SDK jitter-free, never the prior jittered native matrix");
    C continuous;continuous.arm(set_options,[&](const auto& values,const auto& token,const auto& view){submitted=values;return set(values,token,view);},true);
    for(unsigned n=0;n<20;++n){frame.value=1000+n;r=request(frame,1000+n);r.cameras.counter=n;
        continuous.run(r,frame,inputs,5,[]{return true;},invoke);const auto records=continuous.receipts();
        require(actual_view==C::VIEWPORT && records.size()==std::min(size_t(n+1),size_t(4)) && records.back().dispatch==1000+n && !records.back().refusal && records.back().continued==bool(n),"continuous private DLSS survives receipt rollover with its exact previous eye");}
    expect_refusal(continuous,r,[]{return true;});
    C tracked;bind(tracked);r=request(frame,5000);C::Tracked provenance;provenance.dispatch=r.dispatch;provenance.world=r.cameras.world;provenance.world[1][12]=.071f;r.cameras.world=provenance.world;
    for(size_t eye=0;eye<2;++eye){require(vr_projection::build({eye?-.85f:-1.1f,eye?1.25f:1.f,-.8f,1.05f},r.source.cameraNear,r.source.cameraFar,provenance.projection[eye]),"tracked lens fixture");
        r.cameras.projection[eye]=provenance.projection[eye];r.cameras.projection[eye][8]+=2*r.source.jitterOffset.x/1920;r.cameras.projection[eye][9]-=2*r.source.jitterOffset.y/1080;
        sl::float4x4 native,inverse;std::memcpy(&native,r.cameras.projection[eye].data(),64);sl::matrixFullInvert(inverse,native);std::memcpy(r.cameras.inverse_projection[eye].data(),&inverse,64);}
    r.cameras.tracked=provenance;std::memcpy(&r.source.cameraViewToClip,provenance.projection[0].data(),64);sl::matrixFullInvert(r.source.clipToCameraView,r.source.cameraViewToClip);
    tracked.run(r,frame,inputs,5,[]{return true;},invoke);
    require(actual_view==C::VIEWPORT && !tracked.receipts().back().refusal && std::memcmp(&submitted.cameraViewToClip,provenance.projection[1].data(),64)==0 && submitted.cameraPos.x==.071f,"tracked context uses authenticated right lens and actual IPD independently of left");
    C wrong_tracked;bind(wrong_tracked);++r.cameras.tracked->dispatch;expect_refusal(wrong_tracked,r,[]{return true;});
    std::cout<<"Bounded and continuous ordinary private DLSS context and own-eye predecessor passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
