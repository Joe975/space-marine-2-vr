#include "StreamlineHistoryProbe.h"
#include "StreamlineModule.h"
#include "PerformanceTrace.h"
#include "SceneExecutionTiming.h"
#include "NativeCallCensus.h"
#include <MinHook.h>
#include <array>
#include <set>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {
template<class T>bool read(const T* p,T& value){SIZE_T bytes{};const auto a=reinterpret_cast<uintptr_t>(p);return a && a<=UINTPTR_MAX-sizeof(T) && ReadProcessMemory(GetCurrentProcess(),p,&value,sizeof(T),&bytes) && bytes==sizeof(T);}
}
StreamlineHistoryProbe& StreamlineHistoryProbe::get(){static auto* value=new StreamlineHistoryProbe;return *value;}
bool StreamlineHistoryProbe::arm(Provider provider){std::lock_guard lock{m_install};if(m_armed || !provider)return false;m_provider=std::move(provider);m_armed=true;return true;}
void StreamlineHistoryProbe::poll(std::chrono::steady_clock::time_point now){
    std::lock_guard lock{m_install};if(!m_armed || m_installed || now<m_next)return;m_next=now+std::chrono::seconds(1);
    const auto module=streamline_module();if(!module)return;
    std::array<void*,4> targets{reinterpret_cast<void*>(GetProcAddress(module,"slEvaluateFeature")),reinterpret_cast<void*>(GetProcAddress(module,"slSetTag")),reinterpret_cast<void*>(GetProcAddress(module,"slSetTagForFrame")),reinterpret_cast<void*>(GetProcAddress(module,"slGetNewFrameToken"))};
    for(auto p:targets)if(!p)return;const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return;
    std::array<void*,4> detours{reinterpret_cast<void*>(&evaluate),reinterpret_cast<void*>(&tags),reinterpret_cast<void*>(&tags_for_frame),reinterpret_cast<void*>(&new_frame)},original{};
    size_t made{};for(;made<4;++made)if(MH_CreateHook(targets[made],detours[made],&original[made])!=MH_OK)break;
    if(made!=4){while(made)MH_RemoveHook(targets[--made]);return;}
    m_evaluate=reinterpret_cast<PFun_slEvaluateFeature*>(original[0]);m_tags=reinterpret_cast<SetTag*>(original[1]);m_tags_frame=reinterpret_cast<PFun_slSetTagForFrame*>(original[2]);m_new_frame=reinterpret_cast<PFun_slGetNewFrameToken*>(original[3]);
    size_t enabled{};for(;enabled<4;++enabled)if(MH_EnableHook(targets[enabled])!=MH_OK)break;
    if(enabled!=4){for(auto p:targets){MH_DisableHook(p);MH_RemoveHook(p);}m_evaluate=nullptr;m_tags=nullptr;m_tags_frame=nullptr;m_new_frame=nullptr;return;}m_installed=true;
}
std::optional<StreamlineHistoryProbe::Event> StreamlineHistoryProbe::begin(Kind kind,sl::CommandBuffer* command)const{
    try{if(!m_armed)return std::nullopt;const auto scope=m_provider();if(!scope || !scope->dispatch || !scope->render_id || !scope->revision || scope->slot>1 || !scope->entry || !scope->list)return std::nullopt;
        Event e;e.scope=*scope;e.kind=kind;e.thread=GetCurrentThreadId();e.command=reinterpret_cast<uintptr_t>(command);return e;
    }catch(...){return std::nullopt;}
}
void StreamlineHistoryProbe::finish(std::optional<Event> event,sl::Result result,const sl::FrameToken* frame){
    if(!event)return;try{event->result=result;if(frame){event->frame_after=uint32_t(*frame);const auto after=m_tokens.match(reinterpret_cast<uintptr_t>(frame),event->frame_after);event->returned_frame_current=event->returned_frame && after && *after==*event->returned_frame && event->frame_after==event->frame;}const auto current=m_provider();event->scope_current=current && *current==event->scope;
        std::lock_guard lock{m_mutex};auto found=m_batches.find(event->scope.dispatch);if(found==m_batches.end()){if(m_batches.size()==4){if(!m_continuous || event->scope.dispatch<=m_batches.rbegin()->first){++m_untracked;return;}m_batches.erase(m_batches.begin());}found=m_batches.emplace(event->scope.dispatch,Batch{}).first;}
        auto& batch=found->second;if(batch.events.size()==32){++batch.dropped;return;}batch.events.push_back(std::move(*event));
    }catch(...){}
}
void StreamlineHistoryProbe::bind(Event& event,const sl::FrameToken& frame)const{
    event.frame_object=reinterpret_cast<uintptr_t>(&frame);event.frame=uint32_t(frame);event.returned_frame=m_tokens.match(event.frame_object,event.frame);
}
std::function<void(sl::Result)> StreamlineHistoryProbe::constants_begin(const sl::Constants& values,const sl::FrameToken& frame,const sl::ViewportHandle& viewport){
    auto event=begin(Kind::constants,nullptr);if(!event)return {};
    try{
        bind(*event,frame);sl::BaseStructure header{{},0};sl::Constants copy;sl::ViewportHandle view;
        const auto prefix=reinterpret_cast<const uint8_t*>(&copy.minRelativeLinearDepthObjectSeparation)-reinterpret_cast<const uint8_t*>(&copy);
        SIZE_T bytes{};
        if(!read(static_cast<const sl::BaseStructure*>(&values),header) || header.structType!=sl::Constants::s_structType ||
            (header.structVersion!=sl::kStructVersion1 && header.structVersion!=sl::kStructVersion2) ||
            !read(&viewport,view) || view.structType!=sl::ViewportHandle::s_structType || view.structVersion!=sl::kStructVersion1){event->readable=false;}
        else {
            NativeCallCensus::get().count(NativeCallCensus::Hook::sdk_constants);if(!uint32_t(view))NativeCallCensus::get().count(NativeCallCensus::Hook::sdk_constants_viewport0);
            event->viewport=uint32_t(view);const auto size=header.structVersion==sl::kStructVersion1?size_t(prefix):sizeof(copy);
            if(!ReadProcessMemory(GetCurrentProcess(),&values,&copy,size,&bytes) || bytes!=size || copy.structType!=header.structType || copy.structVersion!=header.structVersion)event->readable=false;
            else {
                event->unknown_inputs=copy.next || view.next;
                Constants c;c.version=copy.structVersion;c.reset=uint32_t(copy.reset);c.depth_inverted=uint32_t(copy.depthInverted);
                std::memcpy(c.projection.data(),&copy.cameraViewToClip,sizeof(c.projection));std::memcpy(c.inverse_projection.data(),&copy.clipToCameraView,sizeof(c.inverse_projection));
                std::memcpy(c.clip_to_previous.data(),&copy.clipToPrevClip,sizeof(c.clip_to_previous));std::memcpy(c.previous_to_clip.data(),&copy.prevClipToClip,sizeof(c.previous_to_clip));
                c.position={copy.cameraPos.x,copy.cameraPos.y,copy.cameraPos.z};c.right={copy.cameraRight.x,copy.cameraRight.y,copy.cameraRight.z};
                c.up={copy.cameraUp.x,copy.cameraUp.y,copy.cameraUp.z};c.forward={copy.cameraFwd.x,copy.cameraFwd.y,copy.cameraFwd.z};
                c.jitter={copy.jitterOffset.x,copy.jitterOffset.y};c.motion_scale={copy.mvecScale.x,copy.mvecScale.y};
                c.near_plane=copy.cameraNear;c.far_plane=copy.cameraFar;c.fov=copy.cameraFOV;c.aspect=copy.cameraAspectRatio;
                const auto valid=[](float f){return std::isfinite(f) && f!=sl::INVALID_FLOAT;};
                const auto array_valid=[&](const auto& a){for(float f:a)if(!valid(f))return false;return true;};
                bool ok=array_valid(c.projection) && array_valid(c.inverse_projection) && array_valid(c.clip_to_previous) && array_valid(c.previous_to_clip) &&
                    array_valid(c.position) && array_valid(c.right) && array_valid(c.up) && array_valid(c.forward) && array_valid(c.jitter) && array_valid(c.motion_scale) &&
                    valid(c.near_plane) && valid(c.far_plane) && valid(c.fov) && valid(c.aspect);
                for(auto value:{copy.reset,copy.depthInverted,copy.cameraMotionIncluded,copy.motionVectors3D,copy.orthographicProjection,copy.motionVectorsDilated,copy.motionVectorsJittered})ok=ok && (value==sl::eFalse || value==sl::eTrue);
                if(ok){event->constants=c;if(!event->unknown_inputs)event->native_constants=copy;}else event->readable=false;
            }
        }
    }catch(...){event->readable=false;}
    // The completion runs synchronously inside the sole SetConstants detour.
    return [this,event=std::move(event),frame=&frame](sl::Result result)mutable{finish(std::move(event),result,frame);};
}
sl::Result StreamlineHistoryProbe::new_frame(sl::FrameToken*& frame,const uint32_t* index){
    auto& self=get();const auto result=self.m_new_frame.load()(frame,index);
    // A failed getter need not initialise its output reference.
    if(result!=sl::Result::eOk){self.m_tokens.returned(0,0,std::nullopt,GetCurrentThreadId(),false);return result;}
    try{uint32_t value{},observed_index{};std::optional<uint32_t> explicit_index;
        bool accepted=result==sl::Result::eOk && frame;if(index){accepted=read(index,observed_index) && accepted;explicit_index=observed_index;}
        if(accepted)value=uint32_t(*frame);self.m_tokens.returned(reinterpret_cast<uintptr_t>(frame),value,explicit_index,GetCurrentThreadId(),accepted);
    }catch(...){self.m_tokens.returned(0,0,std::nullopt,GetCurrentThreadId(),false);}return result;
}
void StreamlineHistoryProbe::parse_tags(Event& e,const sl::ResourceTag* tags,uint32_t count){
    if(count>16 || (count && !tags)){e.readable=false;return;}
    const auto base=reinterpret_cast<uintptr_t>(tags);if(count && base>UINTPTR_MAX-sizeof(sl::ResourceTag)*count){e.readable=false;return;}
    for(uint32_t i=0;i<count;++i){sl::ResourceTag tag;
        if(!read(reinterpret_cast<const sl::ResourceTag*>(base+i*sizeof(tag)),tag) || tag.structType!=sl::ResourceTag::s_structType || tag.structVersion!=sl::kStructVersion1){e.readable=false;return;}
        if(tag.next)e.unknown_inputs=true;sl::Resource resource;
        if(!read(tag.resource,resource) || resource.structType!=sl::Resource::s_structType || resource.structVersion!=sl::kStructVersion1){e.readable=false;return;}
        if(resource.next)e.unknown_inputs=true;
        if(e.tags.size()==16){e.readable=false;return;}
        e.tags.push_back({uint32_t(tag.type),uint32_t(tag.lifecycle),resource.state,uint32_t(resource.type),resource.width,resource.height,reinterpret_cast<uintptr_t>(resource.native),{tag.extent.top,tag.extent.left,tag.extent.width,tag.extent.height}});
    }
}
void StreamlineHistoryProbe::parse_inputs(Event& e,const sl::BaseStructure** inputs,uint32_t count){
    if(count>16 || (count && !inputs)){e.readable=false;return;}
    const auto base=reinterpret_cast<uintptr_t>(inputs);if(count && base>UINTPTR_MAX-sizeof(void*)*count){e.readable=false;return;}
    std::set<uintptr_t> visited;
    for(uint32_t i=0;i<count;++i){const sl::BaseStructure* next{};if(!read(reinterpret_cast<const sl::BaseStructure*const*>(base+i*sizeof(void*)),next)){e.readable=false;return;}
        while(next){const auto p=reinterpret_cast<uintptr_t>(next);if(visited.size()==32 || !visited.insert(p).second){e.readable=false;return;}
            sl::BaseStructure header{{},0};if(!read(next,header)){e.readable=false;return;}
            if(header.structType==sl::ViewportHandle::s_structType){sl::ViewportHandle view;
                if(header.structVersion!=sl::kStructVersion1 || !read(reinterpret_cast<const sl::ViewportHandle*>(next),view)){e.readable=false;return;}
                const auto value=uint32_t(view);if(e.viewport && *e.viewport!=value){e.readable=false;return;}e.viewport=value;
            }else if(header.structType==sl::ResourceTag::s_structType){parse_tags(e,reinterpret_cast<const sl::ResourceTag*>(next),1);if(!e.readable)return;}
            else e.unknown_inputs=true;
            next=header.next;
        }
    }
}
sl::Result StreamlineHistoryProbe::evaluate(sl::Feature feature,const sl::FrameToken& frame,const sl::BaseStructure** inputs,uint32_t count,sl::CommandBuffer* command){
    if(feature==sl::kFeatureDLSS && PerformanceTrace::get().probes())SceneExecutionTiming::sdk_entry(PerformanceTrace::now_ms());
    if(feature==sl::kFeatureDLSS)NativeCallCensus::get().count(NativeCallCensus::Hook::sdk_evaluate);
    PerformanceTrace::Scope timing{"ordinary_sdk_total_cpu"};
    auto& self=get();auto scope=self.begin(Kind::evaluate,command);
    const auto skipped=[&]{try{const auto skip=self.m_skip.load();return skip && skip(scope->scope.dispatch);}catch(...){return false;}};
    if(!self.m_private_requested || feature!=sl::kFeatureDLSS || !scope || scope->scope.slot!=1 || skipped())return self.evaluate_observed(feature,frame,inputs,count,command);
    std::optional<OrdinaryDlssContext::Request> request;
    try{const auto sdk=self.m_tokens.match(reinterpret_cast<uintptr_t>(&frame),uint32_t(frame));const auto cameras=self.m_cameras();const auto options=StreamlineOptionsProbe::get().sample(0);
        std::optional<Event> constants,left;
        {std::lock_guard lock{self.m_mutex};const auto batch=self.m_batches.find(scope->scope.dispatch);if(batch!=self.m_batches.end())for(const auto& e:batch->second.events)if(e.scope.slot==0){if(e.kind==Kind::constants){if(constants)throw std::runtime_error("duplicate constants");constants=e;}if(e.kind==Kind::evaluate && e.feature==sl::kFeatureDLSS){if(left)throw std::runtime_error("duplicate evaluation");left=e;}}}
        const auto matches=[&](const std::optional<Event>& e){return e && e->scope.dispatch==scope->scope.dispatch && e->scope.render_id==scope->scope.render_id && e->scope.revision==scope->scope.revision && e->scope.list==scope->scope.list && e->scope.token==scope->scope.token && e->scope_current && e->readable && !e->unknown_inputs && e->result==sl::Result::eOk && e->returned_frame_current && e->returned_frame==sdk;};
        if(sdk && cameras && options && matches(constants) && matches(left) && constants->native_constants && left->dlss_options && left->options_current && left->dlss_options->epoch==options->epoch && left->dlss_options->serial==options->serial && StreamlineOptionsProbe::get().current(*options)){
            uint32_t width{},height{};unsigned found{};for(const auto& tag:left->tags)if(tag.type==sl::kBufferTypeScalingInputColor){++found;if(!tag.width || !tag.height)break;
                if(tag.extent[2] || tag.extent[3]){if(!tag.extent[2] || !tag.extent[3] || uint64_t(tag.extent[1])+tag.extent[2]>tag.width || uint64_t(tag.extent[0])+tag.extent[3]>tag.height)break;width=tag.extent[2];height=tag.extent[3];}
                else if(!tag.extent[0] && !tag.extent[1]){width=tag.width;height=tag.height;}}
            if(found==1 && width && height)request=OrdinaryDlssContext::Request{scope->scope.dispatch,*sdk,*cameras,*constants->native_constants,options->values,width,height};
        }
    }catch(...){}
    const auto current=[&]{const auto now=self.m_provider();const auto sdk=self.m_tokens.match(reinterpret_cast<uintptr_t>(&frame),uint32_t(frame));const auto cameras=self.m_cameras();return request && now && *now==scope->scope && sdk && *sdk==request->sdk && cameras && *cameras==request->cameras;};
    return self.m_private.run(request,frame,inputs,count,current,[&](const sl::BaseStructure** actual,uint32_t actual_count){return self.evaluate_observed(feature,frame,actual,actual_count,command);});
}
sl::Result StreamlineHistoryProbe::evaluate_observed(sl::Feature feature,const sl::FrameToken& frame,const sl::BaseStructure** inputs,uint32_t count,sl::CommandBuffer* command){
    auto& self=get();auto e=self.begin(Kind::evaluate,command);try{if(e){self.bind(*e,frame);e->feature=uint32_t(feature);parse_inputs(*e,inputs,count);if(feature==sl::kFeatureDLSS && e->viewport)e->dlss_options=StreamlineOptionsProbe::get().sample(*e->viewport);}}catch(...){if(e)e->readable=false;}
    try{if(e && self.evaluation_boundary)self.evaluation_boundary(*e,false);}catch(...){}
    sl::Result result;{PerformanceTrace::Scope native_timing{"ordinary_sdk_native_cpu"};result=self.m_evaluate.load()(feature,frame,inputs,count,command);}
    try{if(e && result==sl::Result::eOk && self.evaluation_boundary)self.evaluation_boundary(*e,true);}catch(...){}
    try{if(e && e->dlss_options)e->options_current=StreamlineOptionsProbe::get().current(*e->dlss_options);}catch(...){}self.finish(std::move(e),result,&frame);return result;
}
sl::Result StreamlineHistoryProbe::tags(const sl::ViewportHandle& viewport,const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* command){
    auto& self=get();auto e=self.begin(Kind::tags,command);try{if(e){sl::ViewportHandle copied;if(!read(&viewport,copied) || copied.structType!=sl::ViewportHandle::s_structType || copied.structVersion!=sl::kStructVersion1)e->readable=false;else e->viewport=uint32_t(copied);parse_tags(*e,tags,count);}}catch(...){if(e)e->readable=false;}
    const auto result=self.m_tags.load()(viewport,tags,count,command);self.finish(std::move(e),result);return result;
}
sl::Result StreamlineHistoryProbe::tags_for_frame(const sl::FrameToken& frame,const sl::ViewportHandle& viewport,const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* command){
    auto& self=get();auto e=self.begin(Kind::tags_for_frame,command);try{if(e){self.bind(*e,frame);sl::ViewportHandle copied;if(!read(&viewport,copied) || copied.structType!=sl::ViewportHandle::s_structType || copied.structVersion!=sl::kStructVersion1)e->readable=false;else e->viewport=uint32_t(copied);parse_tags(*e,tags,count);}}catch(...){if(e)e->readable=false;}
    const auto result=self.m_tags_frame.load()(frame,viewport,tags,count,command);self.finish(std::move(e),result,&frame);return result;
}
StreamlineHistoryProbe::Batch StreamlineHistoryProbe::sample(uint64_t dispatch)const{std::lock_guard lock{m_mutex};const auto found=m_batches.find(dispatch);Batch value=found==m_batches.end()?Batch{}:found->second;value.installed=m_installed;value.dropped+=m_untracked;return value;}
void StreamlineHistoryProbe::write(std::ostream& out,const Batch& batch){
    const auto precision=out.precision(9);
    out<<"{\"installed\":"<<(batch.installed?"true":"false")<<",\"dropped\":"<<batch.dropped<<",\"resource_leases\":false,\"histories_verified\":false,\"private_context\":";if(get().private_requested())get().m_private.write(out);else out<<"null";out<<",\"options_observer\":";StreamlineOptionsProbe::get().write_status(out);out<<",\"events\":[";
    for(size_t i=0;i<batch.events.size();++i){if(i)out<<',';const auto& e=batch.events[i];const auto& s=e.scope;
        out<<"{\"kind\":"<<uint32_t(e.kind)<<",\"dispatch\":"<<s.dispatch<<",\"render_id\":"<<s.render_id<<",\"revision\":"<<s.revision<<",\"token\":"<<s.token<<",\"slot\":"<<s.slot<<",\"entry\":"<<s.entry<<",\"list\":"<<s.list<<",\"frame\":"<<e.frame<<",\"feature\":"<<e.feature<<",\"thread\":"<<e.thread<<",\"command\":"<<e.command<<",\"viewport\":";if(e.viewport)out<<*e.viewport;else out<<"null";
        out<<",\"frame_object\":"<<e.frame_object<<",\"frame_after\":"<<e.frame_after<<",\"returned_frame_current\":"<<(e.returned_frame_current?"true":"false")<<",\"returned_frame\":";
        if(!e.returned_frame)out<<"null";else {const auto& r=*e.returned_frame;out<<"{\"ordinal\":"<<r.ordinal<<",\"epoch\":"<<r.epoch<<",\"object\":"<<r.object<<",\"token\":"<<r.token<<",\"thread\":"<<r.thread<<",\"index\":";if(r.index)out<<*r.index;else out<<"null";out<<'}';}
        out<<",\"dlss_options\":";StreamlineOptionsProbe::write(out,e.dlss_options);out<<",\"options_current\":"<<(e.options_current?"true":"false");
        out<<",\"constants\":";
        if(!e.constants)out<<"null";else{const auto& c=*e.constants;const auto array=[&](const char* name,const auto& values){out<<",\""<<name<<"\":[";for(size_t j=0;j<values.size();++j){if(j)out<<',';out<<values[j];}out<<']';};
            out<<"{\"version\":"<<c.version<<",\"reset\":"<<c.reset<<",\"depth_inverted\":"<<c.depth_inverted<<",\"near\":"<<c.near_plane<<",\"far\":"<<c.far_plane<<",\"fov\":"<<c.fov<<",\"aspect\":"<<c.aspect;
            array("projection",c.projection);array("inverse_projection",c.inverse_projection);array("clip_to_previous",c.clip_to_previous);array("previous_to_clip",c.previous_to_clip);
            array("position",c.position);array("right",c.right);array("up",c.up);array("forward",c.forward);array("jitter",c.jitter);array("motion_scale",c.motion_scale);out<<'}';}
        out<<",\"readable\":"<<(e.readable?"true":"false")<<",\"scope_current\":"<<(e.scope_current?"true":"false")<<",\"unknown_inputs\":"<<(e.unknown_inputs?"true":"false")<<",\"result\":"<<int(e.result)<<",\"tags\":[";
        for(size_t j=0;j<e.tags.size();++j){if(j)out<<',';const auto& t=e.tags[j];out<<"{\"type\":"<<t.type<<",\"lifecycle\":"<<t.lifecycle<<",\"state\":"<<t.state<<",\"resource_type\":"<<t.resource_type<<",\"width\":"<<t.width<<",\"height\":"<<t.height<<",\"native\":"<<t.native<<",\"extent\":["<<t.extent[0]<<','<<t.extent[1]<<','<<t.extent[2]<<','<<t.extent[3]<<"]}";}out<<"]}";
    }out<<"]}";out.precision(precision);
}
