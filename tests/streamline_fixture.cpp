#include <Windows.h>
#include <atomic>
#include <sl.h>
#include <sl_pcl.h>
#include <sl_dlss.h>
#include <cstring>

namespace
{
    std::atomic<unsigned> calls{};
    std::atomic<sl::Result> result{sl::Result::eOk};
    std::atomic<const void*> last_values{}, last_frame{}, last_viewport{};
    std::atomic<sl::Boolean> last_reset{sl::eInvalid};
    std::atomic<bool> marker_ready{false};
    std::atomic<unsigned> marker_calls{};
    std::atomic<sl::Result> marker_result{sl::Result::eOk};
    std::atomic<const void*> marker_frame{};
    std::atomic<uint32_t> marker_value{};
    std::atomic<bool> options_ready{};std::atomic<unsigned> options_calls{};
    std::atomic<sl::Result> options_result{sl::Result::eOk};std::atomic<const void*> options_view{},options_values{};
    std::atomic<void(*)()> options_inside{};
    __declspec(noinline) sl::Result fixture_options(const sl::ViewportHandle& viewport,const sl::DLSSOptions& values){
        ++options_calls;options_view=&viewport;options_values=&values;if(const auto inside=options_inside.load())inside();return options_result;
    }

    __declspec(noinline) sl::Result fixture_marker(sl::PCLMarker marker, const sl::FrameToken& frame) {
        ++marker_calls;
        marker_frame = &frame;
        marker_value = static_cast<uint32_t>(marker);
        return marker_result.load();
    }
}

// Export the real API signature. This DLL is only placed in build/mod/fixtures.
extern "C" __declspec(dllexport) sl::Result slSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport)
{
    ++calls;
    last_values = &values;
    last_frame = &frame;
    last_viewport = &viewport;
    if (values.structType == sl::Constants::s_structType && values.structVersion >= sl::kStructVersion1) { last_reset = values.reset; }
    return result.load();
}
extern "C" __declspec(dllexport) void fixture_result(sl::Result value) { result = value; }
extern "C" __declspec(dllexport) unsigned fixture_calls() { return calls.load(); }
extern "C" __declspec(dllexport) sl::Boolean fixture_reset() { return last_reset; }
extern "C" __declspec(dllexport) bool fixture_arguments(const void* v, const void* f, const void* p)
{
    return last_values == v && last_frame == f && last_viewport == p;
}
extern "C" __declspec(dllexport) sl::Result slGetFeatureFunction(sl::Feature feature, const char* name, void*& function) {
    if(options_ready && feature==sl::kFeatureDLSS && name && std::strcmp(name,"slDLSSSetOptions")==0){function=reinterpret_cast<void*>(&fixture_options);return sl::Result::eOk;}
    if (marker_ready && feature == sl::kFeaturePCL && name && std::strcmp(name, "slPCLSetMarker") == 0) {
        function = reinterpret_cast<void*>(&fixture_marker);
        return sl::Result::eOk;
    }
    function = nullptr;
    return sl::Result::eErrorMissingInputParameter;
}
extern "C" __declspec(dllexport) void fixture_marker_ready(bool ready) { marker_ready = ready; }
extern "C" __declspec(dllexport) void fixture_options_ready(bool ready){options_ready=ready;}
extern "C" __declspec(dllexport) void fixture_options_result(sl::Result value){options_result=value;}
extern "C" __declspec(dllexport) unsigned fixture_options_calls(){return options_calls;}
extern "C" __declspec(dllexport) bool fixture_options_args(const void* view,const void* values){return options_view==view && options_values==values;}
extern "C" __declspec(dllexport) void fixture_options_inside(void(*inside)()){options_inside=inside;}
extern "C" __declspec(dllexport) void fixture_marker_result(sl::Result value) { marker_result = value; }
extern "C" __declspec(dllexport) unsigned fixture_marker_calls() { return marker_calls; }
extern "C" __declspec(dllexport) bool fixture_marker_arguments(uint32_t marker, const void* frame) { return marker_value == marker && marker_frame == frame; }

namespace {
    std::atomic<unsigned> history_calls{};
    std::atomic<sl::Result> history_result{sl::Result::eOk};
    std::atomic<uintptr_t> history_a{},history_b{},history_c{},history_d{},history_e{};
    std::atomic<sl::FrameToken*> returned_token{};std::atomic<unsigned> token_calls{};
    std::atomic<const void*> token_output{},token_index{};
    std::atomic<bool> inspect_inputs{};std::atomic<uint32_t> evaluated_viewport{UINT32_MAX};
    void history_args(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e){++history_calls;history_a=a;history_b=b;history_c=c;history_d=d;history_e=e;}
}
extern "C" __declspec(dllexport) sl::Result slEvaluateFeature(sl::Feature feature,const sl::FrameToken& frame,const sl::BaseStructure** inputs,uint32_t count,sl::CommandBuffer* command){
    if(inspect_inputs){for(uint32_t i=0;i<count;++i)for(auto* next=inputs[i];next;next=next->next)if(next->structType==sl::ViewportHandle::s_structType)evaluated_viewport=uint32_t(*static_cast<const sl::ViewportHandle*>(next));}
    history_args(feature,reinterpret_cast<uintptr_t>(&frame),reinterpret_cast<uintptr_t>(inputs),count,reinterpret_cast<uintptr_t>(command));return history_result;
}
extern "C" __declspec(dllexport) sl::Result slSetTag(const sl::ViewportHandle& viewport,const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* command){
    history_args(reinterpret_cast<uintptr_t>(&viewport),reinterpret_cast<uintptr_t>(tags),count,reinterpret_cast<uintptr_t>(command),0);return history_result;
}
extern "C" __declspec(dllexport) sl::Result slSetTagForFrame(const sl::FrameToken& frame,const sl::ViewportHandle& viewport,const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* command){
    history_args(reinterpret_cast<uintptr_t>(&frame),reinterpret_cast<uintptr_t>(&viewport),reinterpret_cast<uintptr_t>(tags),count,reinterpret_cast<uintptr_t>(command));return history_result;
}
extern "C" __declspec(dllexport) void fixture_history_result(sl::Result result){history_result=result;}
extern "C" __declspec(dllexport) unsigned fixture_history_calls(){return history_calls;}
extern "C" __declspec(dllexport) bool fixture_history_args(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e){return history_a==a && history_b==b && history_c==c && history_d==d && history_e==e;}
extern "C" __declspec(dllexport) sl::Result slGetNewFrameToken(sl::FrameToken*& token,const uint32_t* index){++token_calls;token_output=&token;token_index=index;token=returned_token;return history_result;}
extern "C" __declspec(dllexport) void fixture_history_token(sl::FrameToken* token){returned_token=token;}
extern "C" __declspec(dllexport) bool fixture_history_token_args(const void* output,const void* index,unsigned count){return token_output==output && token_index==index && token_calls==count;}
extern "C" __declspec(dllexport) void fixture_history_inspect(bool enabled){inspect_inputs=enabled;}
extern "C" __declspec(dllexport) uint32_t fixture_history_viewport(){return evaluated_viewport;}
