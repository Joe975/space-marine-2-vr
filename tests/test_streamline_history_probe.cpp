#include "StreamlineHistoryProbe.h"
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Frame:sl::FrameToken {operator uint32_t()const override{return 42;}};
thread_local std::optional<StreamlineHistoryProbe::Scope> scope;
thread_local bool provider_throws{};
thread_local unsigned change_scope{};
uintptr_t ptr(const void* p){return reinterpret_cast<uintptr_t>(p);}
}
int main(int argc,char** argv){try{
    require(argc==2,"fixture DLL required");using H=StreamlineHistoryProbe;auto& probe=H::get();
    SdkFrameTokens ledger;ledger.returned(0x100,UINT32_MAX,UINT32_MAX,1,true);const auto first=ledger.match(0x100,UINT32_MAX);require(first && !ledger.match(0x101,UINT32_MAX),"only an actual returned object/value pair can bind");
    ledger.returned(0x100,UINT32_MAX,UINT32_MAX,1,true);require(ledger.match(0x100,UINT32_MAX)==first,"identical repeated successful returns preserve one logical frame identity");
    ledger.returned(0x100,0,0,1,true);require(!ledger.match(0x100,UINT32_MAX) && ledger.match(0x100,0),"reused object and uint32 wrap never inherit old frame identity");
    ledger.returned(0x200,0,0,2,true);require(!ledger.match(0x100,0) && !ledger.match(0x200,0),"same token on different returned objects is ambiguous");
    ledger.returned(0,0,std::nullopt,1,false);require(!ledger.match(0x100,0),"failed SDK returns invalidate retained observations");
    for(unsigned i=0;i<9;++i)ledger.returned(0x1000+i,10+i,std::nullopt,1,true);require(!ledger.match(0x1000,10) && ledger.match(0x1008,18),"only eight recent returned identities are retained");
    const auto now=std::chrono::steady_clock::now();probe.poll(now);require(!probe.sample(1).installed,"unarmed probe does not install");
    require(probe.arm([]()->std::optional<H::Scope>{if(provider_throws)throw std::runtime_error("provider");auto value=scope;if(change_scope && ++change_scope==3 && value)++value->revision;return value;}),"explicit bounded history observer arms");
    probe.poll(now);require(!probe.sample(1).installed,"late-loaded interposer is tolerated");
    const auto module=LoadLibraryA(argv[1]);require(module!=nullptr,"fixture loads");
    const auto evaluate=reinterpret_cast<PFun_slEvaluateFeature*>(GetProcAddress(module,"slEvaluateFeature"));
    using TagApi=sl::Result(const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*);
    const auto tags=reinterpret_cast<TagApi*>(GetProcAddress(module,"slSetTag"));
    const auto tags_frame=reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(module,"slSetTagForFrame"));
    const auto set_result=reinterpret_cast<void(*)(sl::Result)>(GetProcAddress(module,"fixture_history_result"));
    const auto count=reinterpret_cast<unsigned(*)()>(GetProcAddress(module,"fixture_history_calls"));
    const auto args=reinterpret_cast<bool(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t)>(GetProcAddress(module,"fixture_history_args"));
    const auto new_frame=reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(module,"slGetNewFrameToken"));
    const auto set_token=reinterpret_cast<void(*)(sl::FrameToken*)>(GetProcAddress(module,"fixture_history_token"));
    const auto token_args=reinterpret_cast<bool(*)(const void*,const void*,unsigned)>(GetProcAddress(module,"fixture_history_token_args"));
    require(evaluate && tags && tags_frame && set_result && count && args && new_frame && set_token && token_args,"all actual API signature fixture exports required");
    probe.poll(now+std::chrono::seconds(2));probe.poll(now+std::chrono::seconds(4));require(probe.sample(1).installed,"all three passive APIs installed once");
    const auto options_ready=reinterpret_cast<void(*)(bool)>(GetProcAddress(module,"fixture_options_ready"));
    const auto resolve=reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(module,"slGetFeatureFunction"));require(options_ready && resolve,"actual options fixture required");options_ready(true);
    auto& options_probe=StreamlineOptionsProbe::get();options_probe.enable();options_probe.poll(now);void* option_target{};resolve(sl::kFeatureDLSS,"slDLSSSetOptions",option_target);require(option_target,"DLSS options function resolves");
    const auto options_call=reinterpret_cast<PFun_slDLSSSetOptions*>(option_target);sl::DLSSOptions options;options.mode=sl::DLSSMode::eMaxPerformance;options.outputWidth=3840;options.outputHeight=2160;const sl::ViewportHandle options_view{0u};options_call(options_view,options);
    Frame frame;sl::ViewportHandle viewport{0u};const sl::BaseStructure* inputs[]{&viewport};auto* command=reinterpret_cast<sl::CommandBuffer*>(0x123400);
    set_token(&frame);sl::FrameToken* actual{};const uint32_t index=42;
    require(new_frame(actual,&index)==sl::Result::eOk && actual==&frame && token_args(&actual,&index,1),"actual SDK return observer preserves output reference, index pointer, result and original call count");
    scope=H::Scope{1,7,19,42,0,0x100168,0x100000};
    std::vector<std::pair<bool,uint64_t>> boundaries;probe.evaluation_boundary=[&](const auto&,bool after){boundaries.emplace_back(after,count());};
    require(evaluate(sl::kFeatureDLSS,frame,inputs,1,command)==sl::Result::eOk && count()==1 && args(sl::kFeatureDLSS,ptr(&frame),ptr(inputs),1,ptr(command)),"original evaluate receives exact references, input array, feature, count and command once");
    require(boundaries==std::vector<std::pair<bool,uint64_t>>{{false,0},{true,1}},"capture boundaries surround exactly the actual native evaluation");probe.evaluation_boundary={};
    auto batch=probe.sample(1);require(batch.events.size()==1 && batch.events[0].viewport==0 && batch.events[0].scope==*scope && batch.events[0].scope_current && batch.events[0].readable && !batch.events[0].unknown_inputs && batch.events[0].frame==42,"actual evaluation records its explicit native eye and viewport");
    require(batch.events[0].returned_frame_current && batch.events[0].returned_frame && batch.events[0].returned_frame->object==ptr(&frame) && batch.events[0].returned_frame->token==42 && batch.events[0].frame_after==42,"both sides of the actual evaluation match the observed successful SDK return identity");
    require(batch.events[0].dlss_options && batch.events[0].options_current && batch.events[0].dlss_options->values.outputWidth==3840,"actual evaluation is joined to the current successfully submitted native options");
    sl::Resource resource{sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x567800),0x80};resource.width=1920;resource.height=1080;
    sl::ResourceTag tag{&resource,sl::kBufferTypeScalingInputColor,sl::ResourceLifecycle::eValidUntilEvaluate};const auto original_tag=tag;const auto original_resource=resource;
    scope->slot=1;scope->entry+=0xa60;
    set_result(sl::Result::eErrorMissingInputParameter);
    require(tags(viewport,&tag,1,command)==sl::Result::eErrorMissingInputParameter && args(ptr(&viewport),ptr(&tag),1,ptr(command),0),"legacy tags result and exact references pass through unchanged");
    require(tags_frame(frame,viewport,&tag,1,command)==sl::Result::eErrorMissingInputParameter && args(ptr(&frame),ptr(&viewport),ptr(&tag),1,ptr(command)),"frame tags result and exact references pass through unchanged");
    batch=probe.sample(1);require(batch.events.size()==3 && batch.events[1].scope.slot==1 && batch.events[1].result==sl::Result::eErrorMissingInputParameter && batch.events[1].tags.size()==1 && batch.events[2].frame==42 && batch.events[2].tags[0].native==0x567800 && batch.events[2].tags[0].width==1920 && batch.events[2].tags[0].height==1080,"owned POD tags retain actual right-view identity without claiming a COM lease");
    require(std::memcmp(&tag,&original_tag,sizeof(tag))==0 && std::memcmp(&resource,&original_resource,sizeof(resource))==0,"observer never mutates native tag/resource arguments");
    set_result(sl::Result::eOk);
    scope->token=41;evaluate(sl::kFeatureDLSS,frame,inputs,1,command);require(probe.sample(1).events.back().scope.token==41 && probe.sample(1).events.back().returned_frame->token==42,"candidate camera clock and actual returned SDK identity remain separate without an offset rewrite");scope->token=42;
    set_result(sl::Result::eErrorMissingInputParameter);new_frame(actual,&index);set_result(sl::Result::eOk);evaluate(sl::kFeatureDLSS,frame,inputs,1,command);require(!probe.sample(1).events.back().returned_frame_current && !probe.sample(1).events.back().returned_frame,"failed actual SDK getter prevents binding a later evaluation to stale returns");new_frame(actual,&index);
    const auto prior_epoch=probe.sample(1).events.front().returned_frame->epoch;probe.reset();evaluate(sl::kFeatureDLSS,frame,inputs,1,command);require(!probe.sample(1).events.back().returned_frame,"device reset invalidates actual SDK return identities");new_frame(actual,&index);evaluate(sl::kFeatureDLSS,frame,inputs,1,command);require(probe.sample(1).events.back().returned_frame_current && probe.sample(1).events.back().returned_frame->epoch>prior_epoch,"same numeric token after reset requires a fresh observed SDK return epoch");
    change_scope=1;evaluate(sl::kFeatureDLSS,frame,inputs,1,command);change_scope=0;require(!probe.sample(1).events.back().scope_current,"an API call spanning a changed native producing scope is explicitly refused");
    viewport.structVersion=2;tags(viewport,&tag,1,command);require(!probe.sample(1).events.back().readable,"unsupported tag viewport version stays unknown");viewport.structVersion=1;
    const auto expect_bad=[&](const sl::BaseStructure** values,uint32_t n){const auto before=count();require(evaluate(sl::kFeatureDLSS,frame,values,n,command)==sl::Result::eOk && count()==before+1,"invalid observations still call the native API exactly once");require(!probe.sample(1).events.back().readable,"invalid observation cannot become readable evidence");};
    expect_bad(nullptr,1);expect_bad(inputs,17);
    viewport.next=&viewport;expect_bad(inputs,1);viewport.next=nullptr;
    sl::ViewportHandle different{1u};viewport.next=&different;expect_bad(inputs,1);viewport.next=nullptr;
    const sl::BaseStructure* local_tags[]{&viewport,&tag};evaluate(sl::kFeatureDLSS,frame,local_tags,2,command);require(probe.sample(1).events.back().tags[0].native==0x567800,"local evaluation tags are captured separately from global tags");
    sl::BaseStructure unknown{{0x1234,0,0,{}},1};unknown.next=&viewport;const sl::BaseStructure* extension[]{&unknown};evaluate(sl::kFeatureDLSS,frame,extension,1,command);require(probe.sample(1).events.back().unknown_inputs && probe.sample(1).events.back().viewport==0,"unknown chained inputs stay explicitly unknown while known viewport is observed");
    SYSTEM_INFO system{};GetSystemInfo(&system);auto* pages=static_cast<uint8_t*>(VirtualAlloc(nullptr,2*system.dwPageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));require(pages!=nullptr,"guard allocation");DWORD old{};require(VirtualProtect(pages+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&old)!=0,"guard page");
    sl::BaseStructure header{sl::ViewportHandle::s_structType,1};auto* short_view=pages+system.dwPageSize-sizeof(header);std::memcpy(short_view,&header,sizeof(header));const sl::BaseStructure* guarded[]{reinterpret_cast<const sl::BaseStructure*>(short_view)};expect_bad(guarded,1);
    tag.resource=reinterpret_cast<sl::Resource*>(pages+system.dwPageSize);tags(viewport,&tag,1,command);require(!probe.sample(1).events.back().readable,"unreadable tag resource is refused without a COM call or crash");tag.resource=&resource;VirtualFree(pages,0,MEM_RELEASE);
    sl::Constants constants;
    for(auto* matrix:{&constants.cameraViewToClip,&constants.clipToCameraView,&constants.clipToPrevClip,&constants.prevClipToClip})for(unsigned i=0;i<4;++i)(*matrix)[i]=sl::float4{float(i==0),float(i==1),float(i==2),float(i==3)};
    constants.cameraPos={.064f,2,3};constants.cameraRight={1,0,0};constants.cameraUp={0,1,0};constants.cameraFwd={0,0,1};constants.jitterOffset={0,0};constants.mvecScale={1,1};
    constants.cameraNear=.1f;constants.cameraFar=1000;constants.cameraFOV=1;constants.cameraAspectRatio=1.5f;
    constants.depthInverted=sl::eTrue;constants.reset=sl::eFalse;constants.cameraMotionIncluded=sl::eTrue;constants.motionVectors3D=sl::eFalse;
    const auto before_constants=probe.sample(1).events.size();auto complete=probe.constants_begin(constants,frame,viewport);
    require(complete && probe.sample(1).events.size()==before_constants,"constants observer is before/after and never records a guessed SDK result");complete(sl::Result::eOk);
    batch=probe.sample(1);const auto& observed=batch.events.back();require(observed.kind==H::Kind::constants && observed.constants && observed.readable && observed.scope_current && observed.returned_frame_current && observed.command==0 && observed.constants->position[0]==.064f && observed.constants->projection[5]==1 && observed.constants->reset==0,"actual submitted constants own POD camera values and the stable SDK frame without a fabricated command list");
    constants.cameraPos.x=99;require(probe.sample(1).events.back().constants->position[0]==.064f,"constants snapshot owns its values independently of later native edits");
    complete=probe.constants_begin(constants,frame,viewport);scope->revision++;complete(sl::Result::eErrorMissingInputParameter);scope->revision--;require(!probe.sample(1).events.back().scope_current && probe.sample(1).events.back().result==sl::Result::eErrorMissingInputParameter,"constants completion captures changed scope and exact failed result");
    constants.structVersion=3;probe.constants_begin(constants,frame,viewport)(sl::Result::eOk);require(!probe.sample(1).events.back().readable,"future constants layouts never become supported evidence");constants.structVersion=2;
    constants.cameraPos.x=sl::INVALID_FLOAT;probe.constants_begin(constants,frame,viewport)(sl::Result::eOk);require(!probe.sample(1).events.back().readable && !probe.sample(1).events.back().constants,"unset floats never become valid JSON camera evidence");constants.cameraPos.x=.064f;
    pages=static_cast<uint8_t*>(VirtualAlloc(nullptr,2*system.dwPageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));require(pages && VirtualProtect(pages+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&old),"constants guard pages");
    const auto prefix=reinterpret_cast<const uint8_t*>(&constants.minRelativeLinearDepthObjectSeparation)-reinterpret_cast<const uint8_t*>(&constants);constants.structVersion=1;
    auto* v1=reinterpret_cast<sl::Constants*>(pages+system.dwPageSize-prefix);std::memcpy(v1,&constants,prefix);
    probe.constants_begin(*v1,frame,viewport)(sl::Result::eOk);require(probe.sample(1).events.back().readable && probe.sample(1).events.back().constants->version==1,"V1 constants read only the supported prefix before an inaccessible V2 tail");
    v1->structVersion=2;probe.constants_begin(*v1,frame,viewport)(sl::Result::eOk);require(!probe.sample(1).events.back().readable,"truncated V2 constants are explicitly unreadable");VirtualFree(pages,0,MEM_RELEASE);
    std::ostringstream constants_json;H::write(constants_json,probe.sample(1));require(constants_json.str().find("\"position\":[0.064")!=std::string::npos && constants_json.str().find("\"constants\":null")!=std::string::npos,"versioned constants export keeps supported values and explicit unknowns");
    const auto recorded=probe.sample(1).events.size();scope.reset();require(!probe.constants_begin(constants,frame,viewport),"outside-Scene constants do not inherit the last eye");evaluate(sl::kFeatureDLSS,frame,inputs,1,command);provider_throws=true;evaluate(sl::kFeatureDLSS,frame,inputs,1,command);provider_throws=false;require(probe.sample(1).events.size()==recorded,"unscoped and failed-provider calls cannot inherit eye ownership");
    const auto before=count();std::thread a{[&]{scope=H::Scope{2,8,20,42,0,0x200168,0x200000};for(unsigned i=0;i<40;++i)evaluate(sl::kFeatureDLSS,frame,inputs,1,command);}};
    std::thread b{[&]{scope=H::Scope{3,9,21,42,1,0x300bc8,0x300000};for(unsigned i=0;i<20;++i)evaluate(sl::kFeatureDLSS,frame,inputs,1,command);}};a.join();b.join();
    require(count()==before+60 && probe.sample(2).events.size()==32 && probe.sample(2).dropped==8 && probe.sample(3).events.size()==20 && probe.sample(3).events.front().scope.slot==1,"thread-local native scopes remain separate and bounded overflow is explicit");
    scope=H::Scope{4,10,22,42,0,0x400168,0x400000};evaluate(sl::kFeatureDLSS,frame,inputs,1,command);scope->dispatch=5;evaluate(sl::kFeatureDLSS,frame,inputs,1,command);require(probe.sample(4).dropped==1 && probe.sample(5).events.empty(),"extra producing jobs cannot silently displace the four bounded controls");
    std::ostringstream out;H::write(out,probe.sample(1));require(out.str().find("\"resource_leases\":false")!=std::string::npos && out.str().find("\"histories_verified\":false")!=std::string::npos,"POD observations never certify independent histories");
    probe.continuous(true);
    for(uint64_t dispatch=6;dispatch<18;++dispatch){scope->dispatch=dispatch;evaluate(sl::kFeatureDLSS,frame,inputs,1,command);
        require(probe.sample(dispatch).events.size()==1 && probe.sample(dispatch).events.front().scope.dispatch==dispatch && (dispatch<10 || probe.sample(dispatch-4).events.empty()),"continuous observer keeps newest four jobs and exact current identity");}
    std::cout<<"Streamline history API observations passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}}
