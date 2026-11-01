#include "synthetic_code.h"
#include "EngineCameraProbe.h"
#include "game_build_ported.h"
#include "EngineProjectionHook.h"
#include "EngineWorldMarkers.h"
#include "VrWorldMarker.h"
#include "FrameSyncProbe.h"
#include "ImageCameraBinding.h"
#include <array>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
bool close(float a, float b) { return std::abs(a-b) < 1e-4f; }
float ndc(const vr_camera::Matrix& matrix, float x, float y, float z, int axis) {
    const auto value = x*matrix[axis] + y*matrix[4+axis] + z*matrix[8+axis] + matrix[12+axis];
    const auto w = x*matrix[3] + y*matrix[7] + z*matrix[11] + matrix[15];
    return value/w;
}
void test_math() {
    vr_camera::Matrix out{};
    const vr_projection::Tangents fov{-1.2f,0.8f,-0.7f,1.1f};
    require(vr_projection::build(fov,0.1f,1000,out), "asymmetric frustum must build");
    require(close(ndc(out,fov.left*2,0,2,0),-1) && close(ndc(out,fov.right*2,0,2,0),1), "left/right rays must reach their clip edges");
    require(close(ndc(out,0,fov.bottom*2,2,1),-1) && close(ndc(out,0,fov.top*2,2,1),1), "bottom/top rays must reach their clip edges including vertical asymmetry");
    require(close(ndc(out,0,0,0.1f,2),1) && close(ndc(out,0,0,1000,2),0), "game depth must map near to one and far to zero");
    const auto depth_a = ndc(out,0,0,1,2), depth_b = ndc(out,0,0,100,2);
    require(depth_a > depth_b && depth_a > 0 && depth_a < 1, "depth must decrease monotonically with distance");
    require(close(out[8],0.2f) && close(out[9],-0.2222222f) && out[11] == 1, "principal point and w=z must follow game row-vector convention");
    const auto before = out;
    require(!vr_projection::build({0,1,-1,1},0.1f,100,out) && out == before, "degenerate FOV must not mutate output");
    require(!vr_projection::build(fov,0,100,out) && !vr_projection::build(fov,1,1,out), "invalid clip planes must be refused");
    auto invalid = fov; invalid.top = std::numeric_limits<float>::quiet_NaN();
    require(!vr_projection::build(invalid,0.1f,100,out), "NaN FOV must be refused");
    require(!vr_projection::build(fov,0.1f,std::numeric_limits<float>::infinity(),out), "infinite far plane is outside finite engine contract");
    require(!vr_projection::build({-100,100,-100,100},1e38f,3e38f,out), "overflowing camera rectangle must be refused");
}

using BuildFn = void(*)(void*,float*,float,float);
using AddFn = int64_t(*)(void*,void*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
uintptr_t builder_caller{};
unsigned builder_calls{};
void* builder_camera{};
float builder_near{}, builder_far{};
vr_camera::Matrix rendered{}, helper{};
std::array<float,5> marker_point{};
std::array<uintptr_t,3> reader_callers{};
unsigned reader_calls{};
unsigned reader_invocations{};
float reader_fov{},reader_aspect{};
std::array<float,16> reader_world{},reader_view{};
template<size_t Offset,size_t Count> __declspec(noinline) void fake_value(void* object,float* out) {
    reader_callers[0]=reinterpret_cast<uintptr_t>(_ReturnAddress());++reader_calls;
    const auto camera=*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(object)+0x20);
    std::memcpy(out,camera+Offset,Count*sizeof(float));
}
template<size_t Offset> __declspec(noinline) float fake_float(void* object) {
    reader_callers[1]=reinterpret_cast<uintptr_t>(_ReturnAddress());++reader_calls;
    const auto camera=*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(object)+0x20);
    float result{};std::memcpy(&result,camera+Offset,4);return result;
}
__declspec(noinline) void fake_world(void* object,const float* point,float* out) {
    reader_callers[2]=reinterpret_cast<uintptr_t>(_ReturnAddress());++reader_calls;
    const auto camera=*reinterpret_cast<float**>(static_cast<uint8_t*>(object)+0x20);
    for(int i=0;i<3;++i) out[i]=point[0]*camera[i]+point[1]*camera[4+i]+point[2]*camera[8+i]+camera[12+i];
}
__declspec(noinline) void invoke_value(void(*volatile fn)(void*,float*),void* object,float* out) {fn(object,out);++reader_invocations;}
__declspec(noinline) float invoke_float(float(*volatile fn)(void*),void* object) {const auto result=fn(object);++reader_invocations;return result;}
__declspec(noinline) void invoke_world(void(*volatile fn)(void*,const float*,float*),void* object,const float* input,float* out) {fn(object,input,out);++reader_invocations;}
__declspec(noinline) void fake_ui_view(void* object,const float* input,float* output) {
    const auto camera=*reinterpret_cast<float**>(static_cast<uint8_t*>(object)+0x20);
    for(int i=0;i<3;++i) output[i]=input[0]*camera[16+i]+input[1]*camera[20+i]+input[2]*camera[24+i]+camera[28+i];
}
__declspec(noinline) void invoke_ui(void* source) {
    std::array<uint8_t,0x28> ui{};
    std::memcpy(ui.data()+0x20,&source,sizeof(source));
    invoke_value(&fake_value<0,16>,ui.data(),reader_world.data());
    invoke_value(&fake_value<0x40,16>,ui.data(),reader_view.data());
    std::array<float,3> temporary{};
    invoke_value(&fake_value<0x30,3>,ui.data(),temporary.data());
    invoke_value(&fake_value<0x20,3>,ui.data(),temporary.data());
    invoke_value(&fake_value<0,3>,ui.data(),temporary.data());
    invoke_value(&fake_value<0x10,3>,ui.data(),temporary.data());
    reader_fov=invoke_float(&fake_float<0x2d4>,ui.data());
    reader_aspect=invoke_float(&fake_float<0x2d8>,ui.data());
    const std::array<float,3> zero{};invoke_world(&fake_world,ui.data(),zero.data(),temporary.data());
    const std::array<float,3> point{0,0,2};
    marker_point={99,0,0,0,98};
    using UiFn=void(*)(void*,const float*,float*);
    UiFn volatile fn=&fake_ui_view;fn(ui.data(),point.data(),marker_point.data()+1);
}
std::array<uint8_t,EngineCameraProbe::SOURCE_CAMERA_SIZE> seen_camera{};
bool mutate_source{};

__declspec(noinline) void fake_builder(void* camera,float* out,float near_z,float far_z) {
    builder_caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    ++builder_calls; builder_camera = camera; builder_near = near_z; builder_far = far_z;
    if (out) { for (int i = 0; i < 16; ++i) { out[i] = float(-100-i); } }
}
__declspec(noinline) int other_builder(BuildFn volatile fn,void* camera) { fn(camera,helper.data(),0.2f,2000); return 1; }
__declspec(noinline) int64_t fake_add(void*,void* source,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) {
    invoke_ui(source);
    std::array<uint8_t,EngineCameraProbe::SOURCE_CAMERA_SIZE> temporary{};
    std::memcpy(temporary.data(),source,temporary.size());
    seen_camera = temporary;
    float near_z{},far_z{};
    std::memcpy(&near_z,temporary.data()+0x80,4); std::memcpy(&far_z,temporary.data()+0x84,4);
    BuildFn volatile build = &fake_builder;
    build(temporary.data(),rendered.data(),near_z,far_z);
    if (mutate_source) { static_cast<uint8_t*>(source)[0x300] ^= 1; }
    return 123;
}
__declspec(noinline) int64_t main_add(AddFn volatile fn,void* source) { return fn(nullptr,source,0,0,0,0,0,0,0,0,0)+1; }
__declspec(noinline) int64_t helper_add(AddFn volatile fn,void* source) { return fn(nullptr,source,0,0,0,0,0,0,0,0,0)+2; }
template<class T> void put(auto& camera,size_t offset,const T& value) { std::memcpy(camera.data()+offset,&value,sizeof(value)); }
float field(const auto& camera,size_t offset) { float value{}; std::memcpy(&value,camera.data()+offset,4); return value; }
}

int main(int argc,char** argv) {
    try {
        test_math();
        auto& projection = EngineProjectionHook::get();
        require(EngineProjectionHook::matches_build(EngineCameraProbe::BUILD_TIMESTAMP,SYNTH(EngineProjectionHook::PROLOGUE)), "known projection function must match build");
        auto bad = std::array<uint8_t,EngineProjectionHook::PROLOGUE.size()>{};
        require(!EngineProjectionHook::matches_build(EngineCameraProbe::BUILD_TIMESTAMP,bad) &&
            !EngineProjectionHook::matches_build(0,SYNTH(EngineProjectionHook::PROLOGUE)), "changed projection function/build must be refused");
        require(EngineProjectionHook::matches_build(game_build_ported::EXE,game_build_ported::code(EngineProjectionHook::BUILD_RVA)), "the 2026-10-21 projection builder must match");
        auto& camera_probe = EngineCameraProbe::get();
        const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        require(camera_probe.install(reinterpret_cast<void*>(&fake_add),module,0x10000000) == EngineCameraProbe::State::installed,"camera fixture hook must install");
        auto& markers=EngineWorldMarkers::get();
        require(markers.install(reinterpret_cast<void*>(&fake_ui_view),module),"actual UI fixture detour must install");markers.enable(true);
        std::array<uint8_t,EngineCameraProbe::SOURCE_CAMERA_SIZE> camera{};
        put(camera,0,vr_camera::IDENTITY); put(camera,0x40,vr_camera::IDENTITY);
        put(camera,0x80,0.1f); put(camera,0x84,1000.0f);
        put(camera,0x12c,std::array<float,4>{-0.1f,-0.1f,0.2f,0.2f});
        put(camera,0x2d0,std::array<float,3>{90,90,1});
        put(camera,0x8c,-0.1f);put(camera,0x90,-0.1f);put(camera,0x94,0.1f);put(camera,0xa8,0.1f);
        const auto before = camera;
        main_add(&fake_add,camera.data());
        markers.set_marker_return_rva(markers.snapshot().callers.back().caller);
        const auto reader_returns=reader_callers;
        require(markers.install_readers({reinterpret_cast<void*>(&fake_world),reinterpret_cast<void*>(&fake_value<0,16>),
            reinterpret_cast<void*>(&fake_value<0x40,16>),reinterpret_cast<void*>(&fake_value<0x30,3>),reinterpret_cast<void*>(&fake_value<0x20,3>),
            reinterpret_cast<void*>(&fake_value<0,3>),reinterpret_cast<void*>(&fake_value<0x10,3>),reinterpret_cast<void*>(&fake_float<0x2d8>),reinterpret_cast<void*>(&fake_float<0x2d4>)}),"all nine native UI value readers must install");
        markers.set_reader_return_rvas({reader_returns[0]-module,reader_returns[1]-module,reader_returns[2]-module});
        camera_probe.set_main_view_return_rva(camera_probe.snapshot().by_caller.back().caller_rva);
        const auto verified_builder_return = builder_caller-module;
        require(argc == 2, "Streamline fixture path required");
        const auto fixture = LoadLibraryA(argv[1]);
        require(fixture != nullptr, "frame fixture must load");
        const auto ready = reinterpret_cast<void(*)(bool)>(GetProcAddress(fixture,"fixture_marker_ready"));
        const auto resolve = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(fixture,"slGetFeatureFunction"));
        require(ready && resolve, "frame fixture APIs must exist");
        ready(true);
        void* marker_target{};
        require(resolve(sl::kFeaturePCL,"slPCLSetMarker",marker_target) == sl::Result::eOk, "frame marker must resolve");
        FrameSyncProbe::get().poll();
        const auto mark = reinterpret_cast<PFun_slPCLSetMarker*>(marker_target);
        struct Token : sl::FrameToken { uint32_t value{}; operator uint32_t() const override { return value; } } token;
        EngineCameraProbe::VrEye eye{};
        eye.relative_eye[12] = -0.032f;
        eye.render_token = 1;
        eye.captured = std::chrono::steady_clock::now();
        eye.fov = vr_projection::Tangents{-1.2f,0.8f,-0.7f,1.1f};
        require(camera_probe.publish_vr_eye(eye),"complete eye must publish");
        main_add(&fake_add,camera.data());
        require(field(seen_camera,0x30) == 0 && camera == before && rendered[0] == -100,
            "eye requesting projection must stay passive until projection hook is installed");
        require(projection.install(reinterpret_cast<void*>(&fake_builder),module) == EngineProjectionHook::State::installed,"projection fixture hook must install");
        projection.set_add_return_rva(verified_builder_return);
        camera_probe.clear_vr_eye();eye.captured=std::chrono::steady_clock::now();
        require(camera_probe.publish_vr_eye(eye),"refresh fixture pose after detour setup latency");
        token.value = 100;
        mark(sl::PCLMarker::eSimulationStart,token);
        token.value = 101;
        mark(sl::PCLMarker::eRenderSubmitStart,token);
        invoke_ui(camera.data());
        require(markers.snapshot().corrected==1 && camera==before,"UI job before main camera add must prepare its exact eye without writing the game camera");
        const auto calls_before = builder_calls;
        require(main_add(&fake_add,camera.data()) == 124,"original add result must pass through");
        const auto prepared=camera_probe.prepared_ui_eye(1);
        vr_marker::Point encoded{};
        require(prepared && vr_marker::reproject({0,0,2},prepared->view,*eye.fov,0.1f,{-0.1f,-0.1f,0.2f,0.2f},encoded) &&
            std::abs(marker_point[1]-encoded[0])<1e-5f && std::abs(marker_point[2]-encoded[1])<1e-5f && marker_point[3]==encoded[2] && marker_point[0]==99 && marker_point[4]==98,
            "UI jobs inside original camera add must use the prepublished exact eye pose and native rectangle without awaiting completed scene observation");
        require(markers.snapshot().corrected==2,"both early UI and UI inside native camera add must receive the same eye correction");
        require(markers.snapshot().callers.back().effective==encoded && markers.snapshot().callers.back().corrected,"projection telemetry must describe the actual bytes written to native UI output");
        require(reader_fov==90 && reader_aspect==1 && reader_world==prepared->world && reader_view==prepared->view,"3D marker camera readers must use canonical pose with native UI lens even while source lens temporarily becomes VR");
        const auto reader_before=reader_calls;invoke_ui(camera.data());
        require(reader_calls==reader_before+9 && reader_world==prepared->world && reader_view==prepared->view && camera==before,
            "native camera readers after source restoration must return canonical value copies exactly once without writing borrowed objects");
        require(builder_calls == calls_before+1 && builder_camera != camera.data() && builder_near == 0.1f && builder_far == 1000,
            "actual builder must receive copied camera and exact clip planes once");
        require(close(rendered[8],0.2f) && close(rendered[9],-0.2222222f),"main add must apply both asymmetric projection axes through real detour");
        require(close(field(seen_camera,0x30),-0.032f) && close(field(seen_camera,0x12c),-0.12f) && close(field(seen_camera,0x130),-0.07f),
            "submitted pose and near-plane rectangle must describe same eye");
        require(camera == before,"pose and lens fields must be restored byte-exact after submission");
        require(projection.snapshot().latest->binding.render_token == 1 && projection.snapshot().latest->binding.eye == 0,
            "projection must retain the same eye and render identity as pose");
        const auto camera_record = camera_probe.rendered_frame(1);
        require(camera_record && camera_record->game_frame->token == 101 && camera_record->projection && camera_record->vr_projection_applied,
            "camera record must retain simulation identity and actual projection output");
        other_builder(&fake_builder,camera.data());
        require(helper[0] == -100,"projection binding must end when main add ends");
        helper_add(&fake_add,camera.data());
        const auto additional_caller=camera_probe.snapshot().by_caller.back().caller_rva;
        camera_probe.set_additional_view_return_rva(additional_caller);
        helper_add(&fake_add,camera.data());
        const auto additional=camera_probe.additional_frame(1);
        require(additional && additional->game_frame->token==101 && additional->game_frame->render_id==camera_record->game_frame->render_id &&
            !additional->vr_pose_applied && camera_probe.rendered_frame(1).has_value(),"native additional camera must retain the same explicit job without replacing or invalidating the main-eye record");
        helper_add(&fake_add,camera.data());
        require(!camera_probe.additional_frame(1) && camera_probe.rendered_frame(1).has_value(),"ambiguous additional views rejected independently of main view");
        camera_probe.set_additional_view_return_rva(EngineCameraProbe::ADDITIONAL_VIEW_RETURN_RVA);
        {
            EngineProjectionHook::Scope scope{EngineProjectionHook::Binding{*eye.fov,0,1}};
            other_builder(&fake_builder,camera.data());
            require(helper[0] == -100,"even an active scope must reject other builder call sites");
            helper_add(&fake_add,camera.data());
            require(rendered[0] == -100 && camera == before,"nested helper add must mask outer eye scope");
        }
        eye.render_token = 2; eye.eye = 1; eye.fov = vr_projection::Tangents{-0.8f,1.2f,-1.1f,0.7f};
        eye.captured = std::chrono::steady_clock::now();
        require(camera_probe.publish_vr_eye(eye),"new right eye must publish");
        main_add(&fake_add,camera.data());
        require(!camera_probe.rendered_frame(1), "multiple main submissions with one frame identity must be rejected as ambiguous");
        require(close(rendered[8],-0.2f) && close(rendered[9],0.2222222f) && projection.snapshot().latest->binding.eye == 1,
            "right eye must have its own projection and identity");
        mutate_source = true;
        main_add(&fake_add,camera.data());
        auto changed = before; changed[0x300] ^= 1;
        require(camera == changed,"restoration must preserve unrelated writes made by original engine function");
        mutate_source = false;
        camera_probe.clear_vr_eye();
        main_add(&fake_add,camera.data());
        require(rendered[0] == -100,"tracking shutdown must remove projection override too");
        FrameSyncProbe::get().record(FrameSyncProbe::Kind::reset);
        token.value = 300; mark(sl::PCLMarker::eSimulationStart,token);
        token.value = 301; mark(sl::PCLMarker::eRenderSubmitStart,token);
        const auto owner = FrameSyncProbe::get().render_camera_frame();
        require(owner.has_value(), "provider fixture must have an open render interval");
        unsigned provider_calls{};
        camera_probe.set_eye_provider([&](const FrameSyncClock::Frame& frame) {
            ++provider_calls;
            auto provided = eye;
            provided.render_token = frame.render_id; provided.eye = frame.eye; provided.synthetic = true;
            provided.captured = std::chrono::steady_clock::now();
            return std::optional{provided};
        });
        invoke_ui(camera.data());
        const auto early=camera_probe.prepared_ui_eye(owner->render_id);
        main_add(&fake_add,camera.data());
        const auto provided = camera_probe.rendered_frame(owner->render_id);
        require(provider_calls == 1 && provided && provided->vr_synthetic && provided->vr_pose_applied && provided->vr_projection_applied,
            "provider must select pose and projection for explicit render owner");
        require(early && early->eye.captured==provided->pose_captured && std::equal(early->world.begin(),early->world.end(),provided->world),"early marker and scene must reuse the exact canonical world pose and capture timestamp");
        vr_camera::Matrix provided_world{};
        std::memcpy(provided_world.data(),provided->world,sizeof(provided_world));
        require(camera_probe.matching_camera(provided_world,*provided->projection).has_value(), "submitted world and projection must identify a unique retained camera");
        EyeTextureCapture::Metadata image{};
        image.world = provided_world; image.projection = *provided->projection; image.game_token = owner->token+1;
        auto presented = *owner; ++presented.token; ++presented.render_id; presented.eye ^= 1;
        require(bind_image_camera(image,presented,*provided,std::chrono::steady_clock::now()) && image.eye == owner->eye &&
            image.render_id == owner->render_id && !image.tracked_stereo, "synthetic image must inherit preceding rendered eye and never become tracked");
        auto tracked = *provided; tracked.vr_synthetic = false; tracked.tracking_epoch = 1;
        require(bind_image_camera(image,presented,tracked,std::chrono::steady_clock::now()) && image.tracked_stereo,
            "real tracked image must use matching preceding camera pose/projection identity");
        require(!bind_image_camera(image,*owner,tracked,std::chrono::steady_clock::now()), "same-frame camera is outside observed SM2 image contract");
        auto gap = presented; ++gap.token;
        require(!bind_image_camera(image,gap,tracked,std::chrono::steady_clock::now()), "token gaps must refuse image ownership");
        tracked.received -= std::chrono::seconds(1);
        require(!bind_image_camera(image,presented,tracked,std::chrono::steady_clock::now()), "stale camera cannot authorize compositor submission");
        auto different_projection = *provided->projection; different_projection[8] += 0.01f;
        require(!camera_probe.matching_camera(provided_world,different_projection), "pose-only match must not establish image provenance");
        helper_add(&fake_add,camera.data());
        require(provider_calls == 1 && rendered[0] == -100, "helper camera must not request a tracked pose");
        main_add(&fake_add,camera.data());
        require(!camera_probe.matching_camera(provided_world,*provided->projection), "duplicate camera submission must invalidate matching provenance");
        mark(sl::PCLMarker::eRenderSubmitEnd,token);
        main_add(&fake_add,camera.data());
        require(provider_calls == 1 && rendered[0] == -100, "closed render interval must not call provider or apply its previous pose; same-frame jobs share one pose");
        token.value = 301; mark(sl::PCLMarker::eSimulationStart,token);
        token.value = 302; mark(sl::PCLMarker::eRenderSubmitStart,token);
        camera_probe.set_eye_provider([&](const FrameSyncClock::Frame& frame) {
            auto wrong = eye; wrong.render_token = frame.render_id+1; wrong.eye = frame.eye;
            wrong.captured = std::chrono::steady_clock::now(); return std::optional{wrong};
        });
        main_add(&fake_add,camera.data());
        require(rendered[0] == -100, "provider with another frame identity must stay passive");
        FrameSyncProbe::get().record(FrameSyncProbe::Kind::reset);
        camera_probe.set_eye_provider([&](const FrameSyncClock::Frame& frame) {
            auto stationary = eye; stationary.render_token = frame.render_id; stationary.eye = frame.eye;
            stationary.tracking_epoch = 1; stationary.captured = std::chrono::steady_clock::now(); return std::optional{stationary};
        });
        for (uint32_t raw : {1000u,1001u}) {
            token.value = raw; mark(sl::PCLMarker::eSimulationStart,token);
            token.value = raw+1; mark(sl::PCLMarker::eRenderSubmitStart,token);
            const auto frame = *FrameSyncProbe::get().render_camera_frame();
            main_add(&fake_add,camera.data());
            const auto stationary = camera_probe.rendered_frame(frame.render_id);
            require(stationary.has_value(), "stationary jobs must retain unique explicit IDs");
            EyeTextureCapture::Metadata stationary_image{};
            std::memcpy(stationary_image.world.data(),stationary->world,sizeof(stationary_image.world));
            stationary_image.projection = *stationary->projection; stationary_image.game_token = frame.token+1;
            auto next_present = frame; ++next_present.token; ++next_present.render_id; next_present.eye ^= 1;
            require(bind_image_camera(stationary_image,next_present,*stationary,std::chrono::steady_clock::now()) && stationary_image.tracked_stereo,
                "stationary images must bind by explicit preceding ID despite repeated historical matrices");
            if (raw == 1001) { require(!camera_probe.matching_camera(stationary_image.world,stationary_image.projection),
                "repeated pose fixture must actually be ambiguous to global matrix search"); }
            mark(sl::PCLMarker::eRenderSubmitEnd,token);
        }
        camera_probe.set_eye_provider({});
        std::cout << "asymmetric projection geometry and camera/builder detours passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
