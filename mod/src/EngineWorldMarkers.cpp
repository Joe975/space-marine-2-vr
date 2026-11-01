#include "EngineWorldMarkers.h"
#include "FrameSyncProbe.h"
#include "VrWorldMarker.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <spdlog/spdlog.h>

namespace {
constexpr std::array<uintptr_t,9> READER_RVAS{0x322550,0x319580,0x319590,0x319900,0x31a080,0x31a0a0,0x31a0c0,0x318a00,0x3188a0};
constexpr std::array<game_build::Code,9> READER_BYTES{
 game_build::hashed(0x322550,14,"17b9005f69f69221d55d866ea5cf79bb90970217fa39e546a3bb1a108e323887"),
 game_build::hashed(0x319580,14,"c114c058d53c63df09bfb9f9891196b54db4ba1646d2104ca70c01a7bdb90eb3"),
 game_build::hashed(0x319590,14,"11935ca0b1875e50abaf5de6115b46590d83247f2a687fbc4c8c53866b65d998"),
 game_build::hashed(0x319900,14,"6e91797ce969801afd17478db105b3214b290d4088884dae2c46a8ef1b06daa1"),
 game_build::hashed(0x31a080,14,"9a442373385131ae5524f9c16fbea1b0521fb6c3730c9138eb3cb304059df36b"),
 game_build::hashed(0x31a0a0,14,"878ea534d6ade348358a826470cf08b9ec43851ce26a53537d2b8bb67b6d969a"),
 game_build::hashed(0x31a0c0,14,"efe4e8645387a4bcf0f34cbc70ef2df48d15f263dad138c444891e9b1bcbc90a"),
 game_build::hashed(0x318a00,14,"8269b9141a074d0d9306790e9a35796c9f22d798f3b63a5a66f5c797e7e83626"),
 game_build::hashed(0x3188a0,14,"31baa00e598345019fdd98ba8fc0aef202c1993914b86889fe92ca6b185d4861")};
// These verified wrappers read only object+0x20 and the listed camera fields. Their local
// carrier has no borrowed pointers returned to the engine; all outputs are copied values.
struct CameraCopy {
    std::array<uint8_t,0x28> object;
    alignas(16) std::array<uint8_t,0x2dc> camera;
    void fill(void* original,const EngineCameraProbe::UiEye& eye) {
        std::memcpy(object.data(),original,object.size());camera.fill(0);
        const auto pointer=camera.data();std::memcpy(object.data()+0x20,&pointer,sizeof(pointer));
        std::memcpy(camera.data(),eye.world.data(),sizeof(eye.world));
        std::memcpy(camera.data()+0x40,eye.view.data(),sizeof(eye.view));
        std::memcpy(camera.data()+0x80,&eye.near_z,4);std::memcpy(camera.data()+0x84,&eye.far_z,4);
        std::memcpy(camera.data()+0x2d0,eye.native_lens.data(),sizeof(eye.native_lens));
    }
};
}

EngineWorldMarkers& EngineWorldMarkers::get() { static auto* instance=new EngineWorldMarkers; return *instance; }
bool EngineWorldMarkers::matches_build(uint32_t timestamp,std::span<const uint8_t> bytes) {
    return game_build::code(timestamp,WORLD_VIEW_RVA,bytes,PROLOGUE);
}
bool EngineWorldMarkers::install_in_game(HMODULE game) {
    if(installed() && m_readers_installed) return true;
    if(m_reader_failed) return false; // Failed once: not again on every present, with an error line each time.
    const auto base=reinterpret_cast<const uint8_t*>(game);
    const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto stamp=nt->FileHeader.TimeDateStamp;const auto world_view=game_build::rva(stamp,WORLD_VIEW_RVA);
    if(!world_view || world_view+PROLOGUE.size()>nt->OptionalHeader.SizeOfImage ||
        !matches_build(stamp,{base+world_view,PROLOGUE.size()})) return false;
    std::array<void*,9> readers{};
    for(size_t i=0;i<readers.size();++i) {
        const auto reader=game_build::rva(stamp,READER_RVAS[i]);
        if(!reader || reader+14>nt->OptionalHeader.SizeOfImage || !game_build::code(stamp,READER_RVAS[i],{base+reader,READER_BYTES[i].size()},READER_BYTES[i])) return false;
        readers[i]=const_cast<uint8_t*>(base)+reader;
    }
    return install(const_cast<uint8_t*>(base)+world_view,reinterpret_cast<uintptr_t>(base)) && install_readers(readers);
}
bool EngineWorldMarkers::install_readers(const std::array<void*,9>& targets) {
    std::lock_guard lock{m_install_mutex};
    if(m_readers_installed) return true;
    if(std::ranges::any_of(targets,[](auto value){return !value;})) return false;
    const std::array<void*,9> callbacks{reinterpret_cast<void*>(&on_view_world),reinterpret_cast<void*>(&on_value<1>),
        reinterpret_cast<void*>(&on_value<2>),reinterpret_cast<void*>(&on_value<3>),reinterpret_cast<void*>(&on_value<4>),
        reinterpret_cast<void*>(&on_value<5>),reinterpret_cast<void*>(&on_value<6>),reinterpret_cast<void*>(&on_float<7>),reinterpret_cast<void*>(&on_float<8>)};
    size_t created{};auto status=MH_OK;
    for(;created<targets.size();++created) {
        void* original{};status=MH_CreateHook(targets[created],callbacks[created],&original);
        if(status!=MH_OK) break;
        m_reader_originals[created]=original;
    }
    if(status==MH_OK) for(auto target:targets) {status=MH_EnableHook(target);if(status!=MH_OK) break;}
    if(status!=MH_OK) {
        for(size_t i=0;i<created;++i) {MH_DisableHook(targets[i]);MH_RemoveHook(targets[i]);}
        m_reader_failed=true;m_enabled=false;spdlog::error("WorldMarkers: camera reader hooks failed: {}; correction disabled",MH_StatusToString(status));return false;
    }
    m_readers_installed=true;spdlog::info("WorldMarkers: nine camera value readers installed; scoped local camera copies only");return true;
}
bool EngineWorldMarkers::marker_caller(uintptr_t caller) const {
    static const std::array<uintptr_t,4> r{game_build::rva(0xbf3690),game_build::rva(0xbf4596),game_build::rva(0xbf45a0),game_build::rva(0xbf596b)};
    return (r[0] && caller>=r[0] && caller<r[1]) || (r[2] && caller>=r[2] && caller<r[3]) ||
        (m_fixture_caller && caller==m_fixture_caller);
}
std::optional<EngineCameraProbe::UiEye> EngineWorldMarkers::reader_eye(void* object,uintptr_t address) {
    const auto caller=address-m_module;
    bool allowed=marker_caller(caller);
    for(const auto& test:m_fixture_readers) allowed|=test && caller==test;
    if(!m_enabled || !allowed || !object || FrameSyncProbe::get().recent_out_of_band()) return std::nullopt;
    uintptr_t camera{};std::memcpy(&camera,static_cast<const uint8_t*>(object)+0x20,sizeof(camera));
    const auto frame=FrameSyncProbe::get().render_camera_frame();
    if(!frame) return std::nullopt;
    auto result=EngineCameraProbe::get().prepare_ui_eye(camera,*frame);
    const auto completed=FrameSyncProbe::get().render_camera_frame();
    if(!result || !completed || completed->render_id!=frame->render_id || !result->eye.valid_at(std::chrono::steady_clock::now())) return std::nullopt;
    return result;
}
template<size_t I> void EngineWorldMarkers::on_value(void* object,float* out) {
    auto& self=get();CameraCopy copy;void* selected=object;
    try {if(out) if(const auto eye=self.reader_eye(object,reinterpret_cast<uintptr_t>(_ReturnAddress()))) {copy.fill(object,*eye);selected=copy.object.data();++self.m_reader_overrides;}} catch(...) {}
    reinterpret_cast<void(*)(void*,float*)>(self.m_reader_originals[I].load())(selected,out);
}
template<size_t I> float EngineWorldMarkers::on_float(void* object) {
    auto& self=get();CameraCopy copy;void* selected=object;
    try {if(const auto eye=self.reader_eye(object,reinterpret_cast<uintptr_t>(_ReturnAddress()))) {copy.fill(object,*eye);selected=copy.object.data();++self.m_reader_overrides;}} catch(...) {}
    return reinterpret_cast<float(*)(void*)>(self.m_reader_originals[I].load())(selected);
}
void EngineWorldMarkers::on_view_world(void* object,const float* input,float* output) {
    auto& self=get();CameraCopy copy;void* selected=object;
    try {if(input && output) if(const auto eye=self.reader_eye(object,reinterpret_cast<uintptr_t>(_ReturnAddress()))) {copy.fill(object,*eye);selected=copy.object.data();++self.m_reader_overrides;}} catch(...) {}
    reinterpret_cast<WorldViewFn*>(self.m_reader_originals[0].load())(selected,input,output);
}
bool EngineWorldMarkers::install(void* target,uintptr_t module) {
    std::lock_guard lock{m_install_mutex};
    if(installed()) return true;
    m_module=module;
    auto status=MH_Initialize();
    if(status!=MH_OK && status!=MH_ERROR_ALREADY_INITIALIZED) return false;
    void* original{};
    status=MH_CreateHook(target,reinterpret_cast<void*>(&on_world_view),&original);
    if(status==MH_OK) {
        m_original.store(reinterpret_cast<WorldViewFn*>(original));
        status=MH_EnableHook(target);
        if(status!=MH_OK) MH_RemoveHook(target);
    }
    if(status!=MH_OK) { spdlog::error("WorldMarkers: hook failed: {}",MH_StatusToString(status)); return false; }
    m_installed=true;
    spdlog::info("WorldMarkers: native UI hook installed; correction requires enabled, exact frame/source binding");
    return true;
}
void EngineWorldMarkers::on_world_view(void* object,const float* input,float* output) {
    auto& probe=get();
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-probe.m_module;
    // Copy the input before the native call: the API permits input/output aliasing.
    std::array<float,3> world{};
    if(input) std::memcpy(world.data(),input,sizeof(world));
    probe.m_original.load()(object,input,output);
    try {
        Sample sample; sample.caller=caller;sample.thread=GetCurrentThreadId();sample.world=world;
        if(output) std::memcpy(sample.view.data(),output,sizeof(sample.view));
        sample.effective=sample.view;
        if(object) std::memcpy(&sample.camera,static_cast<const uint8_t*>(object)+0x20,sizeof(sample.camera));
        const auto frames=FrameSyncProbe::get().snapshot().clock;
        const auto local=FrameSyncProbe::get().current_camera_frame();
        const auto open=FrameSyncProbe::get().render_camera_frame();
        if(frames.simulation) { sample.simulation=frames.simulation->render_id;sample.simulation_phases=frames.simulation->phases; }
        if(frames.rendering) { sample.rendering=frames.rendering->render_id;sample.rendering_phases=frames.rendering->phases; }
        if(frames.presenting) sample.presenting=frames.presenting->render_id;
        if(local) sample.local=local->render_id;
        if(open) sample.open=open->render_id;
        const auto rendered=open?EngineCameraProbe::get().rendered_frame(open->render_id):std::nullopt;
        if(rendered && rendered->source==sample.camera) { sample.matching=rendered->vr_render_token;sample.vr=rendered->vr_pose_applied; }
        if(sample.camera) {
            const auto camera=reinterpret_cast<const uint8_t*>(sample.camera);
            std::memcpy(sample.rectangle.data(),camera+0x12c,sizeof(sample.rectangle));
            std::memcpy(&sample.near_z,camera+0x80,4);
            std::memcpy(&sample.fov_h,camera+0x2d0,4);std::memcpy(&sample.fov_v,camera+0x2d4,4);
        }
        const bool marker_caller=probe.marker_caller(caller);
        bool corrected=false;
        if(probe.m_enabled && marker_caller && input && output && open && !FrameSyncProbe::get().recent_out_of_band()) {
            const auto prepared=EngineCameraProbe::get().prepare_ui_eye(sample.camera,*open);
            const auto now=std::chrono::steady_clock::now();
            vr_marker::Point encoded{};
            if(prepared && prepared->source==sample.camera && !prepared->frame.invalid && prepared->frame.eye==open->eye &&
                prepared->eye.render_token==open->render_id && prepared->eye.eye==open->eye && prepared->eye.fov &&
                prepared->eye.valid_at(now) && now-prepared->eye.captured<=std::chrono::milliseconds(100) &&
                !FrameSyncProbe::get().recent_out_of_band(now) &&
                vr_marker::reproject(world,prepared->view,*prepared->eye.fov,prepared->near_z,prepared->native_rectangle,encoded)) {
                // A phase may finish while the UI job is computing. Never publish across frames.
                const auto completed=FrameSyncProbe::get().render_camera_frame();
                if(completed && completed->render_id==open->render_id && completed->eye==open->eye) {
                    std::memcpy(output,encoded.data(),sizeof(encoded));corrected=true;
                    sample.corrected=true;sample.effective=encoded;
                    sample.matching=open->render_id;sample.vr=true;sample.rectangle=prepared->native_rectangle;
                }
            }
        }
        std::lock_guard lock{probe.m_mutex};
        ++probe.m_snapshot.calls;
        if(corrected) ++probe.m_snapshot.corrected;
        else if(probe.m_enabled && marker_caller) ++probe.m_snapshot.unbound;
        auto found=std::ranges::find(probe.m_snapshot.callers,caller,&Sample::caller);
        if(found!=probe.m_snapshot.callers.end()) { sample.calls=found->calls+1;*found=sample; }
        else if(probe.m_snapshot.callers.size()<16) { sample.calls=1;probe.m_snapshot.callers.push_back(sample); }
        else ++probe.m_snapshot.overflow;
    } catch(...) {}
}
EngineWorldMarkers::Snapshot EngineWorldMarkers::snapshot() const { std::lock_guard lock{m_mutex};auto result=m_snapshot;result.readers_installed=m_readers_installed;result.reader_overrides=m_reader_overrides;result.enabled=m_enabled;return result; }
